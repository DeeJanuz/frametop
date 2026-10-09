#!/usr/bin/env python3
"""Record what happens to a key combination, for a report on stuck or leaking keys.

Run it on the Frame (a terminal, or `ssh frame python3 - < scripts/keys-report.py`), then
reproduce the problem while it records (40 seconds by default):
  scripts/keys-report.py [seconds]
It writes ~/frametop-keys-report-<time>.txt with:
  - the input devices, how the relay treats each (role, grabbed), and which programs have
    each one open
  - every press, release, and autorepeat of the modifiers, Tab, and Esc, as the relay reads
    them from the physical keyboards and as they come out of the relay's virtual keyboard
    (what gamescope and SteamVR see). Every other key shows as "other key" with no code, so
    nothing you type is in the report.
  - the relay's and pointer helper's log for the same time, and the end of the Frametop
    desktop's log (where typing went)
Bluetooth addresses and the headset's serial number are masked.
"""
import glob
import json
import os
import re
import select
import socket
import struct
import subprocess
import sys
import time

RELAY = "\0frametop_relay"
SCREENS = "\0ft_screens"
EVENT = struct.Struct("llHHi")
EV_KEY = 0x01
KEYS = {1: "Esc", 15: "Tab", 29: "LeftCtrl", 42: "LeftShift", 54: "RightShift", 56: "LeftAlt",
        97: "RightCtrl", 100: "RightAlt", 125: "LeftMeta", 126: "RightMeta",
        0x110: "BTN_LEFT", 0x111: "BTN_RIGHT", 0x112: "BTN_MIDDLE", 0x113: "BTN_SIDE", 0x114: "BTN_EXTRA"}
VALUES = {0: "up", 1: "down", 2: "repeat"}
HOME = os.path.expanduser("~")
ENV = dict(os.environ, XDG_RUNTIME_DIR=f"/run/user/{os.getuid()}",
           DBUS_SESSION_BUS_ADDRESS=f"unix:path=/run/user/{os.getuid()}/bus")


def run(*cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, env=ENV, timeout=20).stdout.strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"({cmd[0]}: {e})"


def ask(address, text, timeout=3.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind("")  # autobind, so the other side can reply
    s.settimeout(timeout)
    try:
        s.sendto(text.encode(), address)
        return s.recv(65536).decode(errors="replace")
    except OSError as e:
        return f"no answer ({e})"
    finally:
        s.close()


def key_name(code):
    return KEYS.get(code, "other key")


def openers():
    """/dev/input/eventN -> programs (of this user) that have it open."""
    out = {}
    for fd in glob.glob("/proc/[0-9]*/fd/*"):
        try:
            target = os.readlink(fd)
        except OSError:
            continue
        if target.startswith("/dev/input/event"):
            pid = fd.split("/")[2]
            try:
                with open(f"/proc/{pid}/comm") as f:
                    comm = f.read().strip()
            except OSError:
                continue
            out.setdefault(target, set()).add(f"{comm}({pid})")
    return out


def input_devices():
    """(name, event node) of every input device, from /proc/bus/input/devices."""
    try:
        with open("/proc/bus/input/devices") as f:
            blocks = f.read().split("\n\n")
    except OSError:
        return []
    found = []
    for b in blocks:
        name = re.search(r'N: Name="(.*)"', b)
        ev = re.search(r"H: Handlers=.*\b(event\d+)", b)
        if name and ev:
            found.append((name.group(1), "/dev/input/" + ev.group(1)))
    return found


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 40
    lines = []
    out = lines.append
    out(f"Frametop keys report, {time.strftime('%Y-%m-%d %H:%M:%S')}")
    # Run from stdin (over SSH) there's no __file__: try the usual checkouts.
    repos = [os.path.dirname(os.path.dirname(os.path.abspath(__file__)))] if "__file__" in globals() else []
    repos += [os.path.join(HOME, "frametop"), os.path.join(HOME, "dev/frametop")]
    repo = next((r for r in repos if os.path.isdir(os.path.join(r, ".git"))), "")
    out("Frametop: " + (run("git", "-C", repo, "log", "-1", "--format=%h %cd", "--date=short") if repo else "?"))
    out("SteamOS: " + run("sh", "-c", ". /etc/os-release; echo $VERSION_ID build $BUILD_ID"))
    try:
        with open(os.path.join(HOME, ".config/frametop.conf")) as f:
            conf = [ln.split("#")[0].strip() for ln in f]
        out("Settings: " + " ".join(c for c in conf if re.match(r"(POINTER|SHARE_KEYS|BACKEND)=", c)))
    except OSError:
        out("Settings: no ~/.config/frametop.conf")
    out("ft-screens state (visibility, manual, wrist, gesture, lasers, game running, in games): "
        + ask(SCREENS, "state"))

    out("\n===== Input devices (programs with the node open)")
    opened = openers()
    for name, node in input_devices():
        out(f"{node}  {name!r}  <- {', '.join(sorted(opened.get(node, []))) or 'nobody'}")

    out("\n===== Relay: devices, roles, grabs")
    reply = ask(RELAY, "devices")
    try:
        for n in json.loads(reply).get("nodes", []):
            out(f"{n['path']}  {n['name']!r}  bus={n['bus']} kinds={','.join(n['kinds'])} "
                f"role={n['role']} grabbed={n['grabbed']} id={n['id']}")
    except (ValueError, KeyError):
        out(reply)

    # The relay's virtual keyboard: what gamescope and SteamVR get from the relay.
    virtual = []
    for name, node in input_devices():
        if name.startswith("frametop virtual"):
            try:
                virtual.append((os.open(node, os.O_RDONLY | os.O_NONBLOCK), name))
            except OSError as e:
                out(f"can't read {name} ({node}): {e}")

    watch = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    watch.bind("")
    watch.sendto(f"watch {seconds:g}".encode(), RELAY)

    print(f"Recording for {seconds:g} seconds. Now, in the Frametop desktop:")
    print("  1. Click into a text field in Chromium, type a few letters.")
    print("  2. Press Shift+Tab once, and let go.")
    print("  3. Close the SteamVR dashboard if it opened, click back into Chromium, and see if Shift or Tab is still held.")
    print("  4. If it is, press and release Shift, then Tab, on their own, and check again.")
    print("Then wait here until it says it's done.", flush=True)
    start_wall = time.time()
    start = time.monotonic()
    events = []
    while (left := start + seconds - time.monotonic()) > 0:
        ready, _, _ = select.select([watch] + [fd for fd, _ in virtual], [], [], min(left, 1.0))
        t = time.monotonic() - start
        for r in ready:
            if r is watch:
                try:
                    m = json.loads(watch.recv(65536).decode(errors="replace"))
                except (OSError, ValueError):
                    continue
                if m.get("t") == "event" and m.get("type") == "key":
                    events.append(f"{t:7.3f}  relay reads   {m['name']!r:40} {key_name(m['code']):12} "
                                  f"{VALUES.get(m['value'], m['value'])}")
                continue
            name = next(n for fd, n in virtual if fd == r)
            try:
                data = os.read(r, EVENT.size * 64)
            except OSError:
                continue
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, etype, code, value = EVENT.unpack_from(data, off)
                if etype == EV_KEY:
                    events.append(f"{t:7.3f}  relay sends  {name!r:40} {key_name(code):12} "
                                  f"{VALUES.get(value, value)}")
    for fd, _ in virtual:
        os.close(fd)
    print("Done. Writing the report.", flush=True)

    out(f"\n===== Keys during the recording (seconds from the start, {time.strftime('%H:%M:%S', time.localtime(start_wall))})")
    out("relay reads: from a physical device. relay sends: out of the relay's virtual devices,"
        " to gamescope and SteamVR. Keys typed into the desktop go to ft-screens, not listed here.")
    lines.extend(events or ["(no keys)"])

    since = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(start_wall - 5))
    for unit in ("frametop-input-relay", "frametop-pointer"):
        out(f"\n===== {unit} log")
        out(run("journalctl", "--user", "-u", unit, "--since", since, "--no-pager", "-o", "short-precise"))
    started = run("sh", "-c", "ps -o lstart= -p $(pgrep -x ft-screens | head -1) 2>/dev/null")
    out(f"\n===== Frametop desktop log (last 40 lines; times count from its start, {started or 'not running'})")
    try:
        with open("/tmp/frametop-screens.log", errors="replace") as f:
            lines.extend(ln.rstrip() for ln in f.readlines()[-40:])
    except OSError:
        out("missing")
    text = "\n".join(lines) + "\n"
    text = re.sub(r"([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", "xx:xx:xx:xx:xx:xx", text)
    text = re.sub(r"cv\.[A-Z0-9]{8,}", "cv.<serial>", text)
    path = os.path.join(HOME, f"frametop-keys-report-{time.strftime('%Y%m%d-%H%M%S')}.txt")
    with open(path, "w") as f:
        f.write(text)
    print(f"Wrote {path}")
    print("Attach it to https://github.com/Frametop/frametop/issues/2 with what you saw at each step.")


if __name__ == "__main__":
    main()
