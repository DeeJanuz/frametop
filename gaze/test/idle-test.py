#!/usr/bin/env python3
"""Offline test of the gaze service idling (gaze/ft-gazed, gaze/gazecheck.py): ft-gaze runs only
while the gaze is in use, and a check asked for while it idles waits for the tracker.

Runs ft-gazed's Service with its sockets renamed, a fake pointer helper (answers "gaze ?
headset" as the test says), and a fake ft-gaze (prints samples, quits when its stdin closes).
SteamVR's tracker is the one in use, so our own isn't started; the panel isn't either. Nothing
reaches the live gaze service, the pointer helper, or SteamVR, so it's safe next to them.

  gaze/test/idle-test.py
"""
import importlib.machinery
import importlib.util
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GAZE = os.path.join(HERE, "..")
sys.path.insert(0, GAZE)
loader = importlib.machinery.SourceFileLoader("ftgazed", os.path.join(GAZE, "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader("ftgazed", loader))
loader.exec_module(gazed)
import gazecheck  # noqa: E402  (the module ft-gazed imported)

tag = f"ft_gaze_idle_test_{os.getpid()}"
gazed.ME = f"\0{tag}_gazed"
gazed.POINTER = gazecheck.POINTER = f"\0{tag}_helper"
gazecheck.SCREENS = f"\0{tag}_screens"
gazecheck.PANEL_PROG = gazed.REPO / "nonexistent-panel"  # "isn't built": no panel
gazed.IDLE_AFTER, gazed.WAKE_SETTLE = 1.0, 3.0
gazed.read_settings = lambda: ("steam", "steam", "auto", 55.0)
gazed.TrackedEye = lambda: lambda now=None: None  # both eyes, whatever SteamVR's settings say
logs = []
gazed.log = gazecheck.log = lambda msg: logs.append(msg)

# The fake ft-gaze: 90 samples a second, both eyes seen, until its stdin closes. It logs the
# sources it was asked for: "argv LIST" (--sources), then "line LIST" for each "sources LIST".
tmp = tempfile.mkdtemp(prefix="ft-gaze-idle-test-")
FAKE = os.path.join(tmp, "ft-gaze")
SOURCES_LOG = os.path.join(tmp, "sources.log")
with open(FAKE, "w") as f:
    f.write('''import json, os, select, sys, time
log = open(sys.argv[1], "a", buffering=1)
log.write("argv " + (sys.argv[sys.argv.index("--sources") + 1] if "--sources" in sys.argv else "-") + "\\n")
buf = b""
while True:
    if select.select([sys.stdin], [], [], 1 / 90)[0]:
        data = os.read(0, 4096)
        if not data:
            break
        buf += data
        while b"\\n" in buf:
            line, buf = buf.split(b"\\n", 1)
            if line.startswith(b"sources "):
                log.write("line " + line[8:].decode() + "\\n")
    eye = {"hy": 1.0, "hp": 2.0}
    print(json.dumps({"t": time.monotonic(), "src": {"mmap1": {"hy": 1.0, "hp": 2.0, "unc": [0.001, 0.001],
          "open": [0.8, 0.8]}, "left": eye, "right": eye}}), flush=True)
''')
started = []


def start_helper(self):
    """ft-gaze, straight from here instead of the dev container."""
    import selectors
    self.proc = subprocess.Popen([sys.executable, FAKE, SOURCES_LOG, "--sources", self.wanted_sources()],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    self.proc_sources = self.wanted_sources()
    os.set_blocking(self.proc.stdout.fileno(), False)
    os.set_blocking(self.proc.stderr.fileno(), False)
    self.sel.register(self.proc.stdout, selectors.EVENT_READ, "stdout")
    self.sel.register(self.proc.stderr, selectors.EVENT_READ, "stderr")
    self.buf = b""
    started.append(time.monotonic())


gazed.Service.start_helper = start_helper

# The fake pointer helper.
helper_state = {"reply": "ok off worn"}
helper = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
helper.bind(gazed.POINTER)
helper.settimeout(0.2)


def answer():
    while True:
        try:
            data, addr = helper.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            return
        if data.startswith(b"gaze ?") and addr:
            helper.sendto(helper_state["reply"].encode(), addr)


threading.Thread(target=answer, daemon=True).start()
svc = gazed.Service(None, False, gazed.POINTER)
threading.Thread(target=svc.run, daemon=True).start()
ctl = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
ctl.bind("")
ctl.settimeout(2)


def ask(cmd):
    ctl.sendto(cmd.encode(), gazed.ME)
    return ctl.recv(4096).decode()


failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def wait(cond, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


time.sleep(2.5)
check("gaze mode off: idle, no ft-gaze", (svc.awake, svc.proc is None), (False, True))
check("status says why", ask("status").count('"idle": "gaze mode is off"'), 1)

helper_state["reply"] = "ok on worn"
check("gaze mode on, headset worn: awake within 2 s", wait(lambda: svc.awake and svc.proc is not None, 2.5), True)
check("samples come in", wait(lambda: svc.counts["samples"] > 20, 2), True)

helper_state["reply"] = "ok on away"
check("headset off: still awake for IDLE_AFTER", wait(lambda: not svc.awake, 0.5), False)
check("then idle, ft-gaze stopped", wait(lambda: not svc.awake and svc.proc is None, 3.5), True)
check("why: nobody wears it", ask("status").count('"idle": "nobody is wearing the headset"'), 1)

helper_state["reply"] = "ok on"
check("an older helper (no headset word): awake with gaze mode on", wait(lambda: svc.awake, 2.5), True)
helper_state["reply"] = "ok off worn"
check("gaze mode off again: idle", wait(lambda: not svc.awake and svc.proc is None, 4.5), True)

check("wake lease", ask("wake 2"), "ok")
check("wake: awake at once", (svc.awake, wait(lambda: svc.proc is not None, 1)), (True, True))
check("lease over (2 s + IDLE_AFTER): idle", wait(lambda: not svc.awake, 4.5), True)

time.sleep(0.5)
logs.clear()
open(SOURCES_LOG, "w").close()
count = len(started)
check("quick check while idle: queued, waking", ask("quickcal"), "ok waking the eye tracker first")
check("it woke", wait(lambda: svc.awake and len(started) > count, 1.5), True)
check("once the tracker sends, it ran (and says why it couldn't open)",
      wait(lambda: any("quickcal, asked for while idle: the panel isn't running" in m for m in logs), 3), True)
check("nothing left pending", svc.checks.pending, None)
sources = open(SOURCES_LOG).read().split("\n")
check("ft-gaze started with every source for the check", sources[0], "argv all")
in_use = svc.wanted_sources()
check("then only those in use (SteamVR's tracker: not the action, not own)",
      (f"line {in_use}" in sources, in_use != "all", "action" in in_use, "own" in in_use), (True, True, False, False))
check("idle again after", wait(lambda: not svc.awake, 3), True)

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
svc.running = False
time.sleep(0.7)
os.remove(FAKE)
os.remove(SOURCES_LOG)
os.rmdir(tmp)
os._exit(1 if failures else 0)
