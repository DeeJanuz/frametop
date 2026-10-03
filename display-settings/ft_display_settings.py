#!/usr/bin/env python3
"""Frametop Display Settings: the Frametop screens' count, resolution, size, scale, and layout.

A Kirigami (QML) app with a Python backend, like Frametop Input Settings. It runs in
the dev container:
  - Screens (ft-screens backend): each screen's resolution (any, portrait too), its width
    in VR in metres, its scale, and which one has the taskbar. Resolution and width
    apply at once (ft-screens' control socket, @ft_screens); adding or removing a screen
    when the desktop starts again. (gamescope backend: one shared resolution, at most
    1920x1080 worth of pixels, rotation for portrait.)
  - Visibility (ft-screens): when the screens show (always, only with the SteamVR
    dashboard open, while you look at a controller, or only when toggled), the wrist
    angle within which a pinned screen shows, and pinning each screen to a wrist or
    your head.
  - Layout: a preset (curved or flat, rows, distance, gap, height) or a named layout
    saved from where the screens are, with a preview; arrange now; save the current
    arrangement under a name; rename and delete; arrange automatically when the
    desktop starts.
  - Power: how long the headset can go unused before ft-powerd turns its displays off
    (DISPLAY_OFF_MIN; the service's state comes from its control socket, @ft_powerd),
    and whether the Frame stays awake while plugged in, which is Steam's own setting
    (steam_settings.py; the value from before is kept as STEAM_SLEEP_AC_BEFORE).
Settings go to ~/.config/frametop.conf and ~/.config/frametop-layout.json. Anything
that touches SteamVR runs layout/ft-layout on the host.
Launch with display-settings/ft-display-settings (host wrapper).
"""
import os
import shutil
import socket
import sys
import threading

from PySide6.QtCore import Property, QObject, QProcess, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QGuiApplication, QIcon
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT_DIR = os.path.join(HERE, "..", "layout")
sys.path.insert(0, LAYOUT_DIR)
import ft_layout  # noqa: E402  (pure Python: the same geometry ft-layout uses)
import steam_settings  # noqa: E402

FT_LAYOUT = os.path.join(LAYOUT_DIR, "ft-layout")
DESKTOPS = os.path.join(HERE, "..", "desktops.sh")
CONF_PATH = ft_layout.CONF_PATH
# gamescope's VR backend uploads a texture the size of a screen at start, through a
# 1920x1080x4-byte buffer: more pixels than 1920x1080 abort it (see the design notes).
MAX_PIXELS = 1920 * 1080
RESOLUTIONS = [(1280, 720), (1600, 900), (1920, 1080), (1728, 1080), (1920, 800), (2224, 928), (2560, 800)]
# ft-screens has no pixel limit; portrait screens are just tall.
SCREEN_RESOLUTIONS = [(1920, 1080, ""), (2560, 1440, ""), (3840, 2160, "4K"), (2560, 1080, "ultrawide"),
                      (3440, 1440, "ultrawide"), (5120, 1440, "super ultrawide"), (1920, 1200, "16:10"),
                      (2560, 1600, "16:10"), (1080, 1920, "portrait"), (1440, 2560, "portrait"),
                      (2160, 3840, "portrait 4K")]
FT_SCREENS = "\0ft_screens"
FT_POWERD = "\0ft_powerd"
# Steam's default for "When Plugged In and Idle -> Sleep after", to go back to when
# nothing was saved.
STEAM_SLEEP_AC_DEFAULT = 3600
SCALES = [0.75, 1.0, 1.25, 4 / 3, 1.5, 1.75, 2.0]
ROTATIONS = [("normal", "Landscape"), ("left", "Portrait"), ("right", "Portrait (flipped)")]


def write_conf_value(key, value):
    """Set KEY=value in frametop.conf, keeping comments and the rest of the file."""
    try:
        with open(CONF_PATH) as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    for i, line in enumerate(lines):
        if line.split("#", 1)[0].strip().startswith(f"{key}="):
            comment = line[line.index("#"):] if "#" in line else ""
            lines[i] = f"{key}={value}" + (f"   {comment}" if comment else "")
            break
    else:
        lines.append(f"{key}={value}")
    with open(CONF_PATH, "w") as f:
        f.write("\n".join(lines) + "\n")


def host_command(*cmd):
    """argv to run a command on the SteamOS host (we live in the dev container).

    distrobox-host-exec reaches the host through the user's real session bus; inside the
    desktop our DBUS_SESSION_BUS_ADDRESS is the nested session's private one, where it
    fails (exit 127, silently). On the host (the Nix packages) the command runs directly, but
    systemd-run --user finds the user manager through XDG_RUNTIME_DIR, which the desktop's
    session points at its own, so both go back to the real session's."""
    runtime = f"/run/user/{os.getuid()}"
    bus = f"unix:path={runtime}/bus"
    if not shutil.which("distrobox-host-exec"):
        return ["env", f"XDG_RUNTIME_DIR={runtime}", f"DBUS_SESSION_BUS_ADDRESS={bus}"] + list(cmd)
    return ["env", f"DBUS_SESSION_BUS_ADDRESS={bus}", "distrobox-host-exec"] + list(cmd)


class Backend(QObject):
    changed = Signal()
    busyChanged = Signal()
    powerChanged = Signal()
    message = Signal(str, bool)  # text, is error
    _steamDone = Signal(object, object, str)  # Steam's sleep settings or None, error or None, what was done

    def __init__(self):
        super().__init__()
        self._busy = ""
        self._proc = None
        self._running = False
        self._running_count = 0
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._sock.bind("")  # an abstract address ft-screens can reply to
        self._sock.settimeout(1.0)
        self._started = {}  # conf values the running desktop started with
        self._psock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._psock.bind("")  # for ft-powerd's replies
        self._psock.settimeout(0.5)
        self._powerd = None  # ft-powerd's status: (state, seconds unused, timeout seconds); None: not running
        self._steam = None  # Steam's sleep settings: {"ac": seconds, "battery": seconds}
        self._steam_error = ""
        self._steam_busy = False
        self._steamDone.connect(self._steam_done, Qt.QueuedConnection)
        self._pins = []     # each running screen's pin: none | left | right | head
        self.poll = QTimer(interval=3000, timeout=self._check_running)
        self.poll.start()
        self._check_running()

    # --- state ---
    def _conf(self):
        conf = ft_layout.read_conf()
        def num(key, default, cast=float):
            try:
                return cast(conf.get(key, default))
            except ValueError:
                return default
        return {"screens": num("SCREENS", 2, int), "width": num("WIDTH", 1920, int),
                "height": num("HEIGHT", 1080, int), "physWidth": num("PHYS_WIDTH", 1.6)}

    def _check_running(self):
        # The session's own Wayland socket (host processes' environments aren't readable
        # from the container, so ft_layout.nested_env() doesn't work here).
        running = os.path.exists(f"/run/user/{os.getuid()}/frametop/wayland-0")
        count = self._screens_running() if running and ft_layout.backend() == "screens" else 0
        pins = self._read_pins(count)
        if running != self._running or count != self._running_count or pins != self._pins:
            if running != self._running or count != self._running_count:
                self._started = self._conf() if running else {}
            self._running = running
            self._running_count = count
            self._pins = pins
            self.changed.emit()
        self._check_powerd()

    def _read_pins(self, count):
        pins = []
        for i in range(count):
            reply = self._ask_screens(f"get {i + 1}")
            f = reply.split() if reply and reply.startswith("ok") else []
            pins.append(f[16] if len(f) > 16 else "none")
        return pins

    def _ask_screens(self, text):
        """Request/reply to ft-screens; None if it isn't running."""
        try:
            self._sock.sendto(text.encode(), FT_SCREENS)
            return self._sock.recv(4096).decode()
        except OSError:
            return None

    def _screens_running(self):
        reply = self._ask_screens("screens")
        return int(reply.split()[1]) if reply and reply.startswith("ok") else 0

    @Property(bool, notify=changed)
    def desktopRunning(self):
        return self._running

    @Property(str, notify=changed)
    def backend(self):
        return ft_layout.backend()

    @Property(bool, notify=changed)
    def restartNeeded(self):
        """ft-screens: screens added or removed since the desktop started; gamescope: count,
        resolution, or width changed."""
        if not self._running:
            return False
        if ft_layout.backend() == "screens":
            return self._running_count != ft_layout.screen_count(ft_layout.load_layout())
        return bool(self._started) and self._started != self._conf()

    @Property("QVariantList", constant=True)
    def screenResolutions(self):
        return [{"text": f"{w} × {h}" + (f"  ({t})" if t else ""), "width": w, "height": h}
                for w, h, t in SCREEN_RESOLUTIONS]

    @Property(int, notify=changed)
    def screens(self):
        return self._conf()["screens"]

    @Property(int, notify=changed)
    def width(self):
        return self._conf()["width"]

    @Property(int, notify=changed)
    def height(self):
        return self._conf()["height"]

    @Property(float, notify=changed)
    def physWidth(self):
        return self._conf()["physWidth"]

    @Property("QVariantList", constant=True)
    def resolutions(self):
        return [{"text": f"{w} × {h}", "width": w, "height": h} for w, h in RESOLUTIONS]

    @Property("QVariantList", constant=True)
    def scales(self):
        return [{"text": f"{round(s * 100)}%", "value": round(s, 4)} for s in SCALES]

    @Property("QVariantList", constant=True)
    def rotations(self):
        return [{"text": t, "value": v} for v, t in ROTATIONS]

    @Property("QVariantList", notify=changed)
    def screenList(self):
        c, layout = self._conf(), ft_layout.load_layout()
        out = []
        if ft_layout.backend() == "screens":
            primary = ft_layout.primary_screen(layout)
            for i in range(ft_layout.screen_count(layout)):
                w, h = ft_layout.screen_pixels(layout, i)
                s = ft_layout.screen_scale(layout, i)
                out.append({"index": i, "width": w, "height": h, "metres": ft_layout.screen_metres(layout, i),
                            "scale": round(s, 4), "primary": i == primary,
                            "curved": float(ft_layout.screen_entry(layout, i).get("curve", 0)) > 0,
                            "effective": f"{round(w / s)} × {round(h / s)}"})
            return out
        for i in range(c["screens"]):
            s = ft_layout.screen_scale(layout, i)
            rot = ft_layout.screen_rotation(layout, i)
            w, h = (c["height"], c["width"]) if rot != "normal" else (c["width"], c["height"])
            out.append({"index": i, "scale": round(s, 4), "rotation": rot,
                        "effective": f"{round(w / s)} × {round(h / s)}"})
        return out

    @Property("QVariantMap", notify=changed)
    def layout(self):
        return ft_layout.load_layout()

    @Property("QVariantList", notify=changed)
    def plan(self):
        """The arrangement in the head frame, for the preview."""
        layout = ft_layout.load_layout()
        c = self._conf()
        # Before any panel was measured: the width SteamVR floats a panel at, and the aspect.
        if "panel_size" not in layout or layout["panel_size"] == list(ft_layout.DEFAULT_PANEL):
            layout["panel_size"] = [ft_layout.DEFAULT_PANEL[0], ft_layout.DEFAULT_PANEL[0] * c["height"] / c["width"]]
        out = []
        for i, t in enumerate(ft_layout.plan(layout, c["screens"])):
            w, h = ft_layout.screen_size(layout, i)
            out.append({"index": i, "x": t["pos"][0], "y": t["pos"][1], "z": t["pos"][2],
                        "faceYaw": t["face"][0], "facePitch": t["face"][1], "width": w, "height": h})
        return out

    @Property(str, notify=busyChanged)
    def busy(self):
        return self._busy

    # --- screens ---
    @Slot(int)
    def setScreens(self, n):
        write_conf_value("SCREENS", str(max(1, min(6, n))))
        self.changed.emit()

    @Slot(int, int)
    def setResolution(self, w, h):
        if w * h > MAX_PIXELS:
            self.message.emit(f"{w} × {h} is more than gamescope's VR mode can draw (1920 × 1080 worth of pixels, "
                              f"about {MAX_PIXELS // 1000} thousand); try {round((MAX_PIXELS * w / h) ** 0.5) // 8 * 8} × "
                              f"{round((MAX_PIXELS * h / w) ** 0.5) // 8 * 8}", True)
            return
        if w >= 640 and h >= 360:
            write_conf_value("WIDTH", str(w))
            write_conf_value("HEIGHT", str(h))
            self.changed.emit()

    @Slot(float)
    def setPhysWidth(self, w):
        write_conf_value("PHYS_WIDTH", f"{w:.2f}")
        self.changed.emit()

    @Slot(int, float)
    def setScale(self, i, s):
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= i:
            screens.append({})
        screens[i]["scale"] = s
        ft_layout.save_layout(layout)
        self.changed.emit()
        if self._running:
            self._run("Applying scale", "scale")

    @Slot(int, str)
    def setRotation(self, i, rotation):
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= i:
            screens.append({})
        screens[i]["rotation"] = rotation
        screens[i].pop("roll", None)  # a saved arrangement follows the new rotation
        ft_layout.save_layout(layout)
        self.changed.emit()
        if self._running:
            self._run("Rotating", "scale")

    # --- ft-screens: per-screen resolution and size ---
    def _edit_screen(self, i, fn):
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= i:
            screens.append({"size": [1920, 1080], "metres": 1920 / ft_layout.PIXELS_PER_METRE})
        fn(screens[i])
        ft_layout.save_layout(layout)
        self.changed.emit()

    @Slot(int, int, int)
    def setScreenSize(self, i, w, h):
        if not (320 <= w <= 16384 and 200 <= h <= 16384):
            self.message.emit("Width and height: 320 to 16384 pixels", True)
            return
        self._edit_screen(i, lambda s: s.__setitem__("size", [w, h]))
        if self._running and i < self._running_count:
            self._ask_screens(f"size {i + 1} {w} {h}")

    @Slot(int, float)
    def setScreenMetres(self, i, m):
        self._edit_screen(i, lambda s: s.__setitem__("metres", round(m, 3)))
        if self._running and i < self._running_count:
            self._ask_screens(f"width {i + 1} {m:.3f}")

    @Slot(int, bool)
    def setCurved(self, i, on):
        """Curve a screen into a cylinder around you (radius: your distance to it now, from
        ft-screens; the layout's distance if the desktop isn't running)."""
        radius = float(ft_layout.load_layout()["preset"].get("distance", 2.0)) if on else 0.0
        if self._running and i < self._running_count:
            reply = self._ask_screens(f"curve {i + 1} {'on' if on else 'off'}")
            if reply and reply.startswith("ok"):
                radius = float(reply.split()[1])
        self._edit_screen(i, lambda s: s.__setitem__("curve", round(radius, 3)))

    @Slot(int)
    def setPrimary(self, i):
        layout = ft_layout.load_layout()
        layout["primary"] = i + 1
        ft_layout.save_layout(layout)
        self.changed.emit()
        if self._running:
            self._run("Moving the taskbar", "scale")

    @Slot()
    def addScreen(self):
        layout = ft_layout.load_layout()
        layout.setdefault("screens", []).append({"size": [1920, 1080], "metres": 1920 / ft_layout.PIXELS_PER_METRE})
        ft_layout.save_layout(layout)
        self.changed.emit()

    @Slot(int)
    def removeScreen(self, i):
        layout = ft_layout.load_layout()
        screens = layout.get("screens", [])
        if len(screens) <= 1 or i >= len(screens):
            return
        screens.pop(i)
        if layout.get("primary") == i + 1:
            layout.pop("primary")
        ft_layout.save_layout(layout)
        self.changed.emit()

    @Slot()
    def toggleScreens(self):
        self._ask_screens("toggle")

    # --- ft-screens: visibility and pinning ---
    @Property("QVariantMap", notify=changed)
    def visibility(self):
        return ft_layout.visibility(ft_layout.load_layout())

    @Slot(str, "QVariant")
    def setVisibility(self, key, value):
        layout = ft_layout.load_layout()
        v = ft_layout.visibility(layout)
        v[key] = value
        layout["visibility"] = v
        ft_layout.save_layout(layout)
        self.changed.emit()
        if self._running:
            if key == "mode":
                self._ask_screens(f"visibility {value}")
            elif key == "wrist_angle":
                self._ask_screens(f"wrist {float(value):.1f}")
            elif key == "controllers":
                self._ask_screens(f"controllers {value}")
            elif key == "in_games":
                self._ask_screens(f"ingames {value}")
            else:
                self._ask_screens(f"gesture {v['gesture_hand']} {float(v['gesture_angle']):.1f}")

    @Property("QVariantList", notify=changed)
    def pins(self):
        return self._pins

    @Property("QVariantList", notify=changed)
    def screensShown(self):
        """For each screen, whether it shows (False: hidden on its own, ft-layout hide N)."""
        layout = ft_layout.load_layout()
        return [not ft_layout.screen_entry(layout, i).get("hidden") for i in range(ft_layout.screen_count(layout))]

    @Slot(int, bool)
    def setScreenShown(self, index, shown):
        """Hide screen `index` (0-based) on its own, whatever the visibility mode, or show it."""
        layout = ft_layout.load_layout()
        screens = layout.setdefault("screens", [])
        while len(screens) <= index:
            screens.append({})
        if shown:
            screens[index].pop("hidden", None)
        else:
            screens[index]["hidden"] = True
        ft_layout.save_layout(layout)
        self.changed.emit()
        if self._running:
            reply = self._ask_screens(f"{'reveal' if shown else 'conceal'} {index + 1}")
            if not (reply and reply.startswith("ok")):
                self.message.emit("Saved; the desktop applies it when it next starts "
                                  "(its compositor is older than hiding screens one at a time)", False)

    @Slot(str, str)
    def pin(self, which, where):
        """Pin screen `which` (1-based, or "all") to "left", "right", or "head" as it is
        now, or take it off ("none")."""
        cmd = f"unpin {which}" if where == "none" else f"pin {which} {where}"
        reply = self._ask_screens(cmd) if self._running else None
        if not (reply and reply.startswith("ok")):
            self.message.emit(f"Couldn't {'unpin' if where == 'none' else 'pin'}: "
                              f"{reply or 'the desktop is not running'}", True)
        elif which == "all" and where != "none":
            place = "on your head" if where == "head" else f"on your {where} wrist"
            self.message.emit(f"All screens ride {place} now. Save as profile… (Layout & profiles) keeps it.", False)
        self._check_running()

    # --- power: ft-powerd and Steam's sleep setting ---
    def _check_powerd(self):
        try:
            self._psock.sendto(b"status", FT_POWERD)
            reply = self._psock.recv(256).decode().split()
            status = (reply[1], float(reply[2]), float(reply[3])) if reply[:1] == ["ok"] else None
        except (OSError, IndexError, ValueError):
            status = None
        if status != self._powerd:
            self._powerd = status
            self.powerChanged.emit()

    @Property("QVariantMap", notify=powerChanged)
    def power(self):
        try:
            off_min = float(ft_layout.read_conf().get("DISPLAY_OFF_MIN") or 0)
        except ValueError:
            off_min = 0.0
        state, unused, _ = self._powerd or ("", 0, 0)
        return {"offMinutes": off_min, "service": self._powerd is not None, "state": state, "unused": unused,
                "steam": self._steam is not None, "steamBusy": self._steam_busy, "steamError": self._steam_error,
                "acSleep": self._steam["ac"] if self._steam else -1,
                "batterySleep": self._steam["battery"] if self._steam else -1}

    @Slot(float)
    def setDisplayOffMinutes(self, minutes):
        """ft-powerd re-reads frametop.conf within 2 s."""
        write_conf_value("DISPLAY_OFF_MIN", f"{max(0.0, minutes):g}")
        self.powerChanged.emit()

    @Slot()
    def displaysOffNow(self):
        try:
            self._psock.sendto(b"off", FT_POWERD)
            reply = self._psock.recv(256).decode()
        except OSError:
            reply = "error the power service isn't running"
        if not reply.startswith("ok"):
            self.message.emit(f"Couldn't turn the displays off: {reply.split(' ', 1)[-1]}", True)
        self._check_powerd()

    def _steam_call(self, what, fn):
        """Runs fn, which talks to Steam (up to a few seconds), off the UI thread, then reads
        Steam's sleep settings; _steam_done gets them on the UI thread."""
        if self._steam_busy:
            return
        self._steam_busy = True
        self.powerChanged.emit()

        def work():
            try:
                fn()
                self._steamDone.emit(steam_settings.sleep_settings(), None, what)
            except (steam_settings.SteamUnreachable, OSError, ValueError) as e:
                self._steamDone.emit(None, str(e), what)

        threading.Thread(target=work, daemon=True).start()

    def _steam_done(self, settings, error, what):
        self._steam_busy = False
        if settings is not None:
            self._steam, self._steam_error = settings, ""
        else:
            self._steam, self._steam_error = None, error
            if what:
                self.message.emit(f"Couldn't change Steam's sleep setting: {error}", True)
        self.powerChanged.emit()

    @Slot()
    def refreshPower(self):
        self._check_powerd()
        self._steam_call("", lambda: None)

    @Slot(bool)
    def setStayAwake(self, on):
        """Steam's "When Plugged In and Idle -> Sleep after" is Never while this is on. The value
        from before is kept in frametop.conf and goes back when it's turned off."""
        def change():
            ac = steam_settings.sleep_settings()["ac"]
            if on:
                if ac > 0:
                    write_conf_value("STEAM_SLEEP_AC_BEFORE", str(ac))
                steam_settings.set_sleep_setting("system_idle_suspend_ac_sec", 0)
            elif ac == 0:
                try:
                    before = int(ft_layout.read_conf().get("STEAM_SLEEP_AC_BEFORE") or STEAM_SLEEP_AC_DEFAULT)
                except ValueError:
                    before = STEAM_SLEEP_AC_DEFAULT
                steam_settings.set_sleep_setting("system_idle_suspend_ac_sec", before if before > 0 else STEAM_SLEEP_AC_DEFAULT)
        self._steam_call("stay awake" if on else "sleep", change)

    @Slot()
    def restartDesktop(self):
        """Restart the Frametop desktop (this app closes with it) to apply count and resolution."""
        argv = host_command("systemd-run", "--user", "--collect", "--quiet", os.path.abspath(DESKTOPS), "restart")
        QProcess.startDetached(argv[0], argv[1:])
        self.message.emit("Restarting the desktop…", False)

    # --- layout ---
    def _edit_layout(self, fn):
        layout = ft_layout.load_layout()
        fn(layout)
        ft_layout.save_layout(layout)
        self.changed.emit()

    @Slot(str)
    def setMode(self, mode):
        def edit(layout):
            layout["mode"] = mode
            layout.pop("active", None)
        self._edit_layout(edit)

    @Property("QVariantList", notify=changed)
    def layoutNames(self):
        return ft_layout.layout_names(ft_layout.load_layout())

    @Slot(str)
    def useLayout(self, name):
        """A named layout as the arrangement (Arrange now puts the screens there)."""
        try:
            self._edit_layout(lambda l: ft_layout.use_named(l, name))
        except RuntimeError as e:
            self.message.emit(str(e), True)

    @Slot(str)
    def saveLayout(self, name):
        try:
            ft_layout.check_name(name)
        except RuntimeError as e:
            return self.message.emit(str(e), True)
        self._run(f"Saving the arrangement as {' '.join(name.split())}", "save", name)

    @Slot(str, str)
    def renameLayout(self, old, new):
        try:
            self._edit_layout(lambda l: ft_layout.rename_named(l, old, new))
            ft_layout.write_launchers(ft_layout.load_layout())
        except (RuntimeError, OSError) as e:
            self.message.emit(str(e), True)

    @Slot(str)
    def deleteLayout(self, name):
        try:
            self._edit_layout(lambda l: ft_layout.delete_named(l, name))
            ft_layout.write_launchers(ft_layout.load_layout())
        except (RuntimeError, OSError) as e:
            self.message.emit(str(e), True)

    # --- profiles (docs/profiles.md): a named layout's apps and hidden screens ---
    @Slot(str, result="QVariantList")
    def profileWindows(self, name):
        """A profile's windows, as "app" and "where" for the list."""
        out = []
        for e in ft_layout.load_layout().get("profiles", {}).get(name, {}).get("windows", []):
            app = e.get("app") or os.path.basename((e.get("cmd") or ["?"])[0])
            app = app.rsplit(".", 1)[-1] if "." in app and not e.get("cmd") else app
            where = "floating" if "float" in e else f"screen {e.get('screen', 1)}" + (", maximized" if e.get("maximized") else "")
            out.append({"app": app, "where": where})
        return out

    @Slot(str, result="QVariantList")
    def profileHidden(self, name):
        return ft_layout.load_layout().get("profiles", {}).get(name, {}).get("hidden", [])

    @Slot(str, int)
    def removeProfileWindow(self, name, index):
        def edit(layout):
            windows = layout.get("profiles", {}).get(name, {}).get("windows", [])
            if 0 <= index < len(windows):
                windows.pop(index)
        self._edit_layout(edit)

    @Property(str, notify=changed)
    def defaultProfile(self):
        return ft_layout.load_layout().get("default_profile", "")

    @Slot(str)
    def setDefaultProfile(self, name):
        def edit(layout):
            if name:
                layout["default_profile"] = name
            else:
                layout.pop("default_profile", None)
        self._edit_layout(edit)

    @Slot(str, "QVariant")
    def setPreset(self, key, value):
        def edit(layout):
            layout["mode"] = "preset"
            layout["preset"][key] = value
        self._edit_layout(edit)

    @Slot(bool)
    def setAuto(self, on):
        self._edit_layout(lambda l: l.__setitem__("auto", bool(on)))

    @Slot()
    def arrange(self):
        """Arrange the screens; in a profile, also open its apps (ft-layout use)."""
        layout = ft_layout.load_layout()
        name = layout.get("active")
        if layout.get("mode") == "custom" and name in layout.get("layouts", {}):
            self._run(f"Opening {name}", "use", name)
        else:
            self._run("Arranging the screens", "apply")

    @Slot()
    def capture(self):
        self._run("Saving the current arrangement", "capture")

    # --- ft-layout on the host ---
    def _run(self, label, *args):
        if self._proc is not None:
            self.message.emit(f"Still busy: {self._busy}", True)
            return
        self._busy = label
        self.busyChanged.emit()
        proc = QProcess(self)
        proc.setProcessChannelMode(QProcess.MergedChannels)
        argv = host_command(os.path.abspath(FT_LAYOUT), *args)
        proc.finished.connect(lambda code, _status: self._done(proc, label, code))
        self._proc = proc
        proc.start(argv[0], argv[1:])

    def _done(self, proc, label, code):
        out = bytes(proc.readAllStandardOutput()).decode(errors="replace").strip()
        self._proc = None
        self._busy = ""
        self.busyChanged.emit()
        self.changed.emit()
        last = out.splitlines()[-1] if out else ""
        if code == 0:
            self.message.emit(f"{label}: done" + (f" ({last})" if last and not last.startswith("ok") else ""), False)
        else:
            self.message.emit(f"{label} failed: {last or 'exit code ' + str(code)}", True)


def main():
    app = QGuiApplication(sys.argv)
    app.setApplicationName("ft-display-settings")
    app.setApplicationDisplayName("Frametop Display Settings")
    app.setDesktopFileName("ft-display-settings")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle("org.kde.desktop")
    engine = QQmlApplicationEngine()
    backend = Backend()
    engine.rootContext().setContextProperty("backend", backend)
    engine.rootContext().setContextProperty("startPage", os.environ.get("FT_DISPLAY_PAGE", "screens"))
    engine.load(QUrl.fromLocalFile(os.path.join(HERE, "main.qml")))
    if not engine.rootObjects():
        sys.exit(1)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
