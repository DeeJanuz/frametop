#!/usr/bin/env python3
"""The hand recorder's sessions and takes on disk: listing, review, deleting, export.

Standard library only (no Qt, no NumPy), so the window and the command line share it.
The layout is hands/rec/DESIGN.md's "Files":
  BASE/profile.json
  BASE/sessions/<YYYYMMDD-HHMMSS>/session.json, calibration.json
  BASE/sessions/<id>/takes/<NN>-<section>/sets.bin (sets-2.bin, ... after pauses),
                                          prompts.jsonl, poses.jsonl, take.json
  BASE/exports/<session>/
A take's recording is one or more FHSET01 files (hands/track/record.h): per set a header
(magic, ncams, bytes), one fh_set_cam_t per camera, then each camera's 8-bit pixels.
Deleted ranges live in take.json ("deleted": [[from_ns, to_ns], ...], CLOCK_MONOTONIC, the
clock of dqbuf_ns); the files keep every set until export leaves them out.

Side cameras (sides.py): a part recorded before the live tracker decided which side camera is
which may carry slam_left's and slam_right's names the wrong way round. TakeIndex renames them as
it reads (session.json "sides" against take.json "parts"), so review and export see the right
names, and export writes them so: an export's files are always named right, its take.json says
"parts": {"sets.bin": {"names_swapped": <the session's swapped>}}, and its manifest has each
take's "sides". `takes.py sides SESSION` shows the decision; --set swapped|named records one by
hand (e.g. from tools/check_sides.py on a session no live tracker decided).

usage: takes.py [--base DIR] list | takes SESSION | export SESSION | sides SESSION [--set swapped|named]
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sides  # noqa: E402  (hands/rec/sides.py, next to this file)

DEFAULT_BASE = os.path.expanduser("~/.local/share/frametop/hands/contrib")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAGIC = b"FHSET01"
HDR = struct.Struct("<8sII")       # fh_set_hdr_t: magic, ncams, bytes (the whole record)
CAM = struct.Struct("<16sIIQQ")    # fh_set_cam_t: name, width, height, capture_ns, dqbuf_ns
MAX_CAMS = 16                      # SetReader's limit
PART_RE = re.compile(r"^sets(?:-(\d+))?\.bin$")
SESSION_RE = re.compile(r"^\d{8}-\d{6}(?:-\d+)?$")   # session.py adds -2, -3 to a second one in a second
EXPORT_SCHEMA = 1
# zstd as DESIGN.md has it: level 10, two threads, at the lowest CPU priority (CPU work while
# someone is in VR makes the headset stutter).
ZSTD_ARGS = ["-10", "-T2", "-q", "-c"]
ZSTD_PATHS = ("/usr/bin/zstd", "/usr/local/bin/zstd", "/run/host/usr/bin/zstd")
CHUNK = 1 << 20


class Cancelled(Exception):
    pass


def read_json(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {} if default is None else default


def write_json(path, obj):
    """Write through a temporary file, so a crash never leaves half a file."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def find_zstd():
    """The zstd binary, or None. The dev container gets it from setup/dev-container.sh."""
    found = shutil.which("zstd")
    if found:
        return found
    return next((p for p in ZSTD_PATHS if os.access(p, os.X_OK)), None)


def standalone(path=None):
    """The standalone Hand Recorder's build info, or None in a Frametop checkout. Its release
    (github.com/Frametop/frametop-hand-recorder) ships this repo's tree with standalone.json at
    the top: {"name", "version", "frametop": this repo's git describe, "reinstall": how to repair
    an install}. Its binaries are built for the SteamOS host, so nothing runs in the dev container."""
    try:
        with open(path or os.path.join(REPO, "standalone.json")) as f:
            info = json.load(f)
    except (OSError, ValueError):
        return None
    return info if isinstance(info, dict) else None


def tool_version():
    """ft-handrec plus the checkout's git describe, for session.json and the manifest. The
    standalone Hand Recorder has no checkout: the describe it was built from, and its version."""
    info = standalone()
    if info:
        return "ft-handrec %s (%s %s)" % (info.get("frametop") or "unknown", info.get("name") or "standalone",
                                          info.get("version") or "unknown")
    try:
        out = subprocess.run(["git", "-C", REPO, "describe", "--always", "--dirty", "--tags"],
                             capture_output=True, text=True, timeout=5)
        described = out.stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        described = ""
    return "ft-handrec " + (described or "unknown")


def free_bytes(path):
    """Free space on the file system that holds path (or its nearest existing parent)."""
    while path and not os.path.exists(path):
        path = os.path.dirname(path)
    try:
        return shutil.disk_usage(path or "/").free
    except OSError:
        return 0


def tree_stat(path):
    """(bytes, newest mtime) of the files under path."""
    total, newest = 0, 0.0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                st = os.lstat(os.path.join(root, name))
            except OSError:
                continue
            total += st.st_size
            newest = max(newest, st.st_mtime)
    return total, newest


def tree_bytes(path):
    return tree_stat(path)[0]


def in_ranges(t, ranges):
    return any(a <= t <= b for a, b in ranges)


def export_jsonl(src, dst, ranges, keep_controllers):
    """Copy a take's poses.jsonl or prompts.jsonl for export:
    - poses in the deleted ranges are left out, and the live tracker's feedback there (the
      prompt timeline stays: it says what was asked, and shows nothing);
    - without controllers (the checklist's "none"), the controllers' poses are null and feedback
      carries no controller state: controllers lying about still get logged, and those poses
      would read as hands' ground truth.
    Lines that don't parse are copied as they are (validate.py reports them)."""
    poses = os.path.basename(src) == "poses.jsonl"
    with open(src, encoding="utf-8") as f, open(dst, "w", encoding="utf-8", newline="\n") as out:
        for line in f:
            try:
                obj = json.loads(line)
            except ValueError:
                out.write(line)
                continue
            if not isinstance(obj, dict):
                out.write(line)
                continue
            t = obj.get("t")
            gone = isinstance(t, int) and in_ranges(t, ranges)
            if poses:
                if gone:
                    continue
                if not keep_controllers:
                    obj["left"] = obj["right"] = None
            elif obj.get("event") == "feedback":
                if gone:
                    continue
                if not keep_controllers:
                    obj.pop("controller", None)
            out.write(json.dumps(obj, separators=(", ", ": ")) + "\n")


def merge_ranges(ranges):
    """Sorted, with overlapping or touching ranges joined."""
    out = []
    for a, b in sorted((min(a, b), max(a, b)) for a, b in ranges):
        if out and a <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


# ---------------------------------------------------------------- one take's recording

class TakeIndex:
    """Where each set of a take's recording is, without reading pixels. Sets are numbered
    across the parts (sets.bin, sets-2.bin, ...) in order."""

    def __init__(self, take_dir):
        self.dir = take_dir
        self.parts = self._parts(take_dir)
        self.sets = []   # (part, offset, bytes, time_ns): time_ns is the cameras' earliest dqbuf_ns
        self.part_starts = []   # index of each part's first set
        self.cams = []   # the first set's [{name, width, height}], side cameras named right
        # per part: are its side cameras named the wrong way round (sides.py)? read_set and
        # read_raw rename them.
        session = read_json(os.path.join(os.path.dirname(os.path.dirname(take_dir)), "session.json"))
        take = read_json(os.path.join(take_dir, "take.json"))
        self.swapped = sides.session_sides(session).get("swapped")
        self.names_swapped = [sides.part_names_swapped(take, os.path.basename(p)) for p in self.parts]
        self.rename = [sides.needs_rename(self.swapped, n) for n in self.names_swapped]
        for p, path in enumerate(self.parts):
            self.part_starts.append(len(self.sets))
            self._index(p, path)

    @staticmethod
    def _parts(take_dir):
        found = []
        try:
            names = os.listdir(take_dir)
        except OSError:
            return []
        for name in names:
            m = PART_RE.match(name)
            if m:
                found.append((int(m.group(1) or 1), os.path.join(take_dir, name)))
        return [path for _, path in sorted(found)]

    def _index(self, p, path):
        try:
            size = os.path.getsize(path)
            f = open(path, "rb")
        except OSError:
            return
        with f:
            off = 0
            while off + HDR.size <= size:
                f.seek(off)
                head = f.read(HDR.size)
                if len(head) < HDR.size:
                    break
                magic, ncams, nbytes = HDR.unpack(head)
                # A truncated last set (the recorder was killed mid-write) ends the part.
                if magic[:7] != MAGIC or not 0 < ncams <= MAX_CAMS or nbytes < HDR.size or off + nbytes > size:
                    break
                cams = f.read(CAM.size * ncams)
                if len(cams) < CAM.size * ncams:
                    break
                fields = [CAM.unpack_from(cams, i * CAM.size) for i in range(ncams)]
                if not self.cams:
                    self.cams = [{"name": self._name(p, n), "width": w, "height": h} for n, w, h, _, _ in fields]
                self.sets.append((p, off, nbytes, min(c[4] for c in fields)))
                off += nbytes

    def _name(self, p, raw):
        name = raw.split(b"\0", 1)[0].decode(errors="replace")
        return sides.other_name(name) if self.rename[p] else name

    def renamed_sets(self, keep=None):
        """How many of the sets (all, or the indices in keep) read renamed."""
        return sum(1 for i in (range(len(self.sets)) if keep is None else keep) if self.rename[self.sets[i][0]])

    def __len__(self):
        return len(self.sets)

    def time_ns(self, i):
        return self.sets[i][3]

    def duration_s(self):
        """Recorded time: each part's first to last set, so pauses don't count."""
        total = 0
        ends = self.part_starts[1:] + [len(self.sets)]
        for start, end in zip(self.part_starts, ends):
            if end > start:
                total += self.sets[end - 1][3] - self.sets[start][3]
        return total / 1e9

    def bytes(self):
        return sum(s[2] for s in self.sets)

    def read_set(self, i, only=None):
        """Set i's cameras: [{name, width, height, capture_ns, dqbuf_ns, pixels}], pixels as raw
        8-bit grey bytes (width x height, packed). only: a camera name, to read just that one."""
        p, off, nbytes, _ = self.sets[i]
        with open(self.parts[p], "rb") as f:
            f.seek(off)
            ncams = HDR.unpack(f.read(HDR.size))[1]
            heads = f.read(CAM.size * ncams)
            out = []
            px_off = off + HDR.size + CAM.size * ncams
            for k in range(ncams):
                name, w, h, capture_ns, dqbuf_ns = CAM.unpack_from(heads, k * CAM.size)
                name = self._name(p, name)
                if only is None or name == only:
                    f.seek(px_off)
                    out.append({"name": name, "width": w, "height": h, "capture_ns": capture_ns,
                                "dqbuf_ns": dqbuf_ns, "pixels": f.read(w * h)})
                px_off += w * h
            return out

    def read_raw(self, i):
        """Set i's whole record, as it is in the file but with the side cameras named right."""
        p, off, nbytes, _ = self.sets[i]
        with open(self.parts[p], "rb") as f:
            f.seek(off)
            record = f.read(nbytes)
        return sides.rename_record(record) if self.rename[p] else record


_index_cache = {}
_index_lock = threading.Lock()


def take_index(take_dir):
    """A TakeIndex, cached until one of the take's recording files, its take.json or its
    session.json (the side cameras' decision) changes size or time."""
    key_parts = []
    meta = [os.path.join(take_dir, "take.json"), os.path.join(os.path.dirname(os.path.dirname(take_dir)), "session.json")]
    for path in TakeIndex._parts(take_dir) + meta:
        try:
            st = os.stat(path)
            key_parts.append((path, st.st_size, st.st_mtime_ns))
        except OSError:
            pass
    key = tuple(key_parts)
    with _index_lock:
        cached = _index_cache.get(take_dir)
        if cached and cached[0] == key:
            return cached[1]
    index = TakeIndex(take_dir)
    with _index_lock:
        _index_cache[take_dir] = (key, index)
    return index


# ---------------------------------------------------------------- the store

class Store:
    def __init__(self, base=DEFAULT_BASE):
        self.base = os.path.abspath(os.path.expanduser(base))
        self.sessions_dir = os.path.join(self.base, "sessions")
        self.exports_dir = os.path.join(self.base, "exports")
        self.profile_path = os.path.join(self.base, "profile.json")

    # --- paths, checked: ids come from the window, so nothing may climb out of the base
    def session_dir(self, session):
        if not SESSION_RE.match(session or ""):
            raise ValueError(f"not a session id: {session!r}")
        return os.path.join(self.sessions_dir, session)

    def take_dir(self, session, take):
        if not take or "/" in take or take.startswith("."):
            raise ValueError(f"not a take id: {take!r}")
        return os.path.join(self.session_dir(session), "takes", take)

    def export_dir(self, session):
        self.session_dir(session)  # checks the id
        return os.path.join(self.exports_dir, session)

    def profile(self):
        return read_json(self.profile_path)

    # --- listing
    def sessions(self):
        """Newest first: [{id, started, takes, bytes, lighting, status, dry_run, contributor, exported,
        export_stale (changed since), export_bytes}]."""
        out = []
        try:
            names = os.listdir(self.sessions_dir)
        except OSError:
            return out
        for sid in names:
            path = os.path.join(self.sessions_dir, sid)
            if not SESSION_RE.match(sid) or not os.path.isdir(path):
                continue
            meta = read_json(os.path.join(path, "session.json"))
            size, changed = tree_stat(path)
            manifest = os.path.join(self.export_dir(sid), "manifest.json")
            exported = os.path.getmtime(manifest) if os.path.isfile(manifest) else 0.0
            out.append({"id": sid, "started": meta.get("started", ""), "takes": len(self.take_ids(sid)),
                        "bytes": size, "contributor": meta.get("contributor", ""),
                        "lighting": (meta.get("lighting") or {}).get("chosen", ""),
                        "status": meta.get("status", ""), "dry_run": bool(meta.get("dry_run")),
                        "exported": bool(exported), "export_stale": bool(exported) and changed > exported,
                        "export_bytes": tree_bytes(self.export_dir(sid)) if exported else 0})
        return sorted(out, key=lambda s: s["id"], reverse=True)

    def take_ids(self, session):
        try:
            names = os.listdir(os.path.join(self.session_dir(session), "takes"))
        except OSError:
            return []
        return sorted(n for n in names if not n.startswith(".")
                      and os.path.isdir(os.path.join(self.session_dir(session), "takes", n)))

    def takes(self, session):
        """[{id, section, title, status, sets, deleted_sets, ranges, duration_s, bytes, cams}]."""
        out = []
        for tid in self.take_ids(session):
            path = self.take_dir(session, tid)
            meta = read_json(os.path.join(path, "take.json"))
            index = take_index(path)
            ranges = merge_ranges(meta.get("deleted") or [])
            out.append({"id": tid, "section": meta.get("section", tid.split("-", 1)[-1]),
                        "title": meta.get("title") or tid, "status": meta.get("status", "recording"),
                        "sets": len(index), "parts": len(index.parts),
                        "deleted_sets": sum(1 for s in index.sets if in_ranges(s[3], ranges)),
                        "ranges": ranges, "duration_s": index.duration_s(), "bytes": index.bytes(),
                        "cams": [c["name"] for c in index.cams]})
        return out

    def take_meta(self, session, take):
        return read_json(os.path.join(self.take_dir(session, take), "take.json"))

    # --- deleted ranges
    def ranges(self, session, take):
        return merge_ranges(self.take_meta(session, take).get("deleted") or [])

    def _set_ranges(self, session, take, ranges):
        path = os.path.join(self.take_dir(session, take), "take.json")
        meta = read_json(path)
        meta["deleted"] = merge_ranges(ranges)
        write_json(path, meta)
        return meta["deleted"]

    def delete_range(self, session, take, from_ns, to_ns):
        """Leave sets from from_ns to to_ns (dqbuf_ns, inclusive) out of the export."""
        return self._set_ranges(session, take, self.ranges(session, take) + [[int(from_ns), int(to_ns)]])

    def restore_range(self, session, take, i):
        """Take deleted range i (in ranges()' order) back."""
        ranges = self.ranges(session, take)
        if 0 <= i < len(ranges):
            del ranges[i]
        return self._set_ranges(session, take, ranges)

    # --- deleting files
    def delete_take(self, session, take):
        path = self.take_dir(session, take)
        if os.path.isdir(path):
            shutil.rmtree(path)
        meta_path = os.path.join(self.session_dir(session), "session.json")
        meta = read_json(meta_path)
        if take in (meta.get("takes") or []):
            meta["takes"] = [t for t in meta["takes"] if t != take]
            meta.setdefault("deleted_takes", []).append(take)
            write_json(meta_path, meta)

    def delete_session(self, session):
        """The session and its export, if any."""
        for path in (self.session_dir(session), self.export_dir(session)):
            if os.path.isdir(path):
                shutil.rmtree(path)

    def delete_export(self, session):
        path = self.export_dir(session)
        if os.path.isdir(path):
            shutil.rmtree(path)

    # --- export
    def sides(self, session, swapped=None):
        """session.json's side camera decision (sides.py) plus each take's parts; with swapped
        (True/False), record that by hand first ("decided_by": "manual"). No set is rewritten:
        reading and export rename."""
        path = os.path.join(self.session_dir(session), "session.json")
        meta = read_json(path)
        if not meta:
            raise RuntimeError(f"no session {session}")
        if swapped is not None:
            old = sides.session_sides(meta)
            new = {"swapped": bool(swapped), "decided_by": "manual",
                   "decided_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds")}
            if old.get("swapped") is not None:
                new["replaced"] = old
            meta["sides"] = new
            write_json(path, meta)
        out = {"sides": sides.session_sides(meta), "takes": {}}
        for tid in self.take_ids(session):
            index = take_index(self.take_dir(session, tid))
            out["takes"][tid] = {os.path.basename(p): {"names_swapped": n, "renamed_when_read": r}
                                 for p, n, r in zip(index.parts, index.names_swapped, index.rename)}
        return out

    def export(self, session, progress=None, cancel=None, keep_notes=False, low_priority=True):
        """Write exports/<session>/ (DESIGN.md "Export"): manifest.json, calibration.json, per
        take prompts.jsonl, poses.jsonl, take.json and sets.bin.zst (sets in deleted ranges
        left out), then SHA256SUMS. progress(fraction 0..1, text) is called as it goes;
        cancel() (or cancel.is_set()) returning true stops it, raising Cancelled. Nothing is
        left behind on failure or cancel. Returns the export's path.
        low_priority: run this thread at nice 19 as well as zstd (Linux nice is per thread)."""
        zstd = find_zstd()
        if not zstd:
            raise RuntimeError("zstd isn't installed (in the dev container: sudo dnf install zstd)")
        cancelled = getattr(cancel, "is_set", cancel) or (lambda: False)
        report = progress or (lambda fraction, text: None)
        src = self.session_dir(session)
        if not os.path.isdir(src):
            raise RuntimeError(f"no session {session}")
        if low_priority:
            try:
                os.setpriority(os.PRIO_PROCESS, threading.get_native_id(), 19)
            except OSError:
                pass
        final = self.export_dir(session)
        work = final + ".partial"
        if os.path.isdir(work):
            shutil.rmtree(work)
        os.makedirs(work)
        try:
            self._export(session, src, work, zstd, report, cancelled, keep_notes)
            if os.path.isdir(final):
                shutil.rmtree(final)
            os.replace(work, final)
        except BaseException:
            shutil.rmtree(work, ignore_errors=True)
            raise
        report(1.0, "Done")
        return final

    def _export(self, session, src, work, zstd, report, cancelled, keep_notes):
        profile = self.profile()
        meta = read_json(os.path.join(src, "session.json"))
        meta.pop("uploads", None)   # earlier uploads' records (hub.py) aren't part of the contribution
        takes = self.takes(session)
        total = sum(t["bytes"] for t in takes) or 1
        done = 0
        take_entries = []
        keep_controllers = (meta.get("checklist") or {}).get("controllers") == "straps"
        for t in takes:
            if cancelled():
                raise Cancelled()
            tdir = self.take_dir(session, t["id"])
            out = os.path.join(work, "takes", t["id"])
            os.makedirs(out)
            ranges = t["ranges"]
            for name in ("prompts.jsonl", "poses.jsonl"):
                if os.path.isfile(os.path.join(tdir, name)):
                    export_jsonl(os.path.join(tdir, name), os.path.join(out, name), ranges, keep_controllers)
            index = take_index(tdir)
            keep = [i for i in range(len(index)) if not in_ranges(index.time_ns(i), ranges)]
            # one file, named right where the decision is known (sides.py): its names_swapped is
            # then the session's swapped; unknown, it's the parts' own (None if they differ)
            if index.swapped is not None:
                names_swapped = bool(index.swapped)
            else:
                names_swapped = index.names_swapped[0] if len(set(index.names_swapped)) == 1 else None
            take_meta = read_json(os.path.join(tdir, "take.json"))
            if take_meta:
                take_meta["parts"] = {"sets.bin": {"names_swapped": names_swapped}}
                write_json(os.path.join(out, "take.json"), take_meta)
            entry = {"id": t["id"], "section": t["section"], "title": t["title"], "status": t["status"],
                     "sets": len(keep), "sets_deleted": len(index) - len(keep), "cameras": index.cams,
                     "duration_s": round(t["duration_s"], 2), "file": None,
                     "sides": {"names_swapped": names_swapped, "renamed_sets": index.renamed_sets(keep)}}
            done += index.bytes() - sum(index.sets[i][2] for i in keep)   # deleted sets count as done
            if keep:
                entry["file"] = "takes/%s/sets.bin.zst" % t["id"]

                def step(nbytes, title=t["title"]):
                    nonlocal done
                    done += nbytes
                    report(min(done / total, 0.99), f"Compressing {title}")
                entry["raw_bytes"] = self._compress(index, keep, zstd, os.path.join(out, "sets.bin.zst"),
                                                    step, cancelled)
            take_entries.append(entry)
        if cancelled():
            raise Cancelled()
        for name in ("calibration.json", "device.json"):
            if os.path.isfile(os.path.join(src, name)):
                shutil.copyfile(os.path.join(src, name), os.path.join(work, name))
        shown_profile = json.loads(json.dumps(profile))
        if not keep_notes and isinstance(shown_profile.get("optional"), dict):
            shown_profile["optional"].pop("notes", None)
        manifest = {"schema": EXPORT_SCHEMA, "tool": tool_version(),
                    "exported": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                    "session_id": session, "contributor": profile.get("contributor", meta.get("contributor", "")),
                    "consent_version": (profile.get("consent") or {}).get("version", ""),
                    "profile": shown_profile, "session": meta, "takes": take_entries}
        write_json(os.path.join(work, "manifest.json"), manifest)
        report(0.995, "Writing checksums")
        sums = []
        for root, _, files in os.walk(work):
            for name in files:
                path = os.path.join(root, name)
                sums.append((os.path.relpath(path, work), sha256_file(path, cancelled)))
        with open(os.path.join(work, "SHA256SUMS"), "w") as f:
            for rel, digest in sorted(sums):
                f.write(f"{digest}  {rel}\n")

    @staticmethod
    def _compress(index, keep, zstd, dest, step, cancelled):
        """Stream the kept sets through zstd into dest. Returns the uncompressed size."""
        raw = 0
        with open(dest, "wb") as out:
            proc = subprocess.Popen(["nice", "-n", "19", zstd] + ZSTD_ARGS, stdin=subprocess.PIPE, stdout=out,
                                    stderr=subprocess.PIPE)
            try:
                for i in keep:
                    if cancelled():
                        raise Cancelled()
                    record = index.read_raw(i)
                    proc.stdin.write(record)
                    raw += len(record)
                    step(len(record))
                proc.stdin.close()
                err = proc.stderr.read().decode(errors="replace").strip()
                if proc.wait() != 0:
                    raise RuntimeError(f"zstd failed: {err or proc.returncode}")
            except BaseException:
                proc.kill()
                proc.wait()
                raise
            finally:
                proc.stderr.close()
        return raw


def sha256_file(path, cancelled=lambda: False):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            if cancelled():
                raise Cancelled()
            h.update(chunk)
    return h.hexdigest()


def human_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1000
    return f"{n:.1f} TB"


def main():
    ap = argparse.ArgumentParser(description="List, check and export hand recorder sessions.")
    ap.add_argument("--base", default=DEFAULT_BASE)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("takes").add_argument("session")
    sub.add_parser("export").add_argument("session")
    sp = sub.add_parser("sides", help="show (or record) which way round the side cameras were")
    sp.add_argument("session")
    sp.add_argument("--set", choices=("swapped", "named"),
                    help="record the decision by hand (e.g. from tools/check_sides.py on a take)")
    a = ap.parse_args()
    store = Store(a.base)
    if a.cmd == "list":
        for s in store.sessions():
            print(f"{s['id']}  {s['takes']} takes  {human_bytes(s['bytes'])}  {s['lighting'] or '-'}"
                  + ("  exported" if s["exported"] else ""))
    elif a.cmd == "takes":
        for t in store.takes(a.session):
            print(f"{t['id']}  {t['status']}  {t['sets']} sets ({t['deleted_sets']} deleted)  "
                  f"{t['duration_s']:.1f} s  {human_bytes(t['bytes'])}")
    elif a.cmd == "sides":
        print(json.dumps(store.sides(a.session, None if a.set is None else a.set == "swapped"), indent=1))
    else:
        path = store.export(a.session, progress=lambda f, text: print(f"\r{f * 100:5.1f}% {text:40.40}", end="",
                                                                       flush=True))
        print(f"\n{path}  {human_bytes(tree_bytes(path))}")


if __name__ == "__main__":
    main()
