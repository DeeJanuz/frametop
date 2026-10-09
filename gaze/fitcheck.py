"""fitcheck: how well the eye tracker sees each eye, for fitting the headset (the headset
panel's fit check, gazecheck.py, and ft-gazeprobe's Headset fit mode).

From each ft-gaze sample it takes, per eye, whether the tracker has that eye (its variance
for the eye's direction, "unc", under EYE_LOST; see gazecal), how open the eye is, and the
tracker's own confidence in its latest measurement of it ("eye" "q": the measurement's
variance, about 2e-5 on a clear view). It keeps that per direction you look in (10 degree
cells, and a few named regions), so a map shows where each eye gets lost, and turns it into
hints.

On the Frame this was written for, the left eye was lost 57-63 % of the time looking 30-50
degrees down (at the keyboard) and the right never; at screen height both were seen over
98 % of the time. Looking down, the lids come down over the eyes, and a glance at the
keyboard isn't where the gaze pointer matters: ft-gazed ignores looks down past the
screens. So those are on the maps, but not in the cards' counts or the hints' warnings.

Directions are head-relative degrees (yaw +left, pitch +up), the combined gaze's.
"""

import math
import statistics
from collections import deque

from gazecal import EYE_FOUND, EYE_LOST

EYES = ("Left eye", "Right eye")
CELL = 10.0
YAW = (-40, 40)
PITCH = (-50, 30)
CLOSED = 0.12      # openness under this: closed (a blink, or squeezed shut)
MIN_REGION = 60    # samples in a region before it's judged (two thirds of a second)

# Named regions, for the hints: (key, words, test on yaw and pitch).
REGIONS = [
    ("down", "down (at a keyboard or desk)", lambda y, p: p < -20),
    ("up", "up", lambda y, p: p > 15),
    ("left", "to the left", lambda y, p: y > 20 and -20 <= p <= 15),
    ("right", "to the right", lambda y, p: y < -20 and -20 <= p <= 15),
    ("centre", "straight ahead (screen height)", lambda y, p: abs(y) <= 20 and -20 <= p <= 15),
]

# The guided check: dots on the screen (fractions of its size; the corners stay clear of the
# probe's title bar and toolbar), then prompts to look past it. Seconds each.
GUIDE = [
    ("dot", (0.5, 0.5), 2.0), ("dot", (0.12, 0.2), 2.0), ("dot", (0.88, 0.2), 2.0),
    ("dot", (0.88, 0.92), 2.0), ("dot", (0.12, 0.92), 2.0), ("dot", (0.5, 0.92), 2.0),
    ("look", "Look down at your keyboard", 4.0), ("look", "Look up, above the screen", 3.0),
    ("look", "Look far to the left", 3.0), ("look", "Look far to the right", 3.0),
    ("dot", (0.5, 0.5), 2.0),
]


class FitCheck:
    def __init__(self, ignore=None):
        # The eye SteamVR's tracker ignores (0 left, 1 right) with Track Dominant Eye Only on
        # (gazecal.tracked_eye), or None: its losses say nothing about the fit.
        self.ignore = ignore
        self.reset()

    def reset(self):
        self.lost = [False, False]
        self.lost_since = [None, None]
        self.losses = [0, 0]                      # times each eye was lost
        self.durations = [[], []]                 # how long each loss lasted (s)
        self.cells = [{}, {}]                     # per eye: (i, j) -> [samples, lost]
        self.regions = [{k: [0, 0] for k, _, _ in REGIONS} for _ in EYES]
        self.recent = [deque(maxlen=900), deque(maxlen=900)]  # (t, lost) for the last 10 s
        self.q = [deque(maxlen=180), deque(maxlen=180)]       # recent fresh measurement variances
        self.open = [0.0, 0.0]
        self.unc = [0.0, 0.0]
        self.gaze = None
        self.samples = 0
        self.guide = None                          # {"start": t, "step": i, "results": [...]}
        self.have_eye_data = False

    # --- Samples ---

    def feed(self, s, now):
        m1 = s["src"].get("mmap1") or {}
        unc, opens = m1.get("unc"), m1.get("open")
        if "hy" not in m1 or not unc or not opens:
            return
        self.have_eye_data = True
        self.samples += 1
        eye = s.get("eye") or {}
        hy, hp = m1["hy"], m1["hp"]
        self.gaze = (hy, hp)
        self.unc = list(unc)
        for k in (0, 1):
            self.open[k] += 0.2 * (opens[k] - self.open[k])
            was = self.lost[k]
            self.lost[k] = unc[k] > (EYE_FOUND if was else EYE_LOST)
            if self.lost[k] and not was:
                if not looking_down(hy, hp):
                    self.losses[k] += 1
                self.lost_since[k] = now
            elif was and not self.lost[k] and self.lost_since[k] is not None:
                if not looking_down(hy, hp):
                    self.durations[k].append(now - self.lost_since[k])
                self.lost_since[k] = None
            q = eye.get("q")
            if q and (eye.get("new") or [1, 1])[k]:
                self.q[k].append(q[k])
        closed = [opens[k] < CLOSED for k in (0, 1)]
        judged = [k for k in (0, 1) if k != self.ignore]
        if all(closed[k] for k in judged) or all(self.lost[k] for k in judged):
            return  # a blink: says nothing about the fit
        key = (math.floor(hy / CELL), math.floor(hp / CELL))
        for k in (0, 1):
            c = self.cells[k].setdefault(key, [0, 0])
            c[0] += 1
            c[1] += self.lost[k]
            for rk, _, test in REGIONS:
                if test(hy, hp):
                    r = self.regions[k][rk]
                    r[0] += 1
                    r[1] += self.lost[k]
            if not looking_down(hy, hp):
                self.recent[k].append((now, self.lost[k]))
        g = self.guide
        if g and g["step"] < len(GUIDE):
            res = g["results"][g["step"]]
            res[0] += 1
            res[1] += self.lost[0]
            res[2] += self.lost[1]

    # --- The guided check ---

    def toggle_guide(self, now):
        if self.guide and self.guide["step"] < len(GUIDE):
            self.guide = None
        else:
            self.guide = {"start": now, "step": 0, "step_start": now, "results": [[0, 0, 0] for _ in GUIDE]}

    def guide_step(self, now):
        """The current step (kind, what, seconds left), or None when there's no check running."""
        g = self.guide
        if not g or g["step"] >= len(GUIDE):
            return None
        kind, what, secs = GUIDE[g["step"]]
        if now - g["step_start"] >= secs:
            g["step"] += 1
            g["step_start"] = now
            return self.guide_step(now)
        return kind, what, secs - (now - g["step_start"])

    # --- Summaries ---

    def status(self, k):
        if k == self.ignore:
            return "not tracked", (0.6, 0.6, 0.6)
        if not self.samples:
            return "no data", (0.6, 0.6, 0.6)
        if self.lost[k]:
            return "LOST", (1.0, 0.35, 0.3)
        if self.open[k] < CLOSED:
            return "closed", (0.8, 0.8, 0.8)
        return "tracking", (0.35, 1.0, 0.5)

    def tracked_share(self, k, now, window=10.0):
        pts = [lost for t, lost in self.recent[k] if now - t <= window]
        return (1 - sum(pts) / len(pts)) if pts else None

    def signal(self, k):
        """The tracker's recent confidence in this eye, 0..1 (from its measurement variance:
        2e-5 or less is 1, 1e-3 or more is 0), or None."""
        if len(self.q[k]) < 10:
            return None
        q = statistics.median(self.q[k])
        return min(1.0, max(0.0, (math.log10(1e-3) - math.log10(max(q, 1e-9))) / (math.log10(1e-3) - math.log10(2e-5))))

    def region_share(self, k, key):
        n, lost = self.regions[k][key]
        return (lost / n) if n >= MIN_REGION else None

    def hints(self):
        if not self.have_eye_data:
            return ["No per-eye data from ft-gaze (it needs SteamVR's eye-server.mmap, and a current build)."]
        if self.samples < 3 * MIN_REGION:
            return ["Look around slowly (the screen's corners, then down at your keyboard, up, left and right) "
                    "or press Enter for a guided check."]
        out = []
        if self.ignore is not None:
            out.append(f"SteamVR tracks only your {EYES[1 - self.ignore].lower()} (Track Dominant Eye Only in "
                       f"SteamVR's settings), so your {EYES[self.ignore].lower()} doesn't count here.")
        bad = {}
        for k in (0, 1):
            if k == self.ignore:
                continue
            for key, words, _ in REGIONS:
                share = self.region_share(k, key)
                if share is not None and share >= 0.15:
                    bad.setdefault(key, {})[k] = share
        for key, words, _ in REGIONS:
            if key not in bad:
                continue
            eyes = bad[key]
            if len(eyes) == 2:
                if key == "down":
                    out.append("Both eyes get lost looking down at the keyboard. That's fine: the gaze service "
                               "ignores looks down past the screens.")
                elif key == "centre":
                    out.append(f"Both eyes get lost looking {words} ({eyes[0]:.0%} and {eyes[1]:.0%} of the time): "
                               "check the lenses are clean and the headset is on as usual; if it stays like this, "
                               "the tracker isn't getting a clear view of either eye.")
                else:
                    out.append(f"Both eyes get lost looking {words}: that's past what the tracker covers for your "
                               "face, not one eye's fit.")
                continue
            k = next(iter(eyes))
            other = self.region_share(1 - k, key) if 1 - k != self.ignore else None
            vs = f", the {EYES[1 - k].lower()} {other:.0%}" if other is not None else ""
            line = f"{EYES[k]}: lost {eyes[k]:.0%} of the time looking {words}{vs}."
            if key == "down":
                line += (" That's fine: glancing at the keyboard, the lids come down over the eyes, and the gaze "
                         "service ignores looks down past the screens, so the pointer stays put.")
            elif key == "centre":
                line += (" Even at screen height: clean that lens, and check its distance from your eye and the "
                         "IPD. Lashes that touch the lens get in the camera's way too.")
            else:
                line += (" At the edge of your view: try the IPD setting, and centring the headset between your "
                         "eyes.")
            out.append(line)
        s0, s1 = self.signal(0), self.signal(1)
        if self.ignore is None and s0 is not None and s1 is not None and abs(s0 - s1) > 0.25:
            k = 0 if s0 < s1 else 1
            out.append(f"The tracker is less sure of your {EYES[k].lower()} even when it has it "
                       f"(signal {min(s0, s1):.0%} against {max(s0, s1):.0%}).")
        if self.ignore is not None and len(out) == 1:
            out.append(f"Your {EYES[1 - self.ignore].lower()} is tracked everywhere you've looked so far.")
        if not out:
            out.append("Both eyes are tracked everywhere you've looked so far.")
        return out

    # --- Drawing (cairo) ---

    def draw(self, cr, w, h, text, now):
        # Right of the probe's collapsed title bar, under its toolbar (top right).
        left = 300
        top = 190
        text(cr, left, top - 60, "Headset fit", (1, 1, 1), 30)
        text(cr, left, top - 28, "Adjust the headset and watch each eye. Enter: guided check. R: start over.",
             (0.8, 0.8, 0.8), 18)
        card_w = min(560, (w - left - 80) / 2)
        mh = max(0, min(card_w * 0.8, h - top - 280 - 200))
        for k in (0, 1):
            x = left + k * (card_w + 40)
            self.draw_card(cr, x, top, card_w, text, now, k)
            self.draw_map(cr, x, top + 280, card_w, mh, text, k)
        y = top + 280 + (mh + 60 if mh >= 80 else 0)
        for line in self.hints()[:4]:
            for part in wrap(line, max(40, int((w - left - 40) / 10))):
                if y > h - 30:
                    break
                text(cr, left, y, part, (1, 0.95, 0.75), 18)
                y += 26
            y += 8
        step = self.guide_step(now)
        g = self.guide
        if step:
            kind, what, left_s = step
            if kind == "dot":
                fx, fy = what
                x, y = fx * w, fy * h
                cr.set_source_rgba(1, 0.85, 0.2, 0.95)
                cr.arc(x, y, 14 + 4 * math.sin(now * 6), 0, 2 * math.pi)
                cr.fill()
            else:
                text(cr, w / 2 - 260, h / 2, f"{what} ({left_s:.0f})", (1, 0.85, 0.2), 34)
        elif g and g["step"] >= len(GUIDE):
            self.draw_guide_results(cr, w, h, text)

    def draw_card(self, cr, x, y, cw, text, now, k):
        cr.set_source_rgba(1, 1, 1, 0.06)
        cr.rectangle(x, y, cw, 230)
        cr.fill()
        word, col = self.status(k)
        text(cr, x + 16, y + 38, EYES[k], (1, 1, 1), 26)
        cr.select_font_face("sans")
        cr.set_font_size(26)
        text(cr, x + cw - 16 - cr.text_extents(word).x_advance, y + 38, word, col, 26)
        rows = [("Open", self.open[k]), ("Signal", self.signal(k)), ("Seen, last 10 s", self.tracked_share(k, now))]
        yy = y + 70
        for label, v in rows:
            text(cr, x + 16, yy + 16, label, (0.85, 0.85, 0.85), 17)
            bx, bw = x + 170, cw - 250
            cr.set_source_rgba(1, 1, 1, 0.12)
            cr.rectangle(bx, yy, bw, 20)
            cr.fill()
            if v is not None:
                v = min(1.0, max(0.0, v))
                cr.set_source_rgba(*bar_colour(v), 0.9)
                cr.rectangle(bx, yy, bw * v, 20)
                cr.fill()
                text(cr, bx + bw + 10, yy + 16, f"{v:.0%}", (0.9, 0.9, 0.9), 17)
            yy += 36
        d = self.durations[k]
        longest = max(d) if d else 0
        n = self.losses[k]
        text(cr, x + 16, yy + 22, f"Lost {n} time{'' if n == 1 else 's'}" + (f", longest {longest:.1f} s" if longest >= 0.05 else ""),
             (0.85, 0.85, 0.85), 17)

    def draw_map(self, cr, x, y, mw, mh, text, k):
        """Where you looked (yaw across, pitch up), each cell coloured by how often this eye
        was lost there: green never, red always, dark: not looked there yet."""
        if mh < 80:
            return
        cols = int((YAW[1] - YAW[0]) / CELL)
        rows = int((PITCH[1] - PITCH[0]) / CELL)
        cw, ch = mw / cols, mh / rows
        text(cr, x, y - 8, f"Where the {EYES[k].lower()} gets lost", (0.85, 0.85, 0.85), 17)
        for i in range(cols):
            yaw_i = math.floor(YAW[1] / CELL) - 1 - i       # left of the map is your left (+yaw)
            for j in range(rows):
                pitch_j = math.floor(PITCH[1] / CELL) - 1 - j
                c = self.cells[k].get((yaw_i, pitch_j))
                cx, cy = x + i * cw, y + j * ch
                if c and c[0] >= 10:
                    share = c[1] / c[0]
                    cr.set_source_rgba(*bar_colour(1 - share), 0.75)
                else:
                    cr.set_source_rgba(1, 1, 1, 0.05)
                cr.rectangle(cx + 1, cy + 1, cw - 2, ch - 2)
                cr.fill()
        # Straight ahead, and the gaze now.
        def at(yaw, pitch):
            return x + (YAW[1] - yaw) / (YAW[1] - YAW[0]) * mw, y + (PITCH[1] - pitch) / (PITCH[1] - PITCH[0]) * mh
        cr.set_source_rgba(1, 1, 1, 0.35)
        cr.set_line_width(1)
        ox, oy = at(0, 0)
        cr.move_to(ox - 10, oy)
        cr.line_to(ox + 10, oy)
        cr.move_to(ox, oy - 10)
        cr.line_to(ox, oy + 10)
        cr.stroke()
        text(cr, x, y + mh + 20, "+ ahead, bottom rows: keyboard", (0.6, 0.6, 0.6), 14)
        if self.gaze:
            gx, gy = at(max(YAW[0], min(YAW[1], self.gaze[0])), max(PITCH[0], min(PITCH[1], self.gaze[1])))
            cr.set_source_rgba(1, 1, 1, 0.95)
            cr.arc(gx, gy, 5, 0, 2 * math.pi)
            cr.fill()

    def draw_guide_results(self, cr, w, h, text):
        res = self.guide["results"]
        lines = []
        for (kind, what, _), (n, l0, l1) in zip(GUIDE, res):
            if not n:
                continue
            name = what if kind == "look" else "dot at {:.0%}, {:.0%}".format(*what)
            lines.append(f"{name}: left lost {l0 / n:.0%}, right {l1 / n:.0%}")
        y = h / 2 - 20 * len(lines)
        text(cr, w / 2 - 300, y - 40, "Guided check", (1, 0.85, 0.2), 26)
        for line in lines:
            text(cr, w / 2 - 300, y, line, (1, 1, 1), 19)
            y += 30


def looking_down(yaw, pitch):
    """A look down at the keyboard: the "down" region, which ft-gazed doesn't send on."""
    return pitch < -20


def bar_colour(v):
    """Red (0) through amber to green (1)."""
    if v < 0.5:
        return 1.0, 0.3 + 0.9 * v, 0.3
    return 1.0 - 1.3 * (v - 0.5), 0.75 + 0.25 * (v - 0.5) * 2, 0.35


def wrap(s, width):
    words, lines, cur = s.split(), [], ""
    for wd in words:
        if cur and len(cur) + 1 + len(wd) > width:
            lines.append(cur)
            cur = wd
        else:
            cur = f"{cur} {wd}".strip()
    if cur:
        lines.append(cur)
    return lines
