"""gazecal: gaze calibration shared by ft-gazeprobe and ft-gazed.

The correction models (Correction: the calibration fitted from calibration dots;
LiveCorrection: what clicks teach on the fly, on top of it), the smoothing filters, the
blink and dropout filter for one look at a spot, EyeFallback (the gaze from one eye while
the tracker has lost the other), EyeWeights (how much each eye counts), and SteamEyeLog, which follows SteamVR's eye tracking log. Angles are head-relative degrees (yaw +left, pitch +up), as ft-gaze
reports them.
"""

import json
import math
import os
import statistics
import time
from pathlib import Path

STATE = Path.home() / ".local" / "state" / "frametop" / "gaze"

# --- Small math ---------------------------------------------------------------------

def px_from_deg(j, dy, dp):
    """Pixels for a head-relative change of (yaw, pitch) degrees, from ft-gaze's Jacobian."""
    return j[0] * dy + j[2] * dp, j[1] * dy + j[3] * dp


def deg_from_px(j, dx, dy):
    """Head-relative (yaw, pitch) degrees for a pixel offset: the Jacobian's inverse."""
    det = j[0] * j[3] - j[2] * j[1]
    if abs(det) < 1e-9:
        return 0.0, 0.0
    return (j[3] * dx - j[2] * dy) / det, (-j[1] * dx + j[0] * dy) / det


class OneEuro:
    """One Euro filter (Casiez et al. 2012): smooth when still, quick when moving.
    `scale` turns the input's units into degrees, so beta is per degree a second."""

    def __init__(self, min_cutoff=1.0, beta=0.01, d_cutoff=1.0):
        self.min_cutoff, self.beta, self.d_cutoff = min_cutoff, beta, d_cutoff
        self.x = self.dx = self.t = None

    @staticmethod
    def alpha(cutoff, dt):
        tau = 1.0 / (2 * math.pi * cutoff)
        return 1.0 / (1.0 + tau / dt)

    def __call__(self, x, t, scale=1.0):
        if self.t is None or t <= self.t or t - self.t > 0.5:
            self.x, self.dx, self.t = x, 0.0, t
            return x
        dt = t - self.t
        dx = (x - self.x) / dt
        a_d = self.alpha(self.d_cutoff, dt)
        self.dx = a_d * dx + (1 - a_d) * self.dx
        cutoff = self.min_cutoff + self.beta * abs(self.dx) * scale
        a = self.alpha(cutoff, dt)
        self.x = a * x + (1 - a) * self.x
        self.t = t
        return self.x


class Fixation:
    """Dispersion-based fixations: while gaze stays within `radius` degrees of the current
    fixation's mean, the output is that mean, so the dot sits still; two samples in a row
    outside it start a new fixation there, so a glance elsewhere moves the dot at once."""

    def __init__(self, radius=1.0):
        self.radius = radius
        self.reset()

    def reset(self):
        self.sum = [0.0, 0.0]
        self.count = 0
        self.outside = []
        self.last_t = None

    def __call__(self, x, y, t, dpp):
        if self.last_t is not None and (t <= self.last_t or t - self.last_t > 0.5):
            self.reset()
        self.last_t = t
        if self.count:
            mx, my = self.sum[0] / self.count, self.sum[1] / self.count
            if math.hypot(x - mx, y - my) * dpp > self.radius:
                self.outside.append((x, y))
                if len(self.outside) < 2:
                    return mx, my  # one stray sample: probably noise
                self.sum = [sum(p[0] for p in self.outside), sum(p[1] for p in self.outside)]
                self.count = len(self.outside)
                self.outside = []
                return self.sum[0] / self.count, self.sum[1] / self.count
        self.outside = []
        if self.count >= 90:  # the last second or so: a slow drift still gets followed
            self.sum = [self.sum[0] * 89 / 90, self.sum[1] * 89 / 90]
            self.count = 89
        self.sum[0] += x
        self.sum[1] += y
        self.count += 1
        return self.sum[0] / self.count, self.sum[1] / self.count


MODELS = ["none", "offset", "affine", "affine+grid", "quadratic", "quadratic+grid"]
DEFAULT_MODEL = "quadratic"


class Correction:
    """Gaze correction in degrees, looked up by where in your view you're looking
    (head-relative yaw and pitch, hy and hp):

      offset          one (yaw, pitch) offset everywhere
      affine          plus a straight-line change across the view: a gain and a tilt
      quadratic       plus curvature (hy*hp, hy^2, hp^2): the second-order polynomial video
                      eye trackers usually calibrate with. The tracker's error grows as
                      the eye turns away from the centre (on the Frame it overstates
                      vertical movement, more the further up or down you look, and looking
                      up adds a sideways error), and a straight line can only follow part
                      of that
      ...+grid        plus a bilinear grid of what's left every 10 degrees

    Coefficients: C @ f, f = [1, x, y, x*y, x^2, y^2] with x = hy/30, y = hp/30; the
    terms a model doesn't use are 0. A polynomial runs away outside the spots it was fitted
    on, so its input is clamped to the range of view it has seen, plus a margin.
    """

    YAWS = list(range(-40, 41, 10))
    PITCHES = list(range(-30, 31, 10))
    NF = 6
    MARGIN = 3.0  # degrees past the fitted range that the polynomial still follows

    def __init__(self):
        self.reset()

    def reset(self):
        self.a = [[0.0] * self.NF, [0.0] * self.NF]
        self.grid = [[[0.0, 0.0] for _ in self.PITCHES] for _ in self.YAWS]
        self.samples = 0
        self.range = None  # [hy min, hy max, hp min, hp max] of the samples so far

    @staticmethod
    def base(mode):
        return mode.split("+")[0]

    def clamp(self, hy, hp):
        if not self.range:
            return hy, hp
        y0, y1, p0, p1 = self.range
        m = self.MARGIN
        return min(max(hy, y0 - m), y1 + m), min(max(hp, p0 - m), p1 + m)

    def features(self, hy, hp, mode):
        kind = self.base(mode)
        if kind == "offset":
            return [1.0, 0.0, 0.0, 0.0, 0.0, 0.0]
        hy, hp = self.clamp(hy, hp)
        x, y = hy / 30.0, hp / 30.0
        if kind == "affine":
            return [1.0, x, y, 0.0, 0.0, 0.0]
        return [1.0, x, y, x * y, x * x, y * y]

    def extend(self, hy, hp):
        if self.range is None:
            self.range = [hy, hy, hp, hp]
        else:
            r = self.range
            self.range = [min(r[0], hy), max(r[1], hy), min(r[2], hp), max(r[3], hp)]

    def weights(self, hy, hp):
        def cell(v, axis):
            v = min(max(v, axis[0]), axis[-1])
            i = min(int((v - axis[0]) // 10), len(axis) - 2)
            return i, (v - axis[i]) / 10.0
        i, fy = cell(hy, self.YAWS)
        k, fp = cell(hp, self.PITCHES)
        return [((i, k), (1 - fy) * (1 - fp)), ((i + 1, k), fy * (1 - fp)),
                ((i, k + 1), (1 - fy) * fp), ((i + 1, k + 1), fy * fp)]

    def get(self, hy, hp, mode):
        if mode == "none":
            return 0.0, 0.0
        f = self.features(hy, hp, mode)
        cy = sum(a * b for a, b in zip(self.a[0], f))
        cp = sum(a * b for a, b in zip(self.a[1], f))
        if mode.endswith("+grid"):
            for (i, k), w in self.weights(hy, hp):
                cy += w * self.grid[i][k][0]
                cp += w * self.grid[i][k][1]
        return cy, cp

    def learn(self, hy, hp, dy, dp, mode, rate):
        """One sample: the correction here should have been (dy, dp) degrees more.
        Normalized LMS for the polynomial; the grid takes half when it's on."""
        if mode == "none":
            return
        self.samples += 1
        self.extend(hy, hp)
        grid = mode.endswith("+grid")
        share = rate * 0.5 if grid else rate
        f = self.features(hy, hp, mode)
        norm = sum(v * v for v in f)
        for row, d in ((self.a[0], dy), (self.a[1], dp)):
            for n in range(self.NF):
                row[n] += share * d * f[n] / norm
        if grid:
            rest = rate - share
            for (i, k), w in self.weights(hy, hp):
                self.grid[i][k][0] += rest * w * dy
                self.grid[i][k][1] += rest * w * dp

    def fit(self, points, mode, ridge=0.05, smooth=0.3):
        """Batch fit from (hy, hp, dy, dp) points, each the whole error there (degrees)."""
        self.reset()
        if mode == "none" or not points:
            return
        self.samples = len(points)
        for p in points:
            self.extend(p[0], p[1])
        kind = self.base(mode)
        used = {"offset": 1, "affine": 3, "quadratic": 6}[kind]
        if used > 1 and len(points) < used + 2:  # too few spots for this many terms
            kind, used = ("affine", 3) if len(points) >= 5 else ("offset", 1)
        if kind == "offset":
            self.a[0][0] = statistics.fmean(p[2] for p in points)
            self.a[1][0] = statistics.fmean(p[3] for p in points)
        else:
            # Least squares, with a little ridge on everything but the offset, so a
            # lopsided set of spots can't bend it far.
            X = [self.features(p[0], p[1], kind)[:used] for p in points]
            M = [[sum(x[r] * x[c] for x in X) + (ridge * len(X) if r == c and r else 0.0) for c in range(used)]
                 for r in range(used)]
            for out, col in ((self.a[0], 2), (self.a[1], 3)):
                b = [sum(x[r] * p[col] for x, p in zip(X, points)) for r in range(used)]
                out[:used] = solve(M, b)
        if not mode.endswith("+grid"):
            return
        # Each node: the weighted mean of what the polynomial left over near it, shrunk toward 0.
        acc = [[[0.0, 0.0, 0.0] for _ in self.PITCHES] for _ in self.YAWS]
        for hy, hp, dy, dp in points:
            ly, lp = self.get(hy, hp, kind)
            for (i, k), w in self.weights(hy, hp):
                acc[i][k][0] += w * (dy - ly)
                acc[i][k][1] += w * (dp - lp)
                acc[i][k][2] += w
        for i in range(len(self.YAWS)):
            for k in range(len(self.PITCHES)):
                sy, sp, sw = acc[i][k]
                self.grid[i][k] = [sy / (sw + smooth), sp / (sw + smooth)]

    def offset(self):
        return self.a[0][0], self.a[1][0]

    def to_json(self):
        return {"coef": self.a, "range": self.range, "grid": self.grid, "samples": self.samples}

    def from_json(self, d):
        self.reset()
        a = d.get("coef") or d.get("affine")  # "affine": the 3-term version of this file
        if a and len(a) == 2 and all(len(r) in (3, self.NF) for r in a):
            self.a = [list(map(float, r)) + [0.0] * (self.NF - len(r)) for r in a]
        grid = d.get("grid")
        if grid and len(grid) == len(self.YAWS) and all(len(r) == len(self.PITCHES) for r in grid):
            self.grid = [[list(c) for c in row] for row in grid]
        r = d.get("range")
        self.range = list(map(float, r)) if r and len(r) == 4 else None
        self.samples = d.get("samples", 0)


def solve(M, b):
    """Solve a small linear system (Gaussian elimination with pivoting); zeros if singular."""
    n = len(b)
    A = [row[:] + [b[i]] for i, row in enumerate(M)]
    for c in range(n):
        piv = max(range(c, n), key=lambda r: abs(A[r][c]))
        if abs(A[piv][c]) < 1e-12:
            return [0.0] * n
        A[c], A[piv] = A[piv], A[c]
        for r in range(n):
            if r != c:
                f = A[r][c] / A[c][c]
                for k in range(c, n + 1):
                    A[r][k] -= f * A[c][k]
    return [A[i][n] / A[i][i] for i in range(n)]


class LiveCorrection:
    """Corrections learned on the fly from snapped clicks, on top of the calibration.

    Each click on an element is a measurement: you were looking at that element when you
    pressed, and the tracker put your gaze at the raw point, so the gap between them is the
    whole error there. Whatever the calibration doesn't already explain (the residual) is
    fitted with the same quadratic terms. Every term but the offset is held close to zero
    (ridge), so one click shifts the whole correction and more clicks bend it. Whatever is
    still left near a click is added within a few degrees of it: on the Frame, errors less
    than 3 degrees apart are alike, and ones further apart are unrelated. Recent clicks count more (a
    half-life counted in clicks), so it follows SteamVR's gaze as that drifts or relearns.
    Replayed on logged points: a calibration from an earlier session was 4.95 degrees off;
    one click brought that to 2.3, five to 1.6, twenty to 1.2.

    A big element says little about where on it you looked, so each axis is weighted by the
    element's size along it: a list row 12 degrees wide barely counts sideways.

    Putting the headset back on moves the error (SteamVR starts its eye model over each
    time, and the headset sits a little differently), so clicks from before the last time
    it went on (`wear_time`, when set) count OLD_WEAR as much: the offset is relearned from
    the first few clicks after, and the shape is kept meanwhile."""

    RIDGE = [0.01, 0.5, 0.5, 0.5, 0.5, 0.5]
    KERNEL = 2.5     # degrees: how far a click's leftover reaches
    SHRINK = 0.5     # near one click, half its leftover; near several, nearly all
    HALF_LIFE = 40   # clicks
    KEEP = 150
    MARGIN = 3.0
    OLD_WEAR = 0.3

    def __init__(self):
        self.samples = []  # dicts: time, hy, hp, dy, dp (the whole error), wy, wp
        self.wear_time = None
        self.reset_fit()

    def reset_fit(self):
        self.cy = [0.0] * 6
        self.cp = [0.0] * 6
        self.left = []     # (hy, hp, leftover yaw, leftover pitch, wy, wp, decay)
        self.range = None

    def features(self, hy, hp):
        if self.range:
            y0, y1, p0, p1 = self.range
            hy = min(max(hy, y0 - self.MARGIN), y1 + self.MARGIN)
            hp = min(max(hp, p0 - self.MARGIN), p1 + self.MARGIN)
        x, y = hy / 30.0, hp / 30.0
        return [1.0, x, y, x * y, x * x, y * y]

    def add(self, sample, base, mode):
        self.samples = (self.samples + [sample])[-self.KEEP:]
        self.refit(base, mode)

    def undo(self, base, mode):
        if self.samples:
            self.samples.pop()
            self.refit(base, mode)

    def refit(self, base, mode):
        """Refit from the samples against the calibration as it is now."""
        self.reset_fit()
        n = len(self.samples)
        if not n:
            return
        self.range = [min(s["hy"] for s in self.samples), max(s["hy"] for s in self.samples),
                      min(s["hp"] for s in self.samples), max(s["hp"] for s in self.samples)]
        rows = []
        for k, s in enumerate(self.samples):
            decay = 0.5 ** ((n - 1 - k) / self.HALF_LIFE)
            if self.wear_time and s.get("time", 0) < self.wear_time:
                decay *= self.OLD_WEAR
            by, bp = base.get(s["hy"], s["hp"], mode)
            rows.append((s, self.features(s["hy"], s["hp"]), s["dy"] - by, s["dp"] - bp, decay))
        for out, ri, wi in ((self.cy, 2, "wy"), (self.cp, 3, "wp")):
            M = [[self.RIDGE[r] if r == c else 0.0 for c in range(6)] for r in range(6)]
            b = [0.0] * 6
            for row in rows:
                w = row[4] * row[0][wi]
                f = row[1]
                for r in range(6):
                    b[r] += w * f[r] * row[ri]
                    for c in range(6):
                        M[r][c] += w * f[r] * f[c]
            out[:] = solve(M, b)
        for s, f, ry, rp, decay in rows:
            ly = ry - sum(a * v for a, v in zip(self.cy, f))
            lp = rp - sum(a * v for a, v in zip(self.cp, f))
            self.left.append((s["hy"], s["hp"], ly, lp, s["wy"] * decay, s["wp"] * decay))

    def get(self, hy, hp):
        if not self.samples:
            return 0.0, 0.0
        f = self.features(hy, hp)
        cy = sum(a * v for a, v in zip(self.cy, f))
        cp = sum(a * v for a, v in zip(self.cp, f))
        k2 = 2 * self.KERNEL ** 2
        sy = sp = wy = wp = 0.0
        for y, p, ly, lp, ay, ap in self.left:
            d2 = (y - hy) ** 2 + (p - hp) ** 2
            if d2 > 9 * k2:
                continue
            g = math.exp(-d2 / k2)
            sy += g * ay * ly
            wy += g * ay
            sp += g * ap * lp
            wp += g * ap
        return cy + sy / (wy + self.SHRINK), cp + sp / (wp + self.SHRINK)

    def offset(self):
        return self.cy[0], self.cp[0]


# The tracker's variance for an eye's direction (ft-gaze's "unc"): 0.0005-0.002 while it
# sees the eye, 0.015-0.03 once it's lost it, falling back through 0.008-0.002 in the 0.1 s
# after it finds it again.
EYE_LOST = 0.004
EYE_FOUND = 0.0025

# SteamOS 0.4's "Track Dominant Eye Only" (SteamVR's settings, General, advanced): SteamVR's
# tracker ignores the other eye, for someone whose eyes don't look at the same spot. Its
# settings: steamvr.eyeTrackingDominantEyeOnly, and steamvr.dominantEye (0 left, 1 right,
# SteamVR's default). Frametop then goes by that eye alone: a calibration that waits for both
# eyes would never take a dot, and the other eye's reading isn't where the person looks.
STEAMVR_SETTINGS = (Path.home() / ".config" / "openvr" / "config" / "steamvr.vrsettings",
                    Path.home() / ".steam" / "steam" / "config" / "steamvr.vrsettings")


def tracked_eye(paths=STEAMVR_SETTINGS):
    """The one eye SteamVR's tracker follows (0 left, 1 right), or None for both. The first
    of the settings files that exists counts (SteamVR keeps only settings changed from its
    defaults, so a missing key is the default)."""
    for path in paths:
        try:
            text = Path(path).read_text()
        except OSError:
            continue
        try:
            steamvr = json.loads(text).get("steamvr")
        except (ValueError, AttributeError):
            return None
        if not isinstance(steamvr, dict) or steamvr.get("eyeTrackingDominantEyeOnly") is not True:
            return None
        return 0 if steamvr.get("dominantEye", 1) == 0 else 1
    return None


class TrackedEye:
    """tracked_eye(), read again when SteamVR's settings file changes (looked at no more than
    every CHECK seconds), since the setting can change while a service runs."""

    CHECK = 2.0

    def __init__(self, paths=STEAMVR_SETTINGS):
        self.paths = paths
        self.eye = None
        self.stamp = None
        self.checked = None

    def __call__(self, now=None):
        now = time.monotonic() if now is None else now
        if self.checked is not None and now - self.checked < self.CHECK:
            return self.eye
        self.checked = now
        stamp = []
        for path in self.paths:
            try:
                st = os.stat(path)
                stamp.append((st.st_mtime_ns, st.st_size))
            except OSError:
                stamp.append(None)
        if stamp != self.stamp:
            self.stamp = stamp
            self.eye = tracked_eye(self.paths)
        return self.eye


class EyeFallback:
    """The gaze from one eye, while the tracker has lost the other.

    SteamVR's combined gaze (mmap set 1) keeps going with one eye lost, but badly: it holds
    the lost eye's yaw where it was and gives it the other eye's pitch, so the gaze moves
    half as far sideways as the eyes do (seen: the right eye swung 5 degrees, the combined
    gaze 2.5). Set 2's eyes are each eye's own reading. While both are seen, this learns what
    each eye reads against the combined gaze (an offset: half the angle between the eyes,
    plus how differently the tracker reads each), in 10 degree cells of where that eye
    looks, blended over the four nearest; while one is lost, the other eye plus its offset
    stands in for the combined gaze. So the rest (fixation lock, calibration, lessons)
    carries on as if nothing happened.

    On a recording, one eye alone came out 1.1 degrees (median) from both eyes' gaze, 0.8
    over a tenth of a second of a steady look, and a little more jittery (0.31-0.37 degrees
    against 0.28). Carrying on the offset from just before a loss did no better: what's
    left is fast noise, not something particular to that look.

    `update` and `get` take head-relative degrees (yaw, pitch)."""

    CELL = 10.0
    GLOBAL_RATE = 0.01   # per sample: about a second at 90 Hz
    CELL_RATE = 0.02     # the least a cell learns per sample, once it has CELL_FULL
    CELL_FULL = 30       # samples before a cell counts fully
    READY = 45           # samples of both eyes before an eye can stand in

    def __init__(self):
        self.glob = [None, None]      # per eye: [oy, op]
        self.seen = [0, 0]
        self.cells = [{}, {}]         # per eye: (i, j) -> [oy, op, n]

    def ready(self, eye):
        return self.seen[eye] >= self.READY

    def update(self, eye, ey, ep, cy, cp):
        oy, op = cy - ey, cp - ep
        g = self.glob[eye]
        if g is None:
            self.glob[eye] = [oy, op]
        else:
            g[0] += self.GLOBAL_RATE * (oy - g[0])
            g[1] += self.GLOBAL_RATE * (op - g[1])
        self.seen[eye] += 1
        key = (math.floor(ey / self.CELL), math.floor(ep / self.CELL))
        c = self.cells[eye].setdefault(key, [oy, op, 0])
        c[2] += 1
        a = max(1.0 / c[2], self.CELL_RATE)
        c[0] += a * (oy - c[0])
        c[1] += a * (op - c[1])

    def offset(self, eye, ey, ep):
        g = self.glob[eye]
        if g is None:
            return None
        # Bilinear over the four cells whose centres surround the point.
        fy, fp = ey / self.CELL - 0.5, ep / self.CELL - 0.5
        i0, j0 = math.floor(fy), math.floor(fp)
        ty, tp = fy - i0, fp - j0
        sy = sp = used = 0.0
        for di, wi in ((0, 1 - ty), (1, ty)):
            for dj, wj in ((0, 1 - tp), (1, tp)):
                c = self.cells[eye].get((i0 + di, j0 + dj))
                if c:
                    w = wi * wj * min(1.0, c[2] / self.CELL_FULL)
                    sy += w * c[0]
                    sp += w * c[1]
                    used += w
        return sy + (1 - used) * g[0], sp + (1 - used) * g[1]

    def get(self, eye, ey, ep):
        """The combined gaze from this eye's reading, or None before it has learned enough."""
        if not self.ready(eye):
            return None
        oy, op = self.offset(eye, ey, ep)
        return ey + oy, ep + op


class EyeWeights:
    """How much each eye (0 left, 1 right) counts in the gaze, for ft-gazed's eye bias.

    Two eyes beat either one: their errors partly cancel. On 306 live clicks with our own
    tracker (gaze/tracker, 2026-09-29) the eyes' sideways errors were correlated -0.37, and
    the mean of both was 0.65 degrees off (median), the left eye alone 0.96, the right 1.11.
    So a bias leans instead of choosing: "left" or "right" counts that eye LEAN times the
    other (on those clicks, 2:1 toward the better eye cost about 0.03 degrees, toward the
    worse one about 0.13). "auto" weights each
    by the inverse square of its RMS miss at the last KEEP lessons, once both have MIN, and
    alike until then. Each miss is measured before its lesson teaches anything, so each is a
    fresh test. On SteamVR's own test (2026-09-29) its calibration dots said the left eye was
    the better one and new spots said the right, so the misses come from lessons, not the fit.
    An eye that isn't seen (None) drops out, and the other carries the gaze alone."""

    LEAN = 2.0
    KEEP = 20
    MIN = 5
    FLOOR = 0.3   # degrees: so one lucky run can't give an eye all the weight
    STALE = 8.0   # degrees: a miss this big is the headset moved, not the eye's accuracy

    def __init__(self, bias="auto", misses=None):
        self.bias = bias
        self.misses = [list(m) for m in (misses or ([], []))]

    def add(self, miss):
        """One lesson's miss per eye (degrees, None where it wasn't seen)."""
        if any(m is not None and m > self.STALE for m in miss):
            return
        for k, m in enumerate(miss):
            if m is not None:
                self.misses[k] = (self.misses[k] + [m])[-self.KEEP:]

    def rms(self):
        return [math.sqrt(sum(m * m for m in ms) / len(ms)) if ms else None for ms in self.misses]

    def weights(self):
        """(left, right), summing to 1."""
        if self.bias in ("left", "right"):
            w = [self.LEAN, 1.0] if self.bias == "left" else [1.0, self.LEAN]
        elif all(len(ms) >= self.MIN for ms in self.misses):
            w = [1.0 / max(r, self.FLOOR) ** 2 for r in self.rms()]
        else:
            w = [1.0, 1.0]
        return w[0] / sum(w), w[1] / sum(w)

    def combine(self, eyes):
        """The weighted gaze from [(yaw, pitch) or None, (yaw, pitch) or None], or None."""
        w = [wk for wk, e in zip(self.weights(), eyes) if e is not None]
        seen = [e for e in eyes if e is not None]
        if not seen:
            return None
        total = sum(w)
        return (sum(wk * e[0] for wk, e in zip(w, seen)) / total, sum(wk * e[1] for wk, e in zip(w, seen)) / total)


class SteamEyeLog:
    """Follows SteamVR's eye tracking log (read only) for what moves the raw gaze under a
    calibration.

    SteamVR's eye tracker calibrates itself from clicks: a quick mouse-button down and up
    (the laser or the Frametop pointer), with the gaze within 5 degrees of the click and
    held still, is taken as "you were looking there" ("Accept usercal"). Accepted clicks
    were all under 0.14 s; 0.38 s was "too slow", and one that moved was refused. It keeps that in
    the running `eyetracking` process and saves nothing, so when the process starts again
    (SteamVR restarting), its calibration starts over. Both events are counted here, and
    each time the headset goes on ("HMD on"): the eye model starts over then too."""

    PATH = Path.home() / ".local" / "share" / "Steam" / "logs" / "eyetracking.txt"
    # It writes "HMD on" again every minute or so while on, and flickers off for 0.01-0.3 s:
    # only an on after an off of BLIP or longer counts.
    BLIP = 1.5

    def __init__(self):
        self.pos = 0
        self.inode = None
        self.partial = ""
        self.starts = []    # when the eyetracking process started
        self.accepts = []   # when it learned from a click
        self.rejects = []
        self.wears = []     # when the headset went on
        self.offs = []      # ... and off

    @staticmethod
    def stamp(line):
        head = line.split(" [", 1)[0]
        main, _, frac = head.partition(".")
        try:
            return time.mktime(time.strptime(main.strip(), "%a %b %d %Y %H:%M:%S")) + float("0." + (frac or "0"))
        except ValueError:
            return None

    def poll(self):
        """Read what's new. True if the eye tracker started again since the last poll."""
        try:
            st = os.stat(self.PATH)
        except OSError:
            return False
        if st.st_ino != self.inode or st.st_size < self.pos:
            self.inode, self.pos, self.partial = st.st_ino, 0, ""
        if st.st_size == self.pos:
            return False
        first = self.pos == 0 and not self.starts
        try:
            with open(self.PATH, "rb") as f:
                f.seek(self.pos)
                data = f.read()
        except OSError:
            return False
        self.pos += len(data)
        lines = (self.partial + data.decode("utf-8", "replace")).split("\n")
        self.partial = lines.pop()
        restarted = False
        for line in lines:
            if "usercal" not in line and "startup with PID" not in line and "HMD o" not in line:
                continue
            t = self.stamp(line)
            if t is None:
                continue
            if "startup with PID" in line:
                self.starts.append(t)
                restarted = not first
            elif "HMD on" in line:
                off = bool(self.offs) and (not self.wears or self.offs[-1] > self.wears[-1])
                if off and self.wears and t - self.offs[-1] < self.BLIP:
                    self.offs.pop()  # the sensor flickering: it never came off
                elif off or not self.wears:
                    self.wears.append(t)
            elif "HMD off" in line:
                if not self.offs or (self.wears and self.wears[-1] > self.offs[-1]):
                    self.offs.append(t)
            elif "Accept usercal" in line:
                self.accepts.append(t)
            elif "Reject usercal" in line:
                self.rejects.append(t)
        return restarted

    def worn(self):
        """When the headset last went on (None if not in this log)."""
        return self.wears[-1] if self.wears else None

    def wearing(self):
        """Whether the headset is on, as far as the log says (None: it doesn't say)."""
        if not self.wears and not self.offs:
            return None
        return bool(self.wears) and (not self.offs or self.wears[-1] > self.offs[-1])

    def started(self):
        return self.starts[-1] if self.starts else None

    def accepted_since(self, t):
        return sum(1 for a in self.accepts if a >= t)


def cross_validate(points, mode):
    """Leave-one-out: each point's error under a model fitted on all the others (degrees)."""
    errs = []
    for i in range(len(points)):
        c = Correction()
        c.fit(points[:i] + points[i + 1:], mode)
        cy, cp = c.get(points[i][0], points[i][1], mode)
        errs.append(math.hypot(points[i][2] - cy, points[i][3] - cp))
    return errs


def steady_samples(samples, vergence_jump=1.5, why=None, eye=None):
    """The samples of one look at one spot where the tracker had both eyes: none in a blink
    (openness under half its median over the samples), none where it had lost an eye (its
    variance over EYE_LOST), and none where the angle between the eyes' directions (`lr`, the
    vergence) is more than `vergence_jump` degrees from its median over the samples. The
    vergence itself depends on distance (about 2.8 degrees for a screen 1.3 m away, a
    fraction of one far off), so only a jump away from what it was during this look means
    the tracker lost an eye. Without the mmap there's nothing to judge by: all are kept.
    `eye` (0 left, 1 right; see tracked_eye) judges that eye alone: the other one's loss,
    openness and the vergence don't count.
    `why`, a dict, gets how many were dropped for each reason: "lost_left", "lost_right",
    "lost_both", "blink" and "vergence" (each sample once, for the first that applies)."""
    if why is None:
        why = {}

    def openness(o):
        return o[eye] if eye is not None else min(o)
    # Openness: a blink is a sharp drop from what it was during this look. Not a fixed
    # level: looking down, the upper lids come down with the eyes, and in bright light you
    # squint, so the reading can stay under 0.5 for the whole look while the tracker follows
    # the eyes fine (a calibration dot at the bottom of the bright round failed that way).
    opens = [openness(o) for o in ((smp["src"].get("mmap1") or {}).get("open") for smp in samples) if o]
    floor = max(0.12, 0.5 * statistics.median(opens)) if len(opens) >= 5 else 0.12
    seen = []
    for smp in samples:
        m1 = smp["src"].get("mmap1") or {}
        o = m1.get("open")
        lost = [u > EYE_LOST for u in m1.get("unc") or [0, 0]]
        if eye is not None:
            lost[1 - eye] = False
        # A lost eye's openness reads 0 too, so a lost eye is named before a blink.
        key = ("lost_both" if all(lost) else "lost_left" if lost[0] else "lost_right") if any(lost) else \
            "blink" if o and openness(o) < floor else None
        if key:
            why[key] = why.get(key, 0) + 1
        else:
            seen.append(smp)
    if eye is not None:
        return seen

    def vergence(smp):
        return (smp["src"].get("mmap1") or {}).get("lr", (smp["src"].get("mmap2") or {}).get("lr"))
    have = [v for v in map(vergence, seen) if v is not None]
    if len(have) < 5:
        return seen
    med = statistics.median(have)
    kept = [smp for smp in seen if vergence(smp) is None or abs(vergence(smp) - med) <= vergence_jump]
    if len(kept) < len(seen):
        why["vergence"] = why.get("vergence", 0) + len(seen) - len(kept)
    return kept
