"""Which side camera is which, in recordings: the rules every reader shares.

Before 2026-10-05, ft-hands named the side cameras by XRService's start-up order and ft-camd bound
their buffers by XRService's descriptor order, so on most starts each side camera's images carried
the other's name (hands/README.md, "Which camera is which"). Both are exact now, and a
tracking ft-hands still tells from the hands (hands/track/sides.h, HANDS_SWAP_SIDES=auto) and publishes
what it found in /run/user/UID/frametop-hands/sides.json (read_live). Two things are recorded:

  swapped         whether ft-camd's naming was backwards during the recording (the truth);
                  None while nobody knows. A hand recorder session has it in session.json
                  "sides": {"swapped", "decided_by", "evidence", ...}; an ft-hands --record
                  recording in DIR/sides.json.
  names_swapped   per recording file: whether the side cameras' names in it were exchanged from
                  ft-camd's (ft-hands --sides 1). A session's take.json "parts": {"sets.bin":
                  {"names_swapped": false}, "sets-2.bin": {...}}; DIR/sides.json "names_swapped":
                  [[first set, bool], ...] (it can change inside a recording when ft-hands decides).

A file's names are right when names_swapped == swapped. Readers rename slam_left <-> slam_right
(and their _dk dark frames) where they differ (needs_rename). Exports always carry the right
names, with names_swapped = swapped written next to them, so applying the rule again is harmless.

Standard library only (the window, validate.py on Windows, the maintainer's tools).
"""
import json
import os
import struct
import time

HDR = struct.Struct("<8sII")       # fh_set_hdr_t (hands/track/record.h)
CAM = struct.Struct("<16sIIQQ")    # fh_set_cam_t: name, width, height, capture_ns, dqbuf_ns
SIDES = {"slam_left": "slam_right", "slam_right": "slam_left",
         "slam_left_dk": "slam_right_dk", "slam_right_dk": "slam_left_dk"}
LIVE_FRESH_S = 6.0   # ft-hands rewrites its sides.json at least every 2 s


def other_name(name):
    """slam_left <-> slam_right (and their dark frames); other names as they are."""
    return SIDES.get(name, name)


def needs_rename(swapped, names_swapped):
    """True when a file's side cameras carry each other's names. Unknown truth: leave them."""
    if swapped is None:
        return False
    return bool(names_swapped) != bool(swapped)


def rename_record(record):
    """One FHSET01 record (bytes) with slam_left and slam_right's names exchanged (and their
    dark frames'). Only the 16-byte name fields change; pixels and lengths stay."""
    out = bytearray(record)
    ncams = HDR.unpack_from(out, 0)[1]
    for k in range(ncams):
        off = HDR.size + k * CAM.size
        raw = bytes(out[off:off + 16])
        name = raw.split(b"\0", 1)[0].decode(errors="replace")
        new = other_name(name)
        if new != name:
            out[off:off + 16] = new.encode().ljust(16, b"\0")
    return bytes(out)


# ------------------------------------------------------------------ sessions and recordings

def session_sides(session_json):
    """A session.json's "sides" (a dict), or {"swapped": None} when it has none."""
    s = (session_json or {}).get("sides")
    return s if isinstance(s, dict) else {"swapped": None}


def part_names_swapped(take_json, part):
    """Whether a take's part file (e.g. "sets-2.bin") was recorded with exchanged names."""
    parts = (take_json or {}).get("parts")
    entry = parts.get(part) if isinstance(parts, dict) else None
    return bool(entry.get("names_swapped")) if isinstance(entry, dict) else False


def part_rename(session_json, take_json, part):
    """Should a session take's part file have its side cameras renamed when read?"""
    return needs_rename(session_sides(session_json).get("swapped"), part_names_swapped(take_json, part))


def recording_runs(rec_dir):
    """An ft-hands --record recording (DIR/sets.bin + DIR/sides.json): [(first set, rename)],
    or [(0, False)] when it has no sides.json (recorded before there was one)."""
    try:
        with open(os.path.join(rec_dir, "sides.json")) as f:
            s = json.load(f)
    except (OSError, ValueError):
        return [(0, False)]
    runs = s.get("names_swapped") or [[0, False]]
    return [(int(first), needs_rename(s.get("swapped"), sw)) for first, sw in runs]


def rename_at(runs, i):
    """recording_runs()'s answer for set i."""
    out = False
    for first, rename in runs:
        if i >= first:
            out = rename
    return out


# ------------------------------------------------------------------ the live decision

def run_dir():
    return "/run/user/%d/frametop-hands" % os.getuid()


def read_live(path=None, ring_path=None, now_ns=None):
    """The tracking ft-hands' sides.json, or None if there's none or it's stale (ft-hands
    rewrites it every 2 s and removes it when it exits). With ring_path, also None when it
    was written for another ft-camd run than the ring there now."""
    path = path or os.path.join(run_dir(), "sides.json")
    try:
        with open(path) as f:
            s = json.load(f)
    except (OSError, ValueError):
        return None
    now = time.clock_gettime_ns(time.CLOCK_MONOTONIC) if now_ns is None else now_ns
    if not isinstance(s, dict) or (now - int(s.get("updated_ns", 0))) / 1e9 > LIVE_FRESH_S:
        return None
    if ring_path:
        try:
            if os.stat(ring_path).st_ino != s.get("ring_ino"):
                return None
        except OSError:
            return None
    if (s.get("state") == "forced, disagrees" and s.get("swapped") is not None
            and bool(s["swapped"]) == bool(s.get("names_swapped"))):
        # An ft-hands built before 2026-10-06 kept a forced HANDS_SWAP_SIDES as the truth even
        # when the hands disagreed. The hands are right: the truth is the other way round.
        s = dict(s, swapped=not s["swapped"], decided_by="auto")
    return s
