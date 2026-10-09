#!/usr/bin/env python3
"""Frametop Remote Displays: other computers' monitors as Frametop screens, streamed with
Moonlight's protocol from Vibepollo (docs/remote-displays.md).

A Kirigami (QML) app with a Python backend, like Frametop Display Settings. It runs in
the dev container:
  - Hosts: computers running Vibepollo, found on the network (they announce _nvstream._tcp
    over mDNS; avahi-browse on the host) or typed in. You sign in once with the host's Web UI
    user name and password: Frametop asks it for an API token that can do only what it needs
    (SCOPES), keeps that in ~/.local/share/frametop-stream/hosts/ADDRESS.token, readable only
    by you, and forgets the password. The Web UI's certificate is self-signed, so the one seen
    then is pinned (ADDRESS.pin, its public key's SHA-256): ft-stream and later sign-ins
    refuse another. Whether each host answers on its Web UI port is checked now and then.
  - Displays: a host's existing monitor, or a virtual one at any size, each its own stream
    (ft-layout remote add pairs a client for it). Each one connects and disconnects on its
    own, or all of a host's at once; a disconnected one keeps its place and settings.
    Its stream's resolution, frame rate and bitrate, its width in VR, and whether it shows.
  - Connection, per host with a Steam Link dongle on the Frame's hotspot (found with it on
    the network, or with Find): auto (the dongle when it answers, else the network), network
    only, or dongle only. Find looks for the host among the hotspot's clients (the same
    <uniqueid> in their serverinfo as at its own address). Hosts without one use the network.
Where the displays are in VR is kept like the screens' (move them there; Display Settings'
Save as profile keeps it). Anything that touches the streams runs layout/ft-layout on the
host; the streams' state comes from ft-screens (@ft_screens: "remotes", "remote N info").
Launch with remote-displays/ft-remote-displays (host wrapper).
"""
import base64
import hashlib
import http.client
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import urllib.request
import uuid

from PySide6.QtCore import Property, QObject, QProcess, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QGuiApplication, QIcon
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuickControls2 import QQuickStyle

HERE = os.path.dirname(os.path.abspath(__file__))
LAYOUT_DIR = os.path.join(HERE, "..", "layout")
sys.path.insert(0, LAYOUT_DIR)
import ft_layout  # noqa: E402

FT_LAYOUT = os.path.join(LAYOUT_DIR, "ft-layout")
FT_SCREENS = "\0ft_screens"
TOKEN_DIR = os.path.expanduser("~/.local/share/frametop-stream/hosts")  # each host's API token (ft-stream)
WEB_UI_PORT = 47990  # Vibepollo's Web UI, where ft-stream pairs and lists monitors
STREAM_RATES = [30, 60, 72, 90, 120]
# What Frametop's API token may do: pair its displays' clients (submit their PINs), list the
# clients and set their permissions, and list the host's monitors.
SCOPES = [{"path": "/api/pin", "methods": ["POST"]}, {"path": "/api/clients/list", "methods": ["GET"]},
          {"path": "/api/clients/update", "methods": ["POST"]}, {"path": "/api/display-devices", "methods": ["GET"]}]
NAME_RE = r"[A-Za-z0-9][A-Za-z0-9 ._-]{0,39}"
RESOLUTIONS = [(1920, 1080, ""), (2560, 1440, ""), (3840, 2160, "4K"), (2560, 1080, "ultrawide"),
               (3440, 1440, "ultrawide"), (5120, 1440, "super ultrawide"), (1920, 1200, "16:10"),
               (2560, 1600, "16:10"), (1080, 1920, "portrait"), (1440, 2560, "portrait")]


def host_command(*cmd):
    """argv to run a command on the SteamOS host (we live in the dev container).

    distrobox-host-exec reaches the host through the user's real session bus; inside the
    desktop our DBUS_SESSION_BUS_ADDRESS is the nested session's private one, where it
    fails (exit 127, silently)."""
    if not shutil.which("distrobox-host-exec"):
        return list(cmd)
    bus = f"unix:path=/run/user/{os.getuid()}/bus"
    return ["env", f"DBUS_SESSION_BUS_ADDRESS={bus}", "distrobox-host-exec"] + list(cmd)


def token_path(address):
    return os.path.join(TOKEN_DIR, f"{address}.token")


def pin_path(address):
    return os.path.join(TOKEN_DIR, f"{address}.pin")


def write_private(path, text):
    os.makedirs(TOKEN_DIR, mode=0o700, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text + "\n")


def spki_pin(der):
    """A certificate's public key pin as curl takes it (CURLOPT_PINNEDPUBLICKEY)."""
    pem = subprocess.run(["openssl", "x509", "-inform", "der", "-pubkey", "-noout"], input=der,
                         capture_output=True, check=True).stdout
    key = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "der"], input=pem,
                         capture_output=True, check=True).stdout
    return "sha256//" + base64.b64encode(hashlib.sha256(key).digest()).decode()


def parse_avahi(text):
    """avahi-browse -rpt _nvstream._tcp -> [{name, address, dongle}]: a host's address on the
    network, and on the Frame's hotspot (wlanap: its Steam Link dongle), if it's there."""
    unescape = lambda v: re.sub(r"\\(\d{3})", lambda m: chr(int(m.group(1))), v).replace("\\.", ".")
    found = {}
    for line in text.splitlines():
        f = line.split(";")
        if len(f) < 9 or f[0] != "=" or f[2] != "IPv4" or f[1].startswith("tailscale"):
            continue
        name = unescape(f[3])
        h = found.setdefault(name, {"name": name, "address": "", "dongle": ""})
        key = "dongle" if f[1] == "wlanap" else "address"
        h[key] = h[key] or f[7]
    out = []
    for h in found.values():
        if not h["address"]:  # only on the hotspot
            h["address"], h["dongle"] = h["dongle"], ""
        out.append(h)
    return sorted(out, key=lambda h: h["name"].lower())


class Backend(QObject):
    changed = Signal()
    busyChanged = Signal()
    message = Signal(str, bool)  # text, is error
    monitorsReady = Signal(str, "QVariantList", str)  # a host's address, its monitors, an error
    dongleFound = Signal(str, str, str)  # a host's name, its dongle's address ("" none), what happened
    hostsFound = Signal("QVariantList")  # computers on the network: [{name, address, dongle, added}]
    signedIn = Signal(str, bool, str)  # a host's name, whether it worked, what went wrong
    _signInDone = Signal(str, str, str, str, str, str)  # name, address, dongle, token, pin, error
    _reached = Signal(str, bool)  # a host's address, whether its Web UI answered

    def __init__(self):
        super().__init__()
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._sock.bind("")  # an abstract address ft-screens can reply to
        self._sock.settimeout(1.0)
        self._running = False
        self._states = {}   # remote screen number -> its stream's state ("lost can't connect")
        self._online = {}   # host address -> True/False (None: not checked yet)
        self._queue = []    # ft-layout runs waiting their turn: (label, args)
        self._proc = None
        self._busy = ""
        self._reached.connect(self._host_reached, Qt.QueuedConnection)
        self._signInDone.connect(self._sign_in_done, Qt.QueuedConnection)
        self.poll = QTimer(interval=2000, timeout=self._check)
        self.poll.start()
        self.ping = QTimer(interval=15000, timeout=self._ping_hosts)
        self.ping.start()
        self._check()
        self._ping_hosts()

    # --- state ---
    def _ask(self, text):
        """Request/reply to ft-screens; None if it isn't running."""
        try:
            self._sock.sendto(text.encode(), FT_SCREENS)
            return self._sock.recv(4096).decode()
        except OSError:
            return None

    def _check(self):
        running = os.path.exists(f"/run/user/{os.getuid()}/frametop/wayland-0")
        states = {}
        reply = self._ask("remotes") if running else None
        if reply and reply.startswith("ok"):
            for e in reply.split()[2:]:
                n, state = int(e.split(":")[0]), e.split(":")[2]
                if state in ("lost", "live"):  # why ("lost can't connect"), or which way ("live via dongle")
                    info = self._ask(f"remote {n} info") or ""
                    state = info[3:].strip() if info.startswith("ok ") else state
                states[n] = state
        if running != self._running or states != self._states:
            self._running, self._states = running, states
            self.changed.emit()

    def _ping_hosts(self):
        for h in ft_layout.load_layout().get("hosts", []):
            address = h.get("address", "")
            if address:
                threading.Thread(target=self._ping, args=(address,), daemon=True).start()

    def _ping(self, address):
        try:
            with socket.create_connection((address, WEB_UI_PORT), timeout=2):
                ok = True
        except OSError:
            ok = False
        self._reached.emit(address, ok)

    def _host_reached(self, address, ok):
        if self._online.get(address) != ok:
            self._online[address] = ok
            self.changed.emit()

    @Property(bool, notify=changed)
    def desktopRunning(self):
        return self._running

    @Property(str, notify=busyChanged)
    def busy(self):
        return self._busy

    @Property("QVariantList", constant=True)
    def streamRates(self):
        return STREAM_RATES

    @Property("QVariantList", constant=True)
    def resolutions(self):
        return [{"text": f"{w} × {h}" + (f"  ({t})" if t else ""), "width": w, "height": h} for w, h, t in RESOLUTIONS]

    @Property("QVariantList", notify=changed)
    def hosts(self):
        """Each host, whether it answers, and its displays with their streams' state."""
        layout = ft_layout.load_layout()
        numbers = {d["id"]: n for n, _, d in ft_layout.remote_displays(layout)}
        out = []
        for h in layout.get("hosts", []):
            displays = []
            for d in h.get("displays", []):
                n = numbers.get(d.get("id"), 0)
                w, hh = d.get("size", [2560, 1440])
                if d.get("off"):
                    state = "disconnected"
                elif not self._running:
                    state = "desktop off"
                else:
                    state = self._states.get(n, "not running")
                displays.append({"id": d["id"], "number": n, "label": d.get("label") or d["id"], "app": d["app"],
                                 "virtual": d["app"] == "monitor", "width": int(w), "height": int(hh),
                                 "fps": int(d.get("fps", 60)), "bitrate": int(d.get("bitrate", 0)),
                                 "metres": ft_layout.remote_size(d)[0], "shown": not d.get("hidden"),
                                 "connected": not d.get("off"), "state": state})
            address = h.get("address", "")
            out.append({"name": h.get("name", ""), "address": address, "hasToken": os.path.exists(token_path(address)),
                        "online": self._online.get(address), "route": h.get("route", "auto"),
                        "direct": ", ".join(h.get("direct", [])), "displays": displays})
        return out

    # --- hosts ---
    @Slot()
    def discoverHosts(self):
        """Vibepollo (and Sunshine) computers on the network (answer: hostsFound)."""
        def work():
            try:
                r = subprocess.run(host_command("avahi-browse", "-rpt", "_nvstream._tcp"), capture_output=True,
                                   text=True, timeout=15)
                found = parse_avahi(r.stdout)
            except (OSError, subprocess.SubprocessError):
                found = []
            known = {h.get("address") for h in ft_layout.load_layout().get("hosts", [])}
            for h in found:
                h["added"] = h["address"] in known
            self.hostsFound.emit(found)
        threading.Thread(target=work, daemon=True).start()

    @Slot(str, str, str, str, str)
    def signIn(self, name, address, dongle, user, password):
        """Asks the host for Frametop's API token with its Web UI login (answer: signedIn).
        Over its dongle when it has one that answers, so the password skips the router."""
        name, address, dongle = " ".join(name.split()), address.strip(), dongle.strip()
        if not re.fullmatch(NAME_RE, name):
            return self.signedIn.emit(name, False, "A host's name needs 1 to 40 letters, digits, spaces, dots or dashes")
        if not re.fullmatch(r"[A-Za-z0-9.:-]{1,64}", address) or (dongle and not re.fullmatch(r"[0-9.]{7,15}", dongle)):
            return self.signedIn.emit(name, False, "The address is a host name or an IP address")
        layout = ft_layout.load_layout()
        if any(h.get("name") == name and h.get("address") != address for h in layout.get("hosts", [])):
            return self.signedIn.emit(name, False, f"There's already a host called {name}")

        def work():
            known = None
            try:
                with open(pin_path(address)) as f:
                    known = f.read().strip() or None
            except OSError:
                pass
            error = ""
            for target in ([dongle] if dongle else []) + [address]:
                try:
                    ctx = ssl.create_default_context()
                    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE  # self-signed: pinned below
                    conn = http.client.HTTPSConnection(target, WEB_UI_PORT, timeout=15, context=ctx)
                    conn.connect()
                    pin = spki_pin(conn.sock.getpeercert(binary_form=True))
                    if known and pin != known:
                        conn.close()
                        return self._signInDone.emit(name, address, dongle, "", "", "Its certificate isn't the one "
                                                     "seen when it was added. If Vibepollo was installed again, "
                                                     "remove the host and add it again.")
                    basic = base64.b64encode(f"{user}:{password}".encode()).decode()
                    conn.request("POST", "/api/token", json.dumps({"scopes": SCOPES}),
                                 {"Authorization": "Basic " + basic, "Content-Type": "application/json"})
                    r = conn.getresponse()
                    text = r.read().decode(errors="replace")
                    conn.close()
                    if r.status == 401:
                        return self._signInDone.emit(name, address, dongle, "", "", "Wrong user name or password")
                    token = json.loads(text).get("token") if r.status == 200 else None
                    if not token:
                        return self._signInDone.emit(name, address, dongle, "", "",
                                                     f"It didn't make a token ({r.status} {text[:120]})")
                    return self._signInDone.emit(name, address, dongle, token, pin, "")
                except (OSError, ValueError, ssl.SSLError, http.client.HTTPException,
                        subprocess.SubprocessError) as e:
                    error = str(e) or type(e).__name__
            self._signInDone.emit(name, address, dongle, "", "", f"Couldn't reach it: {error}")
        threading.Thread(target=work, daemon=True).start()

    def _sign_in_done(self, name, address, dongle, token, pin, error):
        if error:
            return self.signedIn.emit(name, False, error)
        write_private(token_path(address), token)
        write_private(pin_path(address), pin)
        layout = ft_layout.load_layout()
        host = next((h for h in layout.setdefault("hosts", []) if h.get("address") == address), None)
        if host is None:
            host = {"name": name, "address": address, "displays": []}
            layout["hosts"].append(host)
        if dongle and dongle not in host.get("direct", []):
            host["direct"] = [dongle] + host.get("direct", [])
        ft_layout.save_layout(layout)
        self.changed.emit()
        threading.Thread(target=self._ping, args=(address,), daemon=True).start()
        self.signedIn.emit(host["name"], True, "")

    @Slot(str)
    def removeHost(self, name):
        layout = ft_layout.load_layout()
        host = next((h for h in layout.get("hosts", []) if h.get("name") == name), None)
        if host is None:
            return
        if host.get("displays"):
            return self.message.emit("Remove its displays first", True)
        layout["hosts"].remove(host)
        ft_layout.save_layout(layout)
        if not any(h.get("address") == host.get("address") for h in layout["hosts"]):
            for path in (token_path(host.get("address", "")), pin_path(host.get("address", ""))):
                try:
                    os.remove(path)
                except OSError:
                    pass
        self.changed.emit()

    @Slot(str, bool)
    def setHostConnected(self, name, on):
        """All of a host's displays at once."""
        layout = ft_layout.load_layout()
        host = next((h for h in layout.get("hosts", []) if h.get("name") == name), None)
        ids = [d["id"] for d in (host or {}).get("displays", []) if bool(d.get("off")) == on]
        if ids:
            self._run(f"{'Connecting' if on else 'Disconnecting'} {name}", "remote", "connect" if on else "disconnect", *ids)

    @Slot(str, str)
    def setRoute(self, name, route):
        self._run(f"Connecting {name} {dict(auto='either way', network='over the network', dongle='over the dongle')[route]}",
                  "remote", "host", name, f"route={route}")

    @Slot(str, str)
    def setDirect(self, name, text):
        addresses = [a for a in re.split(r"[\s,]+", text.strip()) if a]
        if any(not re.fullmatch(r"[A-Za-z0-9.:-]{1,64}", a) for a in addresses):
            return self.message.emit("The dongle's address is an IP address", True)
        self._run(f"Setting {name}'s dongle", "remote", "host", name, "direct=" + (",".join(addresses) or "none"))

    @staticmethod
    def _host_id(address):
        """A host's <uniqueid> from its serverinfo (Vibepollo can take seconds to answer)."""
        url = f"http://{address}:{47989}/serverinfo?uniqueid=0123456789ABCDEF&uuid={uuid.uuid4()}"
        with urllib.request.urlopen(url, timeout=12) as r:
            m = re.search(r"<uniqueid>([^<]+)</uniqueid>", r.read().decode(errors="replace"))
        return m.group(1) if m else None

    @Slot(str)
    def findDongle(self, name):
        """The host among the Frame hotspot's clients (answer: dongleFound)."""
        host = next((h for h in ft_layout.load_layout().get("hosts", []) if h.get("name") == name), None)
        if host is None:
            return

        def work():
            try:
                want = self._host_id(host["address"])
                if not want:
                    return self.dongleFound.emit(name, "", "it didn't say who it is at its own address")
                with open("/proc/net/arp") as f:  # IP, HW type, flags, MAC, mask, device
                    rows = [line.split() for line in f.read().splitlines()[1:]]
                candidates = [r[0] for r in rows if len(r) >= 6 and r[5] == "wlanap" and r[2] != "0x0"]
                for ip in candidates:
                    try:
                        if self._host_id(ip) == want:
                            return self.dongleFound.emit(name, ip, "")
                    except OSError:
                        continue
                self.dongleFound.emit(name, "", "it isn't on the Frame's hotspot" if candidates else
                                      "nothing is on the Frame's hotspot (is Steam Link's dongle paired?)")
            except OSError as e:
                self.dongleFound.emit(name, "", str(e))
        threading.Thread(target=work, daemon=True).start()

    # --- displays ---
    @Slot(str)
    def listMonitors(self, address):
        """The host's monitors, from its Web UI API (answer: monitorsReady)."""
        def work():
            monitors, error = [], ""
            try:
                code, out, err = ft_layout.run_stream("monitors", address, timeout=30)
                if code:
                    error = (err or out).strip().splitlines()[-1] if (err or out).strip() else f"exit {code}"
                else:
                    for m in json.loads(out[out.index("["):]):
                        info = m.get("info") or {}
                        res = info.get("resolution") or {}
                        virtual = (m.get("edid") or {}).get("manufacturer_id") == "SDD" or \
                            str(m.get("friendly_name", "")).startswith("frametop")
                        monitors.append({"app": "display:" + m.get("device_id", ""),
                                         "name": m.get("friendly_name") or m.get("display_name") or "?",
                                         "width": int(res.get("width", 0)), "height": int(res.get("height", 0)),
                                         "primary": bool(info.get("primary")), "active": bool(info),
                                         "virtual": virtual})
            except Exception as e:  # noqa: BLE001  (shown to the user, not raised in a thread)
                error = str(e)
            self.monitorsReady.emit(address, monitors, error)
        threading.Thread(target=work, daemon=True).start()

    @Slot(str, str, str, int, int, int, int)
    def addDisplay(self, host, app, label, width, height, fps, bitrate):
        """Pairs a client for it with the host's token and starts its stream (ft-layout)."""
        layout = ft_layout.load_layout()
        h = next((x for x in layout.get("hosts", []) if x.get("name") == host), None)
        if h is None:
            return self.message.emit(f"No host called {host}", True)
        label = " ".join(label.split()) or ("Virtual" if app == "monitor" else "Display")
        self._run(f"Adding {label} from {host}", "remote", "add", host, h["address"], app, "--label", label,
                  "--size", f"{width}x{height}", "--fps", str(fps), "--bitrate", str(bitrate))

    @Slot(str, bool)
    def setConnected(self, display_id, on):
        self._run("Connecting" if on else "Disconnecting", "remote", "connect" if on else "disconnect", display_id)

    @Slot(str, int, int, int, int)
    def setStream(self, display_id, width, height, fps, bitrate):
        """Its stream's resolution, frame rate and bitrate (it starts over)."""
        self._run("Changing the stream", "remote", "set", display_id, f"size={width}x{height}", f"fps={fps}",
                  f"bitrate={bitrate}")

    def _edit(self, display_id, fn):
        layout = ft_layout.load_layout()
        n, _, d = ft_layout.find_display(layout, display_id)
        fn(d)
        ft_layout.save_layout(layout)
        self.changed.emit()
        return n, d

    @Slot(str, float)
    def setMetres(self, display_id, m):
        n, d = self._edit(display_id, lambda d: d.__setitem__("metres", round(m, 3)))
        if self._running and not d.get("off"):
            self._ask(f"width {n} {m:.3f}")

    @Slot(str, bool)
    def setShown(self, display_id, shown):
        n, d = self._edit(display_id, lambda d: d.pop("hidden", None) if shown else d.__setitem__("hidden", True))
        if self._running and not d.get("off"):
            self._ask(f"{'reveal' if shown else 'conceal'} {n}")

    @Slot(str)
    def removeDisplay(self, display_id):
        self._run("Removing the display", "remote", "remove", display_id)

    # --- ft-layout on the host, one at a time ---
    def _run(self, label, *args):
        self._queue.append((label, args))
        if self._proc is None:
            self._next()

    def _next(self):
        if not self._queue:
            return
        label, args = self._queue.pop(0)
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
        self._check()
        last = out.splitlines()[-1] if out else ""
        if code == 0:
            self.message.emit(f"{label}: done" + (f" ({last})" if last and not last.startswith("ok") else ""), False)
        else:
            self.message.emit(f"{label} failed: {last or 'exit code ' + str(code)}", True)
        self._next()


def main():
    app = QGuiApplication(sys.argv)
    app.setApplicationName("ft-remote-displays")
    app.setApplicationDisplayName("Frametop Remote Displays")
    app.setDesktopFileName("ft-remote-displays")
    if not QIcon.themeName():
        QIcon.setThemeName("breeze")
    QQuickStyle.setStyle("org.kde.desktop")
    engine = QQmlApplicationEngine()
    backend = Backend()
    engine.rootContext().setContextProperty("backend", backend)
    engine.load(QUrl.fromLocalFile(os.path.join(HERE, "main.qml")))
    if not engine.rootObjects():
        sys.exit(1)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
