"""Check that the side cameras' images carry the right names (slam_left vs slam_right).

usage: python tools/check_sides.py REC [--sets N] [--pair side|upper] [--calib DIR] [--json]
       python tools/check_sides.py --ring [--sets N] [--pair side|upper]   (live, from ft-camd's ring)

REC is a recording folder (its sets.bin) or a sets file (a take's sets-N.bin). It checks the
names as they are in the files: a recording's sides.json or session.json "sides" (the hand
recorder's) isn't applied. Exits 0 when the names are right, 3 when they're swapped, and 2 when
it can't tell (too little texture in view, or the headset isn't worn). --pair upper checks the
upper pair (upper_left vs upper_right) the same way. --calib DIR uses DIR/calibration.json and
DIR/device.json (a hand recorder session's, or cut.py's) instead of /persist. --json prints one
line: {"pair", "verdict": "named"|"swapped"|"unknown", "named", "swapped", "matches", "sets"}.

ft-hands decides the side cameras' naming by itself (HANDS_SWAP_SIDES=auto, track/sides.h);
this is the independent check, from the scene rather than hands.

Before 2026-10-05 the side cameras' images often carried each other's names (hands/README.md,
"Which camera is which"); recordings from then may still. The tracker then sees every hand in
one camera only, at the wrong depth. This
matches features between the two images and measures how close each pair's rays pass
with the factory calibration, once as named and once swapped: true matches meet in
front of both cameras only under the right naming.
"""
import argparse
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from tools.show_set import index, read_set  # noqa: E402
from tools import calib  # noqa: E402


PIPES = {'msm_vfe3_video0': 'slam_right', 'msm_vfe4_video0': 'slam_left',    # with the colour module
         'msm_vfe2_video0': 'upper_left', 'msm_vfe2_video1': 'upper_right'}


def ring_names():
    """{/dev/videoN's N: calibration name} as ft-hands names them: by XRService's log
    (camcheck.py), else by capture pipe (PIPES)."""
    import camcheck
    try:
        by_log = {node: name for name, node in camcheck.read_log(camcheck.newest_log()).camera_map().items()}
    except (OSError, ValueError):
        by_log = {}
    if by_log:
        return by_log
    out = {}
    for path in os.listdir('/sys/class/video4linux'):
        if path.startswith('video'):
            with open('/sys/class/video4linux/%s/name' % path) as f:
                name = PIPES.get(f.read().strip())
            if name:
                out[int(path[5:])] = name
    return out
PAIRS = {'side': ('slam_left', 'slam_right'), 'upper': ('upper_left', 'upper_right')}


def load_cams(calib_dir=None):
    if calib_dir:
        return calib.load(os.path.join(calib_dir, 'calibration.json'), os.path.join(calib_dir, 'device.json'))
    return calib.load()


def matches(a, b):
    """Pixel pairs (N,2), (N,2) of ORB matches between two grey images."""
    clahe = cv2.createCLAHE(2.0, (8, 8))
    orb = cv2.ORB_create(3000)
    ka, da = orb.detectAndCompute(clahe.apply(a), None)
    kb, db = orb.detectAndCompute(clahe.apply(b), None)
    if da is None or db is None:
        return np.zeros((0, 2)), np.zeros((0, 2))
    pairs = cv2.BFMatcher(cv2.NORM_HAMMING).knnMatch(da, db, k=2)
    good = [p[0] for p in pairs if len(p) == 2 and p[0].distance < 0.75 * p[1].distance]
    return (np.array([ka[m.queryIdx].pt for m in good]).reshape(-1, 2),
            np.array([kb[m.trainIdx].pt for m in good]).reshape(-1, 2))


def meet(cam_a, cam_b, ua, ub):
    """Per match: closest distance between the two rays (m), and whether they meet in front of both."""
    ra, rb = cam_a.rays(ua), cam_b.rays(ub)
    w = cam_b.origin - cam_a.origin
    n = np.cross(ra, rb)
    nn = np.linalg.norm(n, axis=1)
    dist = np.abs(w @ n.T) / np.maximum(nn, 1e-12)
    # ray parameters at the closest points
    ta = np.einsum('ij,ij->i', np.cross(np.broadcast_to(w, rb.shape), rb), n) / np.maximum(nn ** 2, 1e-12)
    tb = np.einsum('ij,ij->i', np.cross(np.broadcast_to(w, ra.shape), ra), n) / np.maximum(nn ** 2, 1e-12)
    return dist, (ta > 0.05) & (tb > 0.05)


def score(cam_a, cam_b, ua, ub):
    """Share of matches whose rays meet within 1 cm, in front of both cameras."""
    if len(ua) == 0:
        return 0.0
    d, front = meet(cam_a, cam_b, ua, ub)
    return float(np.mean((d < 0.01) & front))


def recorded_pairs(rec, count, names=PAIRS['side']):
    """(label, left image, right image) from sets spread across a recording (or sets file)."""
    path = rec if os.path.isfile(rec) else os.path.join(rec, 'sets.bin')
    offs = index(path)
    if not offs:
        return
    for n in sorted(set(np.linspace(0, len(offs) - 1, count).astype(int))):
        images = read_set(path, offs[n])
        if names[0] in images and names[1] in images:
            yield 'set %5d' % n, images[names[0]][0], images[names[1]][0]


def live_pairs(count, names=PAIRS['side']):
    """(label, left image, right image) from ft-camd's ring, half a second apart."""
    import time
    from tools.ring import Ring
    ring = Ring()
    if not ring.alive():
        sys.exit('ft-camd isn\'t running (no heartbeat)')
    cams = {}
    names_of = ring_names()
    for c in ring.cams:
        name = names_of.get(c.node)
        if name and not c.name.endswith('-dark'):
            cams[name] = c
    for k in range(count):
        a, b = ring.read(cams[names[0]]), ring.read(cams[names[1]])
        if a is not None and b is not None:
            yield 'frame %2d' % k, a.image, b.image
        time.sleep(0.5)


def verdict(named, swapped, n, total_matches):
    """'named', 'swapped' or 'unknown', from the summed per-set scores: the winner must meet
    clearly more often (by 0.08 a set) and at least 4 times as often. The upper cameras' small
    images meet within 1 cm less often (0.1-0.4 a set as named, 0 swapped) than the side ones (0.4-0.9)."""
    hi, lo = max(named, swapped), min(named, swapped)
    if n == 0 or total_matches < 100 or (hi - lo) / n < 0.08 or lo > 0.25 * hi:
        return 'unknown'
    return 'named' if named > swapped else 'swapped'


def check(pairs, left, right, out=print):
    """Score (label, left image, right image) pairs: (verdict, named, swapped, matches, sets)."""
    named = swapped = 0.0
    n = total_matches = 0
    for label, img_l, img_r in pairs:
        ua, ub = matches(img_l, img_r)
        s_named = score(left, right, ua, ub)     # the left-named image seen by the left camera
        s_swapped = score(right, left, ua, ub)   # ... by the right camera
        named, swapped, n, total_matches = named + s_named, swapped + s_swapped, n + 1, total_matches + len(ua)
        out('%s: %4d matches, meeting as named %3.0f%%, swapped %3.0f%%' %
            (label, len(ua), 100 * s_named, 100 * s_swapped))
    return verdict(named, swapped, n, total_matches), named / max(n, 1), swapped / max(n, 1), total_matches, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rec', nargs='?')
    ap.add_argument('--ring', action='store_true', help='check the live cameras instead of a recording')
    ap.add_argument('--sets', type=int, default=8, help='how many sets or live frames to check')
    ap.add_argument('--pair', choices=sorted(PAIRS), default='side')
    ap.add_argument('--calib', help='folder with calibration.json and device.json (default: /persist)')
    ap.add_argument('--json', action='store_true', help='one JSON line instead of the report')
    a = ap.parse_args()
    if not a.ring and not a.rec:
        ap.error('give a recording or --ring')
    cams = load_cams(a.calib)
    names = PAIRS[a.pair]
    left, right = cams[names[0]], cams[names[1]]
    pairs = live_pairs(a.sets, names) if a.ring else recorded_pairs(a.rec, a.sets, names)
    v, named, swapped, total, n = check(pairs, left, right, out=(lambda s: None) if a.json else print)
    what = 'side cameras' if a.pair == 'side' else 'upper cameras'
    if a.json:
        print(json.dumps({'pair': a.pair, 'verdict': v, 'named': round(named, 3), 'swapped': round(swapped, 3),
                          'matches': total, 'sets': n}))
    elif v == 'unknown':
        print('%s: can\'t tell (%d matches)' % (what, total))
    else:
        print('%s: %s (named %.2f, swapped %.2f)' % (what, 'as named' if v == 'named' else 'SWAPPED', named, swapped))
    sys.exit({'named': 0, 'swapped': 3, 'unknown': 2}[v])


if __name__ == '__main__':
    main()
