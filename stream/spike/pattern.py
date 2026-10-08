#!/usr/bin/env python3
"""Write the S1 colour test pattern as raw RGB24 (width x height x 3 bytes).

Bands from the top: 100% colour bars, 75% colour bars, a 16-step grey ramp,
near-black steps (0-30), near-white steps (225-255), and a black strip where the
clip's moving box runs. ft-dectest shows the same picture as an RGB reference next
to the decoded video, so a wrong YUV matrix or range shows as a visible difference:
range errors lift or crush the near-black and near-white steps, matrix errors shift
the bar hues.

usage: pattern.py WIDTH HEIGHT OUT.rgb
"""
import sys

BARS = [(1, 1, 1), (1, 1, 0), (0, 1, 1), (0, 1, 0), (1, 0, 1), (1, 0, 0), (0, 0, 1), (0, 0, 0)]


def row(width, colours):
    """One row of len(colours) equal columns; the last absorbs the remainder."""
    out = bytearray()
    n = len(colours)
    for i, c in enumerate(colours):
        x0, x1 = width * i // n, width * (i + 1) // n
        out += bytes(c) * (x1 - x0)
    return bytes(out)


def bands(height):
    """(rows, colours) per band, top to bottom; fractions of the height."""
    split = [0.28, 0.20, 0.16, 0.14, 0.14]
    rows = [int(height * f) for f in split]
    rows.append(height - sum(rows))  # the box strip
    level = lambda v: [tuple(int(round(v * 255 * k)) for k in c) for c in BARS]
    return [
        (rows[0], level(1.0)),
        (rows[1], level(0.75)),
        (rows[2], [(v, v, v) for v in (round(i * 255 / 15) for i in range(16))]),
        (rows[3], [(v, v, v) for v in range(0, 32, 2)]),
        (rows[4], [(v, v, v) for v in range(225, 256, 2)]),
        (rows[5], [(0, 0, 0)]),
    ]


def main():
    width, height, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    with open(out, "wb") as f:
        for n, colours in bands(height):
            f.write(row(width, colours) * n)
    # The box strip's top row, for make-clips.sh to place the moving box.
    print(height - bands(height)[-1][0])


if __name__ == "__main__":
    main()
