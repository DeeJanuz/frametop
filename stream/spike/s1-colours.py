#!/usr/bin/env python3
"""Spike S1 colour check. Runs ft-dectest head-locked with the decoded video and the RGB
reference alternating on one panel, grabs the headset view in each state from
/dev/video99 (SteamVR's v4l2cam: one eye, the full composite, 1920x1080 RGB24), and
compares the two per colour patch. Both pass through the same compositor, so any
difference comes from the YUV-to-RGB step: none at all without ft-dectest's --convert
(SteamVR samples NV12 raw), the GPU pass with it.

usage: s1-colours.py CLIP.h265 PATTERN.rgb [--seconds 15] [--out DIR] [-- ft-dectest options]

The headset must be worn, or frame-testbench must hold it worn (`worn on`, a fixed pose,
`compositor awake`): the view is black while the display is off. When worn for real, look
straight ahead and keep still; the panel follows the head, so small movements don't matter.
"""
import os
import subprocess
import sys
import threading
import time
from collections import defaultdict

W, H = 1920, 1080
FRAME = W * H * 3
HERE = os.path.dirname(os.path.abspath(__file__))
DECTEST = os.path.join(HERE, "..", "build", "ft-dectest")


def grab(dev):
    """A fresh frame: v4l2loopback hands out queued frames first, so read a few."""
    buf = None
    for _ in range(4):
        buf = dev.read(FRAME)
    return buf


def save_ppm(path, buf):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(buf)


def compare(ref, vid, step=3, margin=0.12, min_share=0.004):
    """Groups the central pixels by their colour in the reference capture, and returns
    (reference colour, mean video colour, pixel count) per group big enough to be a
    patch rather than an edge or the background."""
    groups = defaultdict(lambda: [0, 0, 0, 0])
    x0, x1 = int(W * margin), int(W * (1 - margin))
    y0, y1 = int(H * margin), int(H * (1 - margin))
    total = 0
    for y in range(y0, y1, step):
        row = y * W * 3
        for x in range(x0, x1, step):
            i = row + x * 3
            g = groups[(ref[i], ref[i + 1], ref[i + 2])]
            g[0] += vid[i]
            g[1] += vid[i + 1]
            g[2] += vid[i + 2]
            g[3] += 1
            total += 1
    out = []
    for c, (r, g, b, n) in groups.items():
        if n >= total * min_share:
            out.append((c, (r / n, g / n, b / n), n))
    out.sort(key=lambda e: (sum(e[0]), e[0]))
    return out


def report(name, pairs):
    print(f"\n{name}: {len(pairs)} reference/video pairs")
    worst_all = 0.0
    for k, (ref, vid) in enumerate(pairs):
        rows = compare(ref, vid)
        if len(rows) < 8:
            print(f"  only {len(rows)} patches found: was the display on, and the panel in view?")
            continue
        worst = max(max(abs(v - c) for v, c in zip(vc, rc)) for rc, vc, _ in rows)
        mean = sum(sum(abs(v - c) for v, c in zip(vc, rc)) / 3 * n for rc, vc, n in rows) / sum(n for *_, n in rows)
        worst_all = max(worst_all, worst)
        print(f"  pair {k + 1}: {len(rows)} patches, mean error {mean:.1f}, worst {worst:.1f} (0-255 scale)")
        if k == 0:
            print("    reference RGB      -> decoded RGB          (difference)")
            for rc, vc, n in rows:
                d = tuple(v - c for v, c in zip(vc, rc))
                print("    %3d %3d %3d      -> %5.1f %5.1f %5.1f    (%+5.1f %+5.1f %+5.1f)" % (*rc, *vc, *d))
    return worst_all


def main():
    args = sys.argv[1:]
    extra = []
    if "--" in args:
        i = args.index("--")
        args, extra = args[:i], args[i + 1:]
    clip, pattern = args[0], args[1]
    seconds, out = 15.0, os.path.expanduser("~/.cache/remote-displays-spike/s1")
    for i, a in enumerate(args):
        if a == "--seconds":
            seconds = float(args[i + 1])
        elif a == "--out":
            out = args[i + 1]
    os.makedirs(out, exist_ok=True)
    name = os.path.basename(clip).rsplit(".", 1)[0]

    status = subprocess.run(["frame-job", "--status"], capture_output=True, text=True).stdout
    if "headset: worn" not in status:
        sys.exit("the headset isn't worn: the headset view is black while the display is off")
    dev = open("/dev/video99", "rb", buffering=0)
    grab(dev)  # v4l2cam starts with the first reader; its first frames are black
    cmd = ["frame-job", "--local", "--", "distrobox", "enter", "dev", "--", DECTEST, clip, "--ref", pattern,
           "--headlocked", "--alternate", "2.5", "--distance", "1.0", "--width", "2.6",
           "--seconds", str(seconds)] + extra
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    shots = []  # (state, frame)
    lock = threading.Lock()

    def shoot(state, n):
        time.sleep(1.0)  # the compositor shows the new texture, v4l2cam passes it on
        frame = grab(dev)
        save_ppm(os.path.join(out, f"{name}-{n:02d}-{state}.ppm"), frame)
        with lock:
            shots.append((n, state, frame))

    threads = []
    for line in proc.stdout:
        line = line.rstrip()
        if line.startswith("show "):
            state = line.split()[1]
            t = threading.Thread(target=shoot, args=(state, len(threads)))
            t.start()
            threads.append(t)
        else:
            print("  ft-dectest:", line)
    proc.wait()
    for t in threads:
        t.join()
    dev.close()

    shots.sort()
    pairs = []
    for (_, s1, f1), (_, s2, f2) in zip(shots, shots[1:]):
        if s1 == "ref" and s2 == "video":
            pairs.append((f1, f2))
    worst = report(name, pairs)
    print(f"\n{name}: worst patch error {worst:.1f}")


if __name__ == "__main__":
    main()
