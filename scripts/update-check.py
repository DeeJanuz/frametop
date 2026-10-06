#!/usr/bin/env python3
"""Check what Frametop needs from SteamOS, for after a SteamOS update.

On the Frame, a SteamOS update replaces SteamVR, KWin, gamescope, and the kernel along with
the rest of the OS image. Frametop lives in the home folder and survives it, but it uses
SteamVR and KWin interfaces that an update can change or drop, some of them undocumented.
This checks each one, and compares the versions with the last ones recorded as working.

  scripts/update-check.py              # check (scripts/doctor.sh runs this)
  scripts/update-check.py --mark-good  # check, then record these versions as working

From a PC: ssh frame python3 - [--mark-good] < scripts/update-check.py

It only reads. It connects to SteamVR as a background app (which never starts SteamVR),
runs `vrcmd --overlays` as the pointer helper does, maps the eye tracker's shared memory
read-only, and follows a controller on vrserver's web socket for a moment. It never starts,
stops, or restarts anything.
"""
import base64
import json
import mmap
import os
import platform
import re
import socket
import struct
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
UID = os.getuid()
KNOWN_GOOD = os.path.join(HOME, ".local/state/frametop/known-good.json")
STEAMVR_BIN = "/opt/steamvr/bin/linuxarm64"
LAUNCHER = os.path.join(HOME, ".local/share/applications/deckard-nested-desktop.desktop")
STOCK_LAUNCHER = "/usr/share/applications/deckard-nested-desktop.desktop"
NATIVE_LAUNCHER = os.path.join(HOME, ".local/share/applications/native-deckard-nested-desktop.desktop")
VRPATHS = os.path.join(HOME, ".config/openvr/openvrpaths.vrpath")
# The Frametop desktop has its own XDG_CONFIG_HOME (session/frametop-session.sh). An install from a
# terminal there, before pointer/driver/install.sh set SteamVR's, left vrpathreg's registry here.
STRAY_VRPATHS = os.path.join(HOME, ".config/frametop/openvr/openvrpaths.vrpath")
BACKLIGHT = "/sys/class/backlight/ae94000.dsi.0/brightness"  # as in power/ft-powerd.cpp
EYE_MMAP = "/dev/shm/eye-server.mmap"
VRSERVER = "http://127.0.0.1:27062"  # its web socket: input/vrws.py
PAUSE_STATE = f"/run/user/{os.getuid()}/frametop-pause.json"  # input/game_pause.py
HOST_GLIBC = (2, 39)  # the newest the pointer driver may need (pointer/driver/build.sh)

# Packages in the OS image that Frametop depends on, and what to try by hand when one changes.
PACKAGES = {
    "deckard-steamvr-rel": "the 3D mouse on the dashboard, on SteamVR Settings, and on a SteamVR "
                           "window's grab bar; picking up a controller; mapped controller buttons; gaze",
    "kwin": "clicks near the far edge of a screen whose scale isn't 1; every screen comes back "
            "after a desktop restart; floating a window; no blur behind the taskbar's menus",
    "plasma-workspace": "the taskbar and panels after a desktop restart; no DiscoverNotifier or "
                        "ibus-daemon inside the desktop",
    "at-spi2-core": "an AT-SPI-aware app appears on the nested desktop's accessibility bus; "
                    "the registry stops and comes back after a desktop restart",
    "gamescope": "the headset's volume buttons with nothing focused; typing goes where you last clicked",
    "bluez": "a Bluetooth mouse reconnecting after it sleeps",
    "steamdeck-kde-presets": "Launch a program -> Native Desktop opens SteamOS's own desktop",
}
KERNEL_HINT = "display power (ft-powerd) and hand tracking"

# Frametop's user services, and the control socket each binds once it's up.
UNITS = {
    "frametop-input-relay": "@frametop_relay",
    "frametop-pointer": "@ft_pointer_helper",
    "frametop-gaze": "@ft_gazed",
    "frametop-power": "@ft_powerd",
    "frametop-camd": None,
    "frametop-hands": None,
}

# eye-server.mmap offsets, the same as gaze/ft-gaze.cpp's (packed, little-endian), and the
# layouts its EyeFile::Detect knows: everything from the timestamp on moved by a shift.
EYE_COUNTER, EYE_TIME, EYE_NEED = 0x38, 0x157, 0x1F3 + 5
EYE_SET1 = (0x15F, 0x16B)  # set 1 left, right
EYE_LAYOUTS = {0: "stable", 5: "the 0.4.x beta's (+5)"}

# Runs in a child process, so a SteamVR that hangs can't hang the check.
OPENVR_PROBE = r"""
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
lib.VR_InitInternal2.restype = ctypes.c_uint32
lib.VR_InitInternal2.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_char_p]
lib.VR_GetVRInitErrorAsSymbol.restype = ctypes.c_char_p
lib.VR_IsInterfaceVersionValid.restype = ctypes.c_bool
lib.VR_IsInterfaceVersionValid.argtypes = [ctypes.c_char_p]
err = ctypes.c_int(0)
lib.VR_InitInternal2(ctypes.byref(err), 3, None)  # VRApplication_Background
if err.value:
    print("init", lib.VR_GetVRInitErrorAsSymbol(err.value).decode())
    sys.exit(1)
for name in sys.argv[2:]:
    print(name, int(lib.VR_IsInterfaceVersionValid(name.encode())))
lib.VR_ShutdownInternal()
"""

failed = False


def report(state, label, detail=""):
    global failed
    failed = failed or state == "FAIL"
    print(f"{state:<6}{label}{': ' + detail if detail else ''}", flush=True)


def run(*cmd, timeout=15, env=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        return p.returncode, p.stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, str(e)


def systemctl(*args):
    return run("systemctl", "--user", *args)[1].strip()


def unix_sockets():
    with open("/proc/net/unix") as f:
        return {parts[7] for parts in map(str.split, f) if len(parts) > 7}


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


# --- versions

def current_versions():
    versions = {}
    with open("/etc/os-release") as f:
        for line in f:
            if line.startswith("BUILD_ID="):
                versions["SteamOS build"] = line.split("=", 1)[1].strip().strip('"')
    for line in run("pacman", "-Q", *PACKAGES)[1].splitlines():
        name, _, version = line.partition(" ")
        if name in PACKAGES:
            versions[name] = version
    versions["kernel"] = platform.release()
    return versions


def check_versions(versions):
    try:
        with open(KNOWN_GOOD) as f:
            good = json.load(f)
        date, old = good["date"], good["versions"]
    except (OSError, ValueError, KeyError, TypeError):
        report("warn", "known-good versions", "none recorded yet; run scripts/doctor.sh --mark-good "
               "once Frametop works")
        return
    changed = [k for k in sorted(set(versions) | set(old)) if old.get(k) != versions.get(k)]
    if not changed:
        report("ok", "versions", f"the same as when marked good ({date})")
    for k in changed:
        hint = PACKAGES.get(k) or (KERNEL_HINT if k == "kernel" else "")
        report("warn", f"{k} changed since {date}",
               f"{old.get(k, 'none')} -> {versions.get(k, 'none')}" + (f"; try {hint}" if hint else ""))


# --- the host, without SteamVR

def check_host():
    needed = [
        ("/usr/bin/kwin_wayland_wrapper", "FAIL", "the desktop can't start KWin"),
        ("/usr/bin/startplasma-wayland", "FAIL", "the desktop can't start Plasma"),
        ("/usr/bin/dbus-run-session", "FAIL", "the desktop can't start its session bus"),
        ("/usr/bin/python3", "warn", "nested accessibility can't run its startup helper"),
        ("/usr/bin/gdbus", "warn", "nested accessibility can't discover or check its bus"),
        ("/usr/lib/at-spi-bus-launcher", "warn", "nested accessibility can't start its bus"),
        ("/usr/lib/at-spi2-registryd", "warn", "nested accessibility has no fallback registry"),
        ("/usr/share/dbus-1/services/org.a11y.Bus.service", "warn",
         "nested accessibility can't activate its bus"),
        ("/usr/share/dbus-1/accessibility-services/org.a11y.atspi.Registry.service", "warn",
         "nested accessibility can't activate its registry natively"),
        (f"{STEAMVR_BIN}/vrcmd", "FAIL", "the 3D mouse can't find panels"),
        (f"{STEAMVR_BIN}/vrpathreg", "warn", "the pointer driver can't be installed or removed"),
        ("/usr/share/deckard/mesavars.sh", "warn", "the desktop starts without SteamOS's Mesa settings"),
        ("/etc/profile.d/flatpak.sh", "warn", "Flatpak apps may open Discover instead of starting"),
        (STOCK_LAUNCHER, "warn", "SteamOS's Desktop launcher entry is gone or renamed, so Frametop's "
         "copy may not replace it, and there's no Native Desktop"),
    ]
    missing = [n for n in needed if not os.path.exists(n[0])]
    for path, state, effect in missing:
        report(state, f"{path} missing", effect)
    if not missing:
        report("ok", "host files", f"all {len(needed)} the desktop and pointer use are there")

    try:
        with open("/usr/bin/kwin_wayland", "rb") as f:
            kwin = f.read()
    except OSError:
        kwin = b""
    # Qt keeps the option name as a UTF-16 string literal.
    if "output-count".encode("utf-16-le") in kwin:
        report("ok", "KWin", "has --output-count (one output per screen)")
    else:
        report("FAIL", "KWin", "no --output-count option: the desktop gets one screen at most")
    # The session turns these built-in effects off by id (blurEnabled, contrastEnabled in kwinrc).
    gone = [e for e in ("blur", "contrast") if b"KWin::%s_factory" % e.encode() not in kwin]
    if not gone:
        report("ok", "KWin effects", "blur and contrast, which the desktop turns off, keep their ids")
    else:
        report("warn", "KWin effects", f"no built-in {', '.join(gone)} effect: renamed? The desktop's "
               "kwinrc may no longer turn it off (session/frametop-session.sh)")

    if systemctl("cat", "steamvr.service"):
        report("ok", "steamvr.service", "Frametop's services start and stop with it")
    else:
        report("FAIL", "steamvr.service", "gone or renamed; Frametop's services are ordered on it")

    glibc = os.confstr("CS_GNU_LIBC_VERSION") or ""
    have = tuple(int(x) for x in re.findall(r"\d+", glibc)[:2])
    want = ".".join(map(str, HOST_GLIBC))
    if have >= HOST_GLIBC:
        report("ok", "glibc", f"{glibc}, the pointer driver needs {want}")
    else:
        report("FAIL", "glibc", f"{glibc}, older than the {want} the pointer driver needs")

    try:
        with open(VRPATHS) as f:
            drivers = json.load(f).get("external_drivers") or []
    except (OSError, ValueError):
        drivers = []
    driver = next((d for d in drivers if os.path.basename(d.rstrip("/")) == "ft_pointer"), None)
    if driver and os.path.isfile(os.path.join(driver, "bin/linuxarm64/driver_ft_pointer.so")):
        report("ok", "pointer driver", f"registered with SteamVR ({driver})")
    else:
        report("FAIL", "pointer driver", "not registered with SteamVR; run pointer/driver/install.sh install, "
               "then restart SteamVR")
    try:
        with open(STRAY_VRPATHS) as f:
            stray = json.load(f).get("runtime") is None
    except (OSError, ValueError, AttributeError):
        stray = False
    if stray:
        report("warn", "OpenVR path registry", f"{STRAY_VRPATHS} has no SteamVR in it, and hides SteamVR from "
               "OpenVR programs started in the Frametop desktop; pointer/driver/install.sh install removes it")

    session = launcher_session()
    if session is None:
        report("warn", "launcher", "Launch a program -> Desktop starts the stock desktop "
               "(./desktops.sh install brings Frametop back)")
    elif os.path.isfile(session):
        report("ok", "launcher", f"Desktop starts {session}")
    else:
        report("FAIL", "launcher", f"Desktop starts {session}, which doesn't exist")
    if os.path.exists(NATIVE_LAUNCHER):
        try:
            same = launcher_keys(NATIVE_LAUNCHER) == launcher_keys(STOCK_LAUNCHER)
        except OSError:
            same = False
        if same:
            report("ok", "Native Desktop", "the launcher's copy matches SteamOS's Desktop entry")
        else:
            report("warn", "Native Desktop", "the launcher's copy no longer matches SteamOS's Desktop entry; "
                   "./desktops.sh install refreshes it")

    if systemctl("is-enabled", "frametop-power") == "enabled":
        if os.access(BACKLIGHT, os.W_OK):
            report("ok", "backlight", "ft-powerd can turn the displays off")
        else:
            report("FAIL", "backlight", f"{BACKLIGHT} isn't writable, so ft-powerd can't turn the displays off")


def launcher_keys(path):
    """A launcher entry's keys, less the ones desktops.sh changes in its Native Desktop copy."""
    with open(path) as f:
        pairs = [line.rstrip("\n").split("=", 1) for line in f if "=" in line and not line.startswith("#")]
    return {k: v for k, v in pairs if k != "X-Steam-Special" and k.split("[")[0] != "Name"}


def launcher_session():
    try:
        with open(LAUNCHER) as f:
            m = re.search(r"^Exec=(\S+)", f.read(), re.M)
    except OSError:
        return None
    return m.group(1) if m else None


# --- with SteamVR running

def installed_binaries():
    """The programs the services and the launcher run: {path: what runs it}."""
    found = {}
    for unit in UNITS:
        argv = re.search(r"argv\[\]=([^;]*)", systemctl("show", "-p", "ExecStart", "--value", unit + ".service"))
        for arg in (argv.group(1).split() if argv else []):
            if not arg.startswith(HOME) or not os.path.isfile(arg):
                continue
            if is_elf(arg):
                found[arg] = unit
            else:
                # A script runs the programs built next to it (ft-gazed runs build/ft-gaze).
                build = os.path.join(os.path.dirname(arg), "build")
                for name in sorted(os.listdir(build)) if os.path.isdir(build) else []:
                    if is_elf(os.path.join(build, name)):
                        found[os.path.join(build, name)] = unit
    session = launcher_session()
    if session:
        screens = os.path.normpath(os.path.join(os.path.dirname(os.path.realpath(session)),
                                                "../screens/build/ft-screens"))
        if os.path.isfile(screens):
            found[screens] = "the desktop"
    return found


def check_openvr():
    needs = {}  # interface version: programs built against it
    for path in installed_binaries():
        with open(path, "rb") as f:
            for name in set(re.findall(rb"IVR[A-Za-z]+_\d{3}", f.read())):
                needs.setdefault(name.decode(), []).append(os.path.basename(path))
    if not needs:
        report("skip", "OpenVR interfaces", "no installed Frametop programs found")
        return
    code, out = run(sys.executable, "-c", OPENVR_PROBE, f"{STEAMVR_BIN}/libopenvr_api.so", *sorted(needs),
                    timeout=30)
    served = dict(line.split() for line in out.splitlines() if len(line.split()) == 2)
    if code != 0 or "init" in served:
        why = served.get("init") or out.strip() or f"exit {code}"
        report("FAIL", "OpenVR", f"can't connect to SteamVR as a background app ({why})")
        return
    gone = [n for n in sorted(needs) if served.get(n) != "1"]
    for name in gone:
        report("FAIL", f"OpenVR {name}", f"this SteamVR doesn't serve it; rebuild {', '.join(sorted(needs[name]))} "
               "against a newer OpenVR header")
    if not gone:
        programs = sorted({p for ps in needs.values() for p in ps})
        report("ok", "OpenVR interfaces", f"all {len(needs)} that {', '.join(programs)} use are served")


def check_overlays(desktop_up):
    code, out = run(f"{STEAMVR_BIN}/vrcmd", "--overlays",
                    env=dict(os.environ, LD_LIBRARY_PATH=STEAMVR_BIN))
    # The format ft-pointer.cpp and ft_layout.py parse: 'key' -- 'name', WxH visible VROverlayType_...
    keys = re.findall(r"^'([^']+)' -- '.*VROverlayType_", out, re.M)
    if not keys:
        report("FAIL", "vrcmd --overlays", "lists no overlays in the expected format, so the 3D mouse can't "
               "find panels (pointer/helper/ft-pointer.cpp and layout/ft_layout.py parse it)")
    elif desktop_up and not any(re.fullmatch(r"frametop\.screen\.\d+", k) for k in keys):
        report("FAIL", "vrcmd --overlays", f"lists {len(keys)} overlays, but none of the desktop's screens")
    else:
        report("ok", "vrcmd --overlays", f"{len(keys)} overlays in the format the pointer parses")


def check_services(sockets, desktop_up):
    try:
        with open(PAUSE_STATE) as f:
            pause = json.load(f)
        paused = pause["stopped"]["units"] if pause.get("paused") else []
    except (OSError, ValueError, KeyError, TypeError):
        paused = []
    for unit, socket in UNITS.items():
        if systemctl("is-enabled", unit) != "enabled":
            continue
        state = systemctl("is-active", unit)
        if state != "active" and unit + ".service" in paused:
            report("skip", unit, "stopped while Frametop is paused for a VR game (input/ft-pause off resumes it)")
        elif state != "active":
            report("FAIL", unit, f"{state}; see journalctl --user -u {unit}")
        elif socket and socket not in sockets:
            report("FAIL", unit, f"runs, but hasn't opened {socket}")
        else:
            report("ok", unit, "running")
    if "@ft_pointer" in sockets:
        report("ok", "ft_pointer driver", "SteamVR loaded it")
    else:
        report("FAIL", "ft_pointer driver", "SteamVR didn't load it; see pointer/driver/install.sh log")
    if not desktop_up:
        report("skip", "desktop", "not running; start it and check again to cover the screens")
    elif "@ft_screens" in sockets:
        report("ok", "desktop", "ft-screens is running")
    else:
        report("FAIL", "desktop", "ft-screens runs, but hasn't opened @ft_screens")
    if systemctl("is-enabled", "frametop-camd") == "enabled" and run("pgrep", "-x", "XRService")[0] != 0:
        report("FAIL", "XRService", "not running, so hand tracking has no cameras")


def check_steam_ui():
    """Steam's UI calls the Steam menu shortcut uses (steam/ft-steam). They're Steam client
    internals, so a Steam update can change them too, not just a SteamOS one."""
    # Run from stdin (over SSH) there's no __file__: try the usual checkouts.
    repos = [os.path.dirname(os.path.dirname(os.path.abspath(__file__)))] if "__file__" in globals() else []
    repos += [os.path.join(HOME, "frametop"), os.path.join(HOME, "dev/frametop")]
    tool = next((t for t in (os.path.join(r, "steam/ft-steam") for r in repos) if os.path.exists(t)), None)
    if not tool:
        report("skip", "Steam menu shortcut", "steam/ft-steam isn't in a checkout here")
        return
    try:
        p = subprocess.run([tool, "check"], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as e:
        report("warn", "Steam menu shortcut", f"ft-steam check didn't run ({e})")
        return
    detail = (p.stdout + p.stderr).strip()
    if p.returncode == 0:
        report("ok", "Steam menu shortcut", "Steam's UI still has the calls steam/ft-steam makes")
    elif detail.startswith("ft-steam:"):
        report("skip", "Steam menu shortcut", detail[len("ft-steam:"):].strip())
    else:
        report("warn", "Steam menu shortcut", f"{detail}; a Meta tap won't open the Steam menu (steam/ft-steam)")


def check_vr_socket():
    """vrserver's web socket, which the pause gesture reads the controllers from (input/vrws.py,
    input/game_pause.py). It's undocumented: SteamVR's controller binding page uses it."""
    label, headers = "vrserver web socket", {"Referer": f"{VRSERVER}/dashboard/controllerbinding.html"}
    try:
        import urllib.request
        with urllib.request.urlopen(urllib.request.Request(f"{VRSERVER}/input/getstate.json", headers=headers),
                                    timeout=3) as r:
            devices = json.load(r).get("devices", [])
    except (OSError, ValueError) as e:
        report("FAIL", label, f"/input/getstate.json didn't answer ({e}); the pause gesture can't read the controllers")
        return
    found = [d for d in devices if d.get("controller_type") == "frame_controller" and d.get("root_path")
             and d.get("side") in ("left", "right")]
    if not found:
        report("skip", label, "no Frame controller is on, so it wasn't checked")
        return
    if not all(any(c.get("path") == "/input/thumbstick/click" for c in d.get("components", [])) for d in found):
        report("warn", label, "a controller lists no /input/thumbstick/click; the default pause gesture needs it")
    data = b""
    try:
        with socket.create_connection(("127.0.0.1", 27062), timeout=3) as s:
            key = base64.b64encode(os.urandom(16)).decode()
            s.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:27062\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: {VRSERVER}\r\n"
                       f"Referer: {headers['Referer']}\r\n\r\n").encode())
            while b"\r\n\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            if b" 101 " not in data.split(b"\r\n", 1)[0] + b" ":
                report("FAIL", label, "vrserver refused the web socket; the pause gesture can't read the controllers")
                return
            mailbox = f"frametop_check_{os.getpid()}"
            # Every controller: one lying still (or asleep) may send nothing at all.
            for text in [f"mailbox_open {mailbox}"] + ["mailbox_send input_server " + json.dumps(
                    {"type": "request_input_state_updates", "device_path": d["root_path"], "returnAddress": mailbox})
                    for d in found]:
                payload, mask = text.encode(), os.urandom(4)
                size = bytes([0x80 | len(payload)]) if len(payload) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(payload))
                s.sendall(b"\x81" + size + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
            deadline = time.monotonic() + 3
            try:
                while b"update_component_states" not in data and time.monotonic() < deadline:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    data += chunk
            except socket.timeout:
                report("skip", label, "it took the subscription, but no controller sent anything in 3 s "
                       "(lying still or asleep?): pick one up and check again")
                return
    except OSError as e:
        report("FAIL", label, f"{e}; the pause gesture can't read the controllers")
        return
    if b"update_component_states" in data and b"/click" in data:
        report("ok", label, "it still streams the controllers' buttons, which the pause gesture reads")
    else:
        report("FAIL", label, "no button states came, so the pause gesture can't read the controllers "
               "(input/vrws.py: the message format changed?)")


def check_eye_tracker():
    gaze = systemctl("is-enabled", "frametop-gaze") == "enabled"
    try:
        with open(EYE_MMAP, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    except (OSError, ValueError):
        report("FAIL" if gaze else "skip", "eye tracker", f"{EYE_MMAP} isn't there")
        return
    if len(m) < EYE_NEED:
        report("FAIL", "eye tracker", f"{EYE_MMAP} is {len(m)} bytes, smaller than gaze/ft-gaze.cpp reads")
        return

    def fits(shift, before, now):
        """ft-gaze's test (EyeFile::Detect): the timestamp near the clock and moving on, and
        both set-1 directions unit vectors."""
        t = struct.unpack_from("<d", m, EYE_TIME + shift)[0]
        return now - 2 < t <= now + 2 and t > before[shift] and all(
            0.81 < sum(x * x for x in struct.unpack_from("<3f", m, o + shift)) < 1.21 for o in EYE_SET1)

    first = struct.unpack_from("<I", m, EYE_COUNTER)[0]
    for attempt in range(5):  # 1.5 s: a blink or a moment with an eye lost doesn't fail it
        before = {shift: struct.unpack_from("<d", m, EYE_TIME + shift)[0] for shift in EYE_LAYOUTS}
        time.sleep(0.3)
        if attempt == 0 and struct.unpack_from("<I", m, EYE_COUNTER)[0] == first:
            report("skip", "eye tracker", "idle, so its layout wasn't checked")
            return
        now = time.clock_gettime(time.CLOCK_MONOTONIC_RAW)
        for shift, name in EYE_LAYOUTS.items():
            if fits(shift, before, now):
                report("ok", "eye tracker", f"eye-server.mmap has a layout gaze/ft-gaze.cpp knows: {name}")
                return
    age = now - struct.unpack_from("<d", m, EYE_TIME)[0]
    report("FAIL" if gaze else "warn", "eye tracker", f"eye-server.mmap has none of the layouts gaze/ft-gaze.cpp "
           f"knows (sample age {age:.3g} s at the stable offset), so SteamVR's eye tracking can't be used: add the "
           "new one to EyeFile::Detect. Right after the headset goes on its tracker warms up for about 20 s: "
           "check again after that")


def main():
    args = sys.argv[1:]
    if args not in ([], ["--mark-good"]):
        sys.exit("usage: update-check.py [--mark-good]")
    # A terminal in the desktop has its session's runtime dir and bus; systemctl needs the real ones.
    os.environ["XDG_RUNTIME_DIR"] = f"/run/user/{UID}"
    os.environ["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path=/run/user/{UID}/bus"
    # And its own config folder, where SteamVR's path registry isn't: OpenVR and vrcmd need ours.
    os.environ["XDG_CONFIG_HOME"] = os.path.join(HOME, ".config")

    versions = current_versions()
    check_versions(versions)
    check_host()
    steamvr_up = run("pgrep", "-x", "vrserver")[0] == 0
    if not steamvr_up:
        report("skip", "SteamVR checks", "SteamVR isn't running")
    else:
        sockets = unix_sockets()
        desktop_up = run("pgrep", "-x", "ft-screens")[0] == 0
        check_openvr()
        check_overlays(desktop_up)
        check_services(sockets, desktop_up)
        check_steam_ui()
        check_vr_socket()
        check_eye_tracker()

    if args == ["--mark-good"]:
        if failed:
            print("Not recording these versions as working: fix the failures first.")
        elif not steamvr_up:
            print("Not recording these versions as working: start SteamVR, so its checks run too.")
            sys.exit(1)
        else:
            os.makedirs(os.path.dirname(KNOWN_GOOD), exist_ok=True)
            with open(KNOWN_GOOD, "w") as f:
                json.dump({"date": time.strftime("%Y-%m-%d"), "versions": versions}, f, indent=2)
            print(f"Recorded these versions as working in {KNOWN_GOOD}.")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
