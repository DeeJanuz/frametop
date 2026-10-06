"""gazecheck: the gaze service's checks and calibration, in the panel fixed to the headset
(gaze/panel/ft-gazepanel; ft-gazed runs it). Every kind is made of dots shown at head-relative
directions: look at each one.

  quick   one dot in the middle of your view. It opens when the headset goes on: eyes seen
          for DON_DELAY after none for AWAY_MIN (SteamVR's tracker's variance for an eye under
          EYE_LOST). SteamVR's "HMD on" can't say: it repeats every minute or so, and it can
          stay on for hours with nobody in the headset. It also opens when our own tracker asks
          for a click (its "reseat": the headset may sit differently on your face now), at most
          once every QUICK_COOLDOWN, and on "quickcal" (Frametop Input Settings, or a mouse
          button or key combination mapped to Gaze quick check), and when a click's correction
          was past POINTER_GAZE_NUDGE_MAX (55 degrees; the helper's "recheck"): the tracker is
          far off. Ignored, it closes after QUICK_TIMEOUT and changes nothing. The headset going
          on is seen only while the gaze service is awake (gaze mode on, someone wearing it: see
          ft-gazed), so it also opens when gaze mode comes on after the service idled.
  five    the middle and four around it, when the first FIVE_COUNT lessons after a quick check
          were all over FIVE_LIMIT degrees off: the quick check didn't fix it.
  full    the calibration, as the gaze probe's: three rounds, dark, medium and bright (pupil
          size, and the tracker's error with it, changes with brightness), each the middle and
          a ring of six (SteamVR's tracker) or eight (ours, whose fit goes wrong past its dots)
          RING degrees out, half that in the middle round, turned 20 degrees a round. It opens
          whenever gaze mode is on without a calibration for the tracker in use and someone's
          in the headset, and on "calibrate". One that closes unfinished (ignored, too few
          dots) opens again only once the headset comes off and on, or gaze mode off and on.
          Frametop's screens hide while it runs. Quitting it while there's still no
          calibration turns gaze mode off (POINTER_GAZE=0); turning it on again reopens it.
          Why gaze mode can't work yet goes in the status ("problem"), for Input Settings.

  fit     the headset fit check (on "fitcheck", Check headset fit on the Gaze page): live, a
          card per eye (tracked or lost, the tracker's signal, how much of the last 10 s it was
          seen) and hints, from the gaze probe's Headset fit (gaze/fitcheck.py), while you
          adjust the headset. A left click or Meta+J runs its guided check (dots, then looks
          down, up, left and right); a right click or Meta+K closes it, as does FIT_TIMEOUT.

A check asked for while the gaze service idles (quickcal, calibrate, fitcheck) wakes it and
waits until the tracker sends, at most ft-gazed's WAKE_SETTLE; then it opens, or logs why not.

The quick check's dot captures itself: from CHECK_SETTLE after it shows (the eyes getting
there), once the gaze has held within CHECK_SPREAD for CHECK_WINDOW (the probe's max spread and
capture time). It's the gaze holding still that counts, not where the tracker puts it, so it
works however far off the tracker is; a left click or Meta+J (the pointer helper's
"calaccept") takes it now. The full calibration's and five's dots wait for that click: you
click when you're looking at the dot (the user asked for that: a steady gaze isn't always on
the dot), and the gaze held still up to then is taken (ACCEPT_SPREAD). They wait as long as
it takes, up to CLICK_IDLE. A dot not taken says why in the panel's note line (reject_reason:
gazecal.steady_samples' drop counts for SteamVR's tracker, ft-eyes' reply for ours), as does a
click with nothing taken after ACCEPT_WAIT, and a failed calibration names its most common
reason there and in the status. A right click or Meta+K ("calquit") closes the panel. The pointer hides meanwhile ("calpanel 1",
renewed every second; the helper shows it again by itself when that stops).

Our tracker's first calibration: before it has one, ft-eyes publishes no gaze (it maps pupils
to a gaze only with a calibration), so there's no gaze to hold still. Its calibration runs
anyway ("blind"): someone in the headset (SteamVR's tracker sees an eye) and ft-eyes answering
are enough to start it, each dot stands in for the gaze, and a click takes the CHECK_WINDOW up
to it. ft-eyes then checks that each pupil was seen and held still in that window (calib-point)
and says why not. Without this, a fresh install could never calibrate our tracker.

What a capture teaches:
  our tracker   quick and five: a click ("click T YAW PITCH", like a pointer lesson); full:
                calib-start, a calib-point for each dot, calib-fit (its calibration)
  SteamVR's     quick and five: a lesson for each eye, like the pointer's; full: each source's
                calibration fitted from the dots (gazecal.Correction, the probe's way), saved
                to calibration.json, and the lessons start over on top of it
Either way, how far off each eye was goes to the eye bias, and each capture to the lesson log.
"""

import json
import math
import os
import selectors
import signal
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path

from fitcheck import MIN_REGION, FitCheck, wrap
from gazecal import DEFAULT_MODEL, EYE_LOST, STATE, steady_samples

REPO = Path(__file__).resolve().parents[1]
PANEL_PROG = REPO / "gaze" / "build" / "ft-gazepanel"
PANEL = "\0ft_gazepanel"
POINTER = "\0ft_pointer_helper"
SCREENS = "\0ft_screens"
EYES = "\0ft_eyes"
CONF = Path.home() / ".config" / "frametop.conf"
CALIBRATION = STATE / "calibration.json"
POINTS = STATE / "points.jsonl"   # the probe's record of calibration dots, for its refine
CHECK_LOG = STATE / "checks.jsonl"

CHECK_SETTLE = 0.45
CHECK_WINDOW = 0.6
CHECK_SPREAD = 1.0     # degrees
ACCEPT_SPREAD = 2.5    # degrees: a capture asked for (calaccept) takes this much
CLICK_IDLE = 120.0     # seconds a dot of five or full waits for its click; then the check closes
QUICK_TIMEOUT = 6.0
QUICK_COOLDOWN = 120.0
DON_DELAY = 3.0        # seconds of eyes after AWAY_MIN without: the headset went on
AWAY_MIN = 3.0
EYES_GONE = 2.0        # seconds without eyes that close a check: the headset came off
FIVE_LIMIT = 2.0
FIVE_COUNT = 3
DONE_PAUSE = 0.35      # seconds the filled dot shows before the next
RING = 20.0
ROUND_BG = (0.03, 0.33, 0.8)
ROUND_NAMES = ("dark", "medium", "bright")
RING_SCALE = (1.0, 0.5, 1.0)
PANEL_RETRY = 10.0
FULL_RETRY = 10.0      # seconds before an automatic calibration that failed to start tries again
FIT_TIMEOUT = 300.0    # seconds the fit check stays up
FIT_EVERY = 0.5        # seconds between its cards' updates (each is a new picture for the panel)
FIT_HINT_WIDTH = 95    # characters a hint line holds in the panel


def log(msg):
    print(f"ft-gazed: {msg}", file=sys.stderr, flush=True)


def check_dots(kind, own):
    """(yaw, pitch, round): head-relative degrees, yaw +left, pitch +up."""
    if kind == "quick":
        return [(0.0, 0.0, 0)]
    if kind == "five":
        return [(0.0, 0.0, 0), (12.0, 0.0, 0), (-12.0, 0.0, 0), (0.0, 9.0, 0), (0.0, -9.0, 0)]
    out = []
    n = 8 if own else 6
    for rnd in range(3):
        out.append((0.0, 0.0, rnd))
        r = RING * RING_SCALE[rnd]
        for i in range(n):
            a = math.radians(-90 + rnd * 20 + i * 360 / n)  # as the probe: x right, y down
            x, y = r * math.cos(a), r * math.sin(a) * (0.9 if own else 1.0)
            out.append((-x, -y, rnd))
    return out


# Why SteamVR's samples for a look were dropped (gazecal.steady_samples' counts) -> (short, long):
# short for the small panel, long for the calibration's. FIT_HINT goes after the ones the
# headset's fit causes, in the calibration's panel.
STEAM_REASONS = {"lost_left": ("left eye lost", "SteamVR lost your left eye"),
                 "lost_right": ("right eye lost", "SteamVR lost your right eye"),
                 "lost_both": ("both eyes lost", "SteamVR lost both eyes"),
                 "blink": ("blinked", "you blinked"),
                 "vergence": ("eyes disagreed", "SteamVR's two eyes disagreed")}
FIT_HINT = "check the headset fit"
ACCEPT_WAIT = 1.5      # seconds after a click with no capture before the panel says what it waits for


def reject_reason(reply, why):
    """Why a dot wasn't taken -> (short, long, fit): our tracker's reply (`reply`), or SteamVR's
    drop counts (`why`, when `reply` is None). `fit`: the headset's fit is the likely cause."""
    if reply is None:
        if not why:
            return "no reading", "SteamVR sent no reading for that look", False
        key = max(why, key=why.get)
        return *STEAM_REASONS[key], key.startswith("lost")
    words = reply.removeprefix("fail ").split()
    # ft-eyes: "fail the left eye was seen in only 3 frames", "fail the left eye moved (4.2 px)"
    if len(words) >= 3 and words[0] == "the" and words[2] == "eye":
        eye = words[1]
        if "seen" in words:
            return f"{eye} eye not seen", f"our tracker saw your {eye} eye in only {words[-2]} frames", True
        if "moved" in words:
            return f"{eye} eye moved", f"your {eye} eye moved while you looked", False
    if not reply:
        return "no answer", "our tracker didn't answer", False
    if reply.startswith("our tracker isn't running"):
        return "tracker not running", "our tracker isn't running", False
    text = reply.removeprefix("fail ")
    return text[:24], f"our tracker said: {text}", False


def spread(points):
    """The median point and the spread around it (1.4826 x the median distance: a standard
    deviation that one stray sample can't move far)."""
    mx = statistics.median(p[0] for p in points)
    my = statistics.median(p[1] for p in points)
    return 1.4826 * statistics.median(math.hypot(p[0] - mx, p[1] - my) for p in points), (mx, my)


def write_gaze(on, path=CONF):
    """POINTER_GAZE=1|0 in the config, every other line kept."""
    try:
        lines = path.read_text().splitlines()
    except OSError:
        lines = []
    value = f"POINTER_GAZE={1 if on else 0}"
    for i, line in enumerate(lines):
        if line.split("#", 1)[0].split("=", 1)[0].strip() == "POINTER_GAZE":
            lines[i] = value
            break
    else:
        lines.append(value)
    tmp = path.with_suffix(".tmp")
    tmp.write_text("\n".join(lines) + "\n")
    tmp.replace(path)


def ask(addr, command, timeout=1.0):
    """A command to a local datagram socket, and its reply ("" without one)."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
    try:
        s.bind("")
        s.settimeout(timeout)
        s.sendto(command.encode(), addr)
        return s.recv(4096).decode("utf-8", "replace")
    except OSError:
        return ""
    finally:
        s.close()


class Checks:
    def __init__(self, svc, sel):
        self.svc = svc
        self.sel = sel
        self.out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC | socket.SOCK_NONBLOCK)
        self.out.bind("")  # the panel's and the helper's replies come back here
        sel.register(self.out, selectors.EVENT_READ, "checks")
        self.check = None
        self.gaze_on = None
        self.headset = None      # someone wears it (the helper's "worn"), False "away", None unknown
        self.gaze_heard = 0.0
        self.pending = None      # (command words, when): asked for while the gaze service idled
        self.full_armed = True    # gaze mode on without a calibration opens the full one (need_full)
        self.full_blocked = None  # why it can't open now
        self.full_retry_at = 0.0
        self.full_failed = None   # why the last calibration failed (too few dots), for problem()
        self.last_quick = 0.0
        self.sample_at = 0.0     # the tracker last sent anything
        self.seen_at = 0.0       # eyes last seen (SteamVR's tracker's variance for them, "unc")
        self.away = True         # no eyes for AWAY_MIN: their coming back is the headset going on
        self.back_since = None
        self.reseat_seen = False
        self.after_quick = None
        self.panel_proc = None
        self.panel_restart_at = 0.0
        self.screens_shown = None
        self.last_progress = 0.0

    @property
    def active(self):
        return self.check is not None

    # --- The panel process ---

    def start_panel(self):
        if not PANEL_PROG.exists():
            log(f"ft-gazepanel isn't built: run {REPO}/gaze/build.sh")
            self.panel_restart_at = time.monotonic() + 60
            return
        env = dict(os.environ)
        env["XDG_RUNTIME_DIR"] = f"/run/user/{os.getuid()}"
        distrobox = Path.home() / ".local" / "bin" / "distrobox"
        self.panel_proc = subprocess.Popen([str(distrobox), "enter", "dev", "--", str(PANEL_PROG), "--watch-stdin"],
                                           env=env, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                           stderr=subprocess.PIPE, start_new_session=True)
        os.set_blocking(self.panel_proc.stderr.fileno(), False)
        self.sel.register(self.panel_proc.stderr, selectors.EVENT_READ, "panel")
        log("ft-gazepanel started")

    def read_panel(self):
        try:
            data = os.read(self.panel_proc.stderr.fileno(), 65536)
        except BlockingIOError:
            return
        if not data:
            log(f"ft-gazepanel stopped (exit {self.panel_proc.poll()}); again in {PANEL_RETRY:.0f} s")
            self.stop_panel()
            self.panel_restart_at = time.monotonic() + PANEL_RETRY
            if self.check:
                self.close("the panel stopped")
            return
        for line in data.decode("utf-8", "replace").splitlines():
            if line.strip():
                log(line)

    def stop_panel(self):
        if not self.panel_proc:
            return
        try:
            self.sel.unregister(self.panel_proc.stderr)
        except (KeyError, ValueError):
            pass
        if self.panel_proc.stdin and not self.panel_proc.stdin.closed:
            self.panel_proc.stdin.close()
        try:
            self.panel_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.panel_proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        self.panel_proc = None

    def to_panel(self, command):
        try:
            self.out.sendto(command.encode(), PANEL)
        except OSError:
            pass

    def to_helper(self, command):
        try:
            self.out.sendto(command.encode(), POINTER)
        except OSError:
            pass

    def on_readable(self):
        """Replies on our socket: the helper's "ok on|off [worn|away]" to "gaze ? headset" (an
        older helper leaves the headset out); the panel's are dropped."""
        while True:
            try:
                words = self.out.recv(4096).decode("utf-8", "replace").split()
            except (BlockingIOError, OSError):
                return
            if words[:1] == ["ok"] and len(words) in (2, 3) and words[1] in ("on", "off"):
                on = words[1] == "on"
                headset = None if len(words) == 2 else words[2] == "worn"
                was = (self.gaze_on, self.headset)
                self.gaze_on, self.headset, self.gaze_heard = on, headset, time.monotonic()
                if on and was[0] is False:
                    self.on_gaze_on()
                if was != (on, headset):
                    self.svc.update_awake()

    # --- State ---

    def calibrated(self):
        """Does the tracker in use have a calibration? None: our tracker hasn't said yet."""
        svc = self.svc
        kind = svc.kind
        if kind == "own":
            if time.monotonic() - svc.own_at > 5 or not svc.own:
                return None
            return bool((svc.own.get("calibration") or {}).get("dots"))
        if kind == "eyes":
            return True
        return svc.models[svc.source].samples > 0

    def eyes_seen(self, within=1.0):
        return time.monotonic() - self.seen_at < within

    def can_run(self):
        """Someone's in the headset and the tracker is sending. Our tracker sends no gaze before
        its first calibration: then ft-eyes answering (calibrated() is False only once it has)
        is enough, since the calibration is what it needs (see "first calibration" at the top)."""
        if self.blind():
            return self.eyes_seen()
        return self.eyes_seen() and time.monotonic() - self.svc.last_sample < 2

    def blind(self):
        """Our tracker is in use, running, and not calibrated yet: it has no gaze to send."""
        return self.svc.kind == "own" and self.calibrated() is False

    def on_gaze_on(self):
        self.full_armed = True
        self.need_full("gaze mode came on without a calibration")

    def need_full(self, reason):
        """Gaze mode is on without a calibration: open the full one, once per arming (see the top),
        or note why it can't open."""
        cal = self.calibrated()
        if cal:
            self.full_armed = True  # missing one later (the other tracker picked) is news again
        if not self.gaze_on or self.check or not self.full_armed or cal is not False:
            self.full_blocked = None
            return
        now = time.monotonic()
        if not self.can_run() and self.svc.waking():
            return  # the tracker is still starting (the service idled)
        if not self.can_run():
            why = ("the eye tracker isn't sending" if now - self.svc.last_sample >= 2 and not self.blind()
                   else "no eyes seen (is the headset on?)")
        elif now < self.full_retry_at:
            return
        else:
            reply = self.start("full", reason)
            why = None if reply == "ok" else reply.removeprefix("error ")
            if why:
                self.full_retry_at = now + FULL_RETRY
        if why and why != self.full_blocked:
            log(f"the calibration can't open: {why}")
        self.full_blocked = why
        if not why:
            self.full_armed = False

    def problem(self):
        """Why gaze mode, on, can't follow your eyes yet, or None. Our tracker not having said
        yet is None: Input Settings has its own line for our tracker."""
        if not self.gaze_on:
            return None
        own = self.svc.kind == "own"
        if self.svc.mmap_unknown and (not own or self.calibrated() is False):
            # ft-gaze can't read SteamVR's tracker (EyeFile::Detect in ft-gaze.cpp): not its gaze,
            # and not the eyes it sees, which our tracker's calibration waits for.
            return ("SteamVR's eye data has a layout Frametop doesn't know (after a SteamOS update?), so "
                    + ("the calibration can't open" if own else "SteamVR's eye tracker can't be used")
                    + ". A newer Frametop may know it")
        if self.calibrated() is not False:
            return None
        if self.check and self.check["kind"] == "full":
            return "Not calibrated yet: the calibration is open in the headset"
        if self.full_blocked:
            return f"Not calibrated, and the calibration can't open: {self.full_blocked}"
        if not self.full_armed:
            if self.full_failed:
                return f"Not calibrated: the calibration failed (most dots: {self.full_failed}). Use Calibrate"
            return "Not calibrated: the calibration closed unfinished. Use Calibrate"
        return "Not calibrated: the calibration opens in the headset"

    def auto_quick(self, reason):
        now = time.monotonic()
        if self.check or not self.gaze_on or not self.can_run() or self.calibrated() is not True:
            return
        if now - self.last_quick < QUICK_COOLDOWN:
            log(f"quick check skipped ({reason}): one ran {now - self.last_quick:.0f} s ago")
            return
        self.start("quick", reason)

    # --- Running a check ---

    def start(self, kind, reason):
        svc = self.svc
        if self.check:
            return "error a check is running"
        if not self.panel_proc:
            return "error the panel isn't running (gaze/build.sh builds it)"
        if kind == "fit":
            return self.start_fit(reason)
        if not self.can_run():
            return "error the headset is off or the tracker isn't sending"
        own = svc.kind == "own"
        blind = self.blind()
        if blind and kind != "full":
            return "error our tracker isn't calibrated yet: use Calibrate"
        if kind == "full" and own:
            reply = ask(EYES, "calib-start", 3.0)
            if not reply.startswith("ok"):
                log(f"calibration not started: our tracker says {reply or 'nothing'}")
                return f"error our tracker: {reply or 'no reply'}"
        now = time.monotonic()
        self.check = {"kind": kind, "reason": reason, "own": own, "dots": check_dots(kind, own), "i": 0,
                      "started": now, "shown": now, "run": [], "accept": False, "done_at": None, "tries": 0,
                      "skipped": 0, "captured": 0, "points": {}, "reasons": {}, "fit_reasons": set(), "note": "",
                      "blind": blind}
        log(f"{kind} check: {reason}" + (" (our tracker's first: no gaze yet, so each click takes the look "
                                         "up to it)" if blind else ""))
        if kind == "full":
            st = ask(SCREENS, "state", 0.5).split()
            if len(st) >= 3 and st[0] == "ok":
                self.screens_shown = (st[2] == "0") if st[1] == "always" else (st[2] == "1")
                ask(SCREENS, "hide", 0.5)
        self.to_helper("calpanel 1")
        self.to_panel(f"show {'full' if kind == 'full' else 'quick'}")
        self.show_dot()
        if kind == "quick":
            self.last_quick = now
        return "ok"

    def start_fit(self, reason):
        # Eyes lost are what it's there to show, so it needs only the tracker sending.
        if time.monotonic() - self.sample_at > 2:
            return "error the headset is off or the eye tracker isn't sending"
        now = time.monotonic()
        self.check = {"kind": "fit", "reason": reason, "own": False, "dots": [], "i": 0, "started": now, "shown": now,
                      "run": [], "accept": False, "done_at": None, "tries": 0, "skipped": 0, "captured": 0,
                      "points": {}, "fit": FitCheck(), "drawn": {}, "drawn_at": 0.0, "step": None}
        log(f"fit check: {reason}")
        self.to_helper("calpanel 1")
        self.to_panel("show fit")
        self.to_panel("title Headset fit: adjust the headset while you watch")
        self.to_panel("text Left click or Meta+J: guided check · Right click or Meta+K: done")
        return "ok"

    def fit_tick(self, now):
        c = self.check
        fit = c["fit"]
        # The guided check: its dots at head-relative directions in the panel (the probe's
        # screen fractions, spread over the middle of the panel), and its looks as the title.
        step = fit.guide_step(now)
        key = None if step is None else (step[0], step[1])
        if key != c["step"]:
            c["step"] = key
            if step is None:
                self.to_panel("dot 0 0 off")
                self.to_panel("title Headset fit: adjust the headset while you watch")
            elif step[0] == "dot":
                fx, fy = step[1]
                self.to_panel(f"dot {(0.5 - fx) * 32:.2f} {(0.5 - fy) * 24:.2f} look")
                self.to_panel("title Look at the dot")
            else:
                self.to_panel("dot 0 0 off")
                self.to_panel(f"title {step[1]}")
        if now - c["drawn_at"] < FIT_EVERY:
            return
        c["drawn_at"] = now
        drawn = c["drawn"]
        for k in (0, 1):
            word, (r, g, b) = fit.status(k)
            sig, seen = fit.signal(k), fit.tracked_share(k, now)
            cmd = (f"eye {k} {r:.2f} {g:.2f} {b:.2f} {-1 if sig is None else round(sig, 1):g} "
                   f"{-1 if seen is None else round(seen * 20) / 20:g} {word.capitalize()}")
            if drawn.get(k) != cmd:
                drawn[k] = cmd
                self.to_panel(cmd)
        if fit.have_eye_data and fit.samples < 3 * MIN_REGION:
            hints = ["Look around slowly: up, down, left and right. Or left click (Meta+J) for a guided check."]
        else:
            hints = fit.hints()
        lines = [line for h in hints for line in wrap(h, FIT_HINT_WIDTH)][:8]
        cmd = "hints " + "|".join(lines)
        if drawn.get("hints") != cmd:
            drawn["hints"] = cmd
            self.to_panel(cmd)

    def show_dot(self):
        c = self.check
        yaw, pitch, rnd = c["dots"][c["i"]]
        if c["kind"] == "full":
            self.to_panel(f"bg {ROUND_BG[rnd]}")
            self.to_panel(f"title Gaze calibration: {ROUND_NAMES[rnd]} round, {rnd + 1} of 3")
            self.to_panel("text Look at the dot and click (left click or Meta+J). Right click or Meta+K: stop")
        elif c["kind"] == "five":
            self.to_panel(f"text Look at the dot and click ({c['i'] + 1} of {len(c['dots'])})")
        else:
            self.to_panel("text Look at the dot")
        self.to_panel(f"dot {yaw:.3f} {pitch:.3f} look")
        self.last_progress = None
        c["shown"] = time.monotonic()
        c["run"], c["accept"], c["done_at"], c["accept_at"] = [], False, None, None

    def on_sample(self, s):
        self.sample_at = time.monotonic()
        if self.pending:
            self.run_pending()
        unc = (s["src"].get("mmap1") or {}).get("unc")
        if unc and min(unc) <= EYE_LOST:
            self.seen_at = time.monotonic()
            if self.away and self.back_since is None:
                self.back_since = self.seen_at
        c = self.check
        if c and c["kind"] == "fit":
            c["fit"].feed(s, self.sample_at)
            return
        if not c or c["done_at"]:
            return
        if c["own"]:
            src = s["src"].get("own") or {}
            if c["blind"]:
                # Our tracker's first calibration: no gaze yet, so the dot stands in for it (the
                # gaze can't seem to move) and a click takes the look up to it. ft-eyes checks
                # the pupils held still (see the top).
                yaw, pitch, _ = c["dots"][c["i"]]
                src = {"hy": yaw, "hp": pitch}
        else:
            src = s["src"].get("mmap1") or {}
            if "hy" not in src:
                per = [s["src"].get(n) or {} for n in ("left", "right")]
                per = [p for p in per if "hy" in p]
                src = {"hy": statistics.fmean(p["hy"] for p in per), "hp": statistics.fmean(p["hp"] for p in per)} if per else {}
        if "hy" not in src:
            return
        now = time.monotonic()
        c["gaze_at"] = now
        if now < c["shown"] + CHECK_SETTLE:
            return
        g = (src["hy"], src["hp"])
        run = c["run"]
        if run:
            recent = run[-30:]
            my, mp = statistics.median(p[2] for p in recent), statistics.median(p[3] for p in recent)
            if math.hypot(g[0] - my, g[1] - mp) > 2.5 * CHECK_SPREAD:
                run.clear()  # the eyes moved on (a saccade, a blink): start over
        run.append((now, s, g[0], g[1]))
        window = [p for p in run if p[0] >= now - CHECK_WINDOW]
        held = now - run[0][0]
        # The quick check's ring fills in quarters: each step is a new picture for the panel, so
        # few of them keep it solid. The others wait for the click (see the top): no ring.
        progress = math.floor(min(1.0, held / CHECK_WINDOW) * 4) / 4
        if c["kind"] == "quick" and progress != self.last_progress:
            yaw, pitch, _ = c["dots"][c["i"]]
            self.to_panel(f"dot {yaw:.3f} {pitch:.3f} capture {progress:.2f}")
            self.last_progress = progress
        if c["accept"] and held >= 0.3 and len(window) >= 10:
            sd, _ = spread([(p[2], p[3]) for p in window])
            if sd <= ACCEPT_SPREAD:
                self.capture(window)
            return
        if c["kind"] == "quick" and held >= CHECK_WINDOW and len(window) >= 20:
            sd, _ = spread([(p[2], p[3]) for p in window])
            if sd <= CHECK_SPREAD:
                self.capture(window)
            else:
                del run[:len(run) // 2]  # not steady enough yet: keep trying with the newer half

    def capture(self, window):
        svc = self.svc
        c = self.check
        yaw, pitch, rnd = c["dots"][c["i"]]
        samples = [p[1] for p in window]
        rec = {"time": time.time(), "check": c["kind"], "dot": c["i"], "round": rnd, "true": [yaw, pitch],
               "samples": len(samples), "own": c["own"]}
        ok = True
        if c["own"]:
            eyes = []
            for k in (0, 1):
                seen = [smp["src"]["own"]["eyes"][k] for smp in samples
                        if (smp["src"].get("own") or {}).get("eyes") and smp["src"]["own"]["eyes"][k]]
                eyes.append((statistics.median(e[0] for e in seen), statistics.median(e[1] for e in seen)) if seen else None)
            miss = [math.hypot(yaw - e[0], pitch - e[1]) if e else None for e in eyes]
            rec.update(eyes=eyes, miss=miss)
            t0, t1 = samples[0]["t"], samples[-1]["t"]
            if c["kind"] == "full":
                reply = ask(EYES, f"calib-point {t0:.6f} {t1:.6f} {yaw:.4f} {pitch:.4f}", 3.0)
                rec["reply"] = reply
                ok = reply.startswith("ok")
            else:
                try:
                    svc.eyes_sock.sendto(f"click {t1:.6f} {yaw:.4f} {pitch:.4f}".encode(), EYES)
                except OSError as e:
                    rec["reply"], ok = f"our tracker isn't running ({e})", False
            if ok:
                svc.weights["own"].add(miss)
        else:
            why = {}
            steady = steady_samples(samples, why=why)
            rec["dropped"] = why
            reads = {}
            for name in ("action", "mmap1", "mmap2", "left", "right"):
                pts = [(smp["src"][name]["hy"], smp["src"][name]["hp"]) for smp in steady
                       if "hy" in (smp["src"].get(name) or {})]
                if len(pts) >= 15:
                    reads[name] = (statistics.median(p[0] for p in pts), statistics.median(p[1] for p in pts))
            rec["reads"] = reads
            main = ("left", "right") if svc.kind == "eyes" else (svc.source,)
            if not all(n in reads for n in main):
                ok = False
                rec["reply"] = f"only {len(steady)} of {len(samples)} samples had both eyes"
            elif c["kind"] == "full":
                for name, (hy, hp) in reads.items():
                    c["points"].setdefault(name, []).append((hy, hp, yaw - hy, pitch - hp))
                try:
                    with open(POINTS, "a") as f:
                        for name, (hy, hp) in reads.items():
                            f.write(json.dumps({"time": time.time(), "source": name, "hy": hy, "hp": hp,
                                                "off": [yaw - hy, pitch - hp], "layout": None, "how": "panel"}) + "\n")
                except OSError:
                    pass
            else:
                miss = []
                for name in main:
                    hy, hp = reads[name]
                    cy, cp = svc.correction(name, hy, hp)
                    miss.append(math.hypot(yaw - hy - cy, pitch - hp - cp))
                    svc.lives[name].add({"time": time.time(), "hy": hy, "hp": hp, "dy": yaw - hy, "dp": pitch - hp,
                                         "wy": 1.0, "wp": 1.0, "how": "check"}, svc.models[name], svc.mode)
                rec["miss"] = miss
                if svc.kind == "eyes":
                    svc.weights["steam"].add(miss)
                svc.dirty = True
        if not ok:
            short, long, fit = reject_reason(rec.get("reply", "") if c["own"] else None, rec.get("dropped"))
            rec["reason"] = long
            c["reasons"][long] = c["reasons"].get(long, 0) + 1
            if fit:
                c["fit_reasons"].add(long)
        rec["accepted"] = ok
        self.log_check(rec)
        if not ok:
            c["tries"] += 1
            log(f"{c['kind']} check dot {c['i'] + 1}: not taken: {rec['reason']} ({rec.get('reply', '')})")
            full = c["kind"] == "full"
            if c["tries"] >= 2 or not full:
                self.note(f"Dot skipped: {long}" + (f" ({FIT_HINT})" if fit else "") if full else f"Skipped: {short}")
                self.skip()
            else:
                self.note(f"Not taken: {long}. Look at the dot and click again")
                self.to_panel(f"dot {yaw:.3f} {pitch:.3f} fail")
                c["run"], c["accept"], c["accept_at"] = [], False, None
                c["shown"] = time.monotonic()  # settle again, then retry
            return
        c["captured"] += 1
        c["tries"] = 0
        self.note("")
        self.to_panel(f"dot {yaw:.3f} {pitch:.3f} done")
        c["done_at"] = time.monotonic() + DONE_PAUSE

    def note(self, text):
        """The panel's warning line, over the instructions (empty: none). It stays until the
        next dot is taken, so a skipped dot's reason is still there at the one after it."""
        c = self.check
        if c.get("note") != text:
            c["note"] = text
            self.to_panel(f"note {text}".rstrip())

    def skip(self):
        c = self.check
        yaw, pitch, _ = c["dots"][c["i"]]
        c["skipped"] += 1
        c["tries"] = 0
        self.to_panel(f"dot {yaw:.3f} {pitch:.3f} fail")
        c["done_at"] = time.monotonic() + DONE_PAUSE

    def advance(self):
        c = self.check
        c["i"] += 1
        if c["i"] < len(c["dots"]):
            self.show_dot()
            return
        self.finish()

    def finish(self):
        svc = self.svc
        c = self.check
        kind = c["kind"]
        if kind in ("quick", "five"):
            if c["captured"]:
                log(f"{kind} check done ({c['captured']} of {len(c['dots'])} dots)")
                self.after_quick = [] if kind == "quick" else None
            self.close()
            return
        n = len(c["dots"])
        if c["captured"] < n * 2 / 3:
            reasons = c["reasons"]
            main = max(reasons, key=reasons.get) if reasons else None
            self.full_failed = main
            log(f"calibration failed: only {c['captured']} of {n} dots; the old one stays"
                + (f". Not taken: {', '.join(f'{r} ({k})' for r, k in reasons.items())}" if reasons else ""))
            self.to_panel(f"text Calibration failed: only {c['captured']} of {n} dots. Try again from Input Settings")
            if main:
                fit = main in c["fit_reasons"]
                self.note(f"Most dots: {main}" + (". Check headset fit on the Gaze page" if fit else ""))
            self.to_panel("dot 0 0 off")
            c["done_at"] = time.monotonic() + (8.0 if main else 3.0)  # time to read why
            c["closing"] = True
            return
        self.full_failed = None
        if c["own"]:
            reply = ask(EYES, "calib-fit", 10.0)
            log(f"calibration ({c['captured']} of {n} dots): our tracker says {reply or 'nothing'}")
        else:
            mode = svc.mode if svc.mode != "none" else DEFAULT_MODEL
            for name, pts in c["points"].items():
                if name in svc.models and pts:
                    svc.models[name].fit(pts, mode)
            d = {name: m.to_json() for name, m in svc.models.items()}
            d["_meta"] = {"calibrated_at": time.time(), "model": mode, "how": "panel"}
            d["_live"] = {}
            tmp = CALIBRATION.with_suffix(".tmp")
            tmp.write_text(json.dumps(d, indent=1))
            tmp.replace(CALIBRATION)
            svc.forget_lessons()  # a new calibration: the pointer's lessons start over on top of it
            svc.load_calibration()
            svc.refit()
            log(f"calibration ({c['captured']} of {n} dots): {mode}, saved")
        self.close()

    def close(self, why=None):
        if not self.check:
            return
        if why:
            log(f"{self.check['kind']} check closed: {why}")
        self.to_panel("hide")
        self.to_helper("calpanel 0")
        if self.check["kind"] == "full" and self.screens_shown:
            ask(SCREENS, "show", 0.5)
        self.screens_shown = None
        self.check = None

    def quit(self):
        c = self.check
        if not c:
            return
        self.close("quit")
        if c["kind"] == "full" and self.calibrated() is False:
            # No calibration still: gaze mode can't work, so it goes off until it's turned on again.
            try:
                write_gaze(False)
            except OSError as e:
                log(f"can't write {CONF}: {e}")
            self.to_helper("gaze off")
            self.gaze_on = False
            log("gaze mode off: no calibration")

    def log_check(self, rec):
        try:
            with open(CHECK_LOG, "a") as f:
                f.write(json.dumps(rec) + "\n")
        except OSError:
            pass

    # --- From the service ---

    def run_pending(self):
        """A check asked for while the service idled: once the tracker sends (and our tracker has
        said whether it's calibrated), or WAKE_SETTLE after waking, when its error is the real one."""
        svc = self.svc
        words, _ = self.pending
        ready = (time.monotonic() - svc.last_sample < 2 if words[0] == "fitcheck" else self.can_run()) \
            and (svc.kind != "own" or self.calibrated() is not None)
        if not ready and svc.waking():
            return
        self.pending = None
        reply = self.command(words, queue=False)
        if reply != "ok":
            log(f"{words[0]}, asked for while idle: {reply.removeprefix('error ')}")

    def command(self, words, queue=True):
        """quickcal, calibrate, calaccept, calquit -> a reply."""
        cmd = words[0]
        if cmd in ("quickcal", "calibrate", "fitcheck") and queue and self.svc.waking() and not self.check:
            # The tracker isn't running (or only just started): wake it, and do this once it sends.
            self.pending = (words, time.monotonic())
            self.svc.update_awake()
            return "ok waking the eye tracker first"
        if cmd == "quickcal":
            return self.start("full" if self.calibrated() is False else "quick", "asked for")
        if cmd == "calibrate":
            return self.start("full", "asked for")
        if cmd == "fitcheck":
            return self.start("fit", "asked for")
        if cmd == "calaccept":
            if self.check and self.check["kind"] == "fit":
                self.check["fit"].toggle_guide(time.monotonic())
            elif self.check:
                if not self.check["accept"]:
                    self.check["accept_at"] = time.monotonic()
                self.check["accept"] = True
            return "ok"
        if cmd == "calquit":
            self.quit()
            return "ok"
        if cmd == "recheck" and len(words) == 2:
            self.auto_quick(f"a click was corrected {words[1]} deg")
            return "ok"
        return "error unknown command"

    def after_lesson(self, rec):
        if self.after_quick is None or "refused" in rec:
            return
        self.after_quick.append(rec.get("lesson_deg", 0.0))
        if len(self.after_quick) < FIVE_COUNT:
            return
        off = self.after_quick
        self.after_quick = None
        if all(d > FIVE_LIMIT for d in off):
            log(f"the {FIVE_COUNT} lessons after the quick check were {', '.join(f'{d:.1f}' for d in off)} deg off")
            if self.gaze_on and self.can_run() and not self.check:
                self.start("five", "the quick check didn't fix it")

    def tick(self):
        c = self.check
        if not c:
            return
        now = time.monotonic()
        if c["kind"] == "fit":
            # Not closed when the eyes go: adjusting the headset loses them.
            if now - c["started"] > FIT_TIMEOUT:
                self.close("timed out")
            else:
                self.fit_tick(now)
            return
        if c["done_at"] and now >= c["done_at"]:
            if c.get("closing"):
                self.close()
            else:
                self.advance()
            return
        if not self.eyes_seen(EYES_GONE):
            self.close("the headset came off")
            return
        if c["done_at"]:
            return
        if c["accept"] and c["accept_at"] and now - c["accept_at"] > ACCEPT_WAIT:
            # Clicked, but no capture yet (see on_sample): say what it's waiting for.
            full = c["kind"] == "full"
            if now - c.get("gaze_at", 0.0) > 0.5:
                self.note("Waiting: the eye tracker isn't sending a gaze" if full else "Waiting: no gaze")
            else:
                self.note("Waiting for your gaze to hold still on the dot" if full else "Hold your look still")
        if c["kind"] == "quick" and now - c["started"] > QUICK_TIMEOUT:
            self.close("ignored")
        elif c["kind"] != "quick" and now - c["shown"] > CLICK_IDLE:
            self.close(f"no click on dot {c['i'] + 1} for {CLICK_IDLE:.0f} s")

    def periodic(self):
        svc = self.svc
        now = time.monotonic()
        if not self.panel_proc and now >= self.panel_restart_at:
            self.start_panel()
        self.to_helper("gaze ? headset")
        if now - self.gaze_heard > 5:
            self.gaze_on = self.headset = None  # the helper isn't answering
        if self.pending:
            self.run_pending()
        if self.check:
            self.to_helper("calpanel 1")
        if not self.eyes_seen(AWAY_MIN):
            self.away, self.back_since = True, None
        elif self.back_since is not None and not self.eyes_seen():
            self.back_since = None  # gone again before DON_DELAY
        elif self.back_since is not None and now - self.back_since >= DON_DELAY:
            self.away, self.back_since = False, None
            if not self.check:
                # A calibration that closed unfinished opens again. Not one still open: gaze mode
                # coming on wakes our tracker, so its eyes come back just as the calibration
                # opens, and re-arming then opened a second one when the first ended.
                self.full_armed = True
            self.auto_quick("the headset went on")
        if svc.kind == "own" and now - svc.own_at < 5:
            reseat = any(e.get("reseat") for e in (svc.own.get("eyes") or {}).values())
            if not reseat:
                self.reseat_seen = False
            elif not self.reseat_seen and self.can_run():
                self.reseat_seen = True
                self.auto_quick("our tracker asked for a click")
        self.need_full("gaze mode is on without a calibration")

    def status(self):
        c = self.check
        st = {"check": None, "gaze_mode": self.gaze_on, "headset_worn": self.headset, "pending": self.pending[0][0]
              if self.pending else None, "calibrated": self.calibrated(), "eyes": self.eyes_seen(),
              "problem": self.problem(),
              "panel": self.panel_proc is not None,
              "last_quick_s": round(time.monotonic() - self.last_quick) if self.last_quick else None}
        if c:
            st["check"] = {"kind": c["kind"], "dot": c["i"] + 1, "dots": len(c["dots"]), "captured": c["captured"],
                           "skipped": c["skipped"], "reason": c["reason"]}
        return st

    def stop(self):
        self.close("the gaze service is stopping")
        self.stop_panel()
