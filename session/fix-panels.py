#!/usr/bin/env python3
"""Bring back a taskbar that Plasma saved against a screen this desktop doesn't have.

Plasma 6.2.5 ties each panel to a screen number (lastScreen in its containment), and the
numbers rank the enabled outputs by priority: 0 is the primary screen. A panel whose
number is past the screen count gets no view and stays hidden; Plasma never moves it,
even on a later start (sanitizeScreenLayout() only remaps a panel whose number has no
desktop, and the Frametop desktop keeps a desktop for every spare output it has seen).
So a taskbar saved on a spare output (issue #18), or on a screen that a smaller layout
dropped, was lost until the config was deleted.

The session runs this before Plasma starts, so nothing races Plasma for the file. Each
panel numbered at or past the screen count moves to screen 0, with its system tray's
containment, and keeps its widgets and settings. A panel is left where it is when screen
0 already has a panel on that edge: it comes back by itself if the screens do. Before
each repair the file is backed up to <file>.ft-bak.last, and <file>.ft-bak keeps it as it
was before the first repair (a later repair never overwrites that one). The changes go
through kwriteconfig6.

  fix-panels.py [--screens N] [--file APPLETSRC] [--check]
    --screens  the desktop's screen count (default: the configured layout's)
    --file     default: $XDG_CONFIG_HOME/plasma-org.kde.plasma.desktop-appletsrc, with
               XDG_CONFIG_HOME defaulting to the Frametop desktop's ~/.config/frametop
    --check    change nothing; list the panels, and exit 1 if one is lost
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

EDGES = {3: "top", 4: "bottom", 5: "left", 6: "right"}  # Plasma::Types::Location
SYSTRAY = "org.kde.plasma.private.systemtray"
GROUP = re.compile(r"\[([^\]]*)\]")


def parse(text):
    """KConfig text -> {(group, subgroup, ...): {key: value}}."""
    groups, cur = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("["):
            cur = tuple(GROUP.findall(line))
            groups.setdefault(cur, {})
        elif cur is not None and "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            groups[cur][re.sub(r"\[\$.*\]$", "", k.strip())] = v.strip()
    return groups


def number(v, default=-1):
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def panels(groups):
    """The panels, by containment id: location, lastScreen, and the ids of their system
    trays' own containments (which sit on the same edge and screen as the panel)."""
    trays, found = {}, {}
    for g, keys in groups.items():
        if len(g) == 5 and g[0] == "Containments" and g[2] == "Applets" and g[4] == "Configuration":
            tray = keys.get("SystrayContainmentId")
            if tray:
                trays.setdefault(g[1], []).append(tray)
    owned = {t for ts in trays.values() for t in ts}
    for g, keys in groups.items():
        if len(g) != 2 or g[0] != "Containments" or number(g[1]) <= 0 or g[1] in owned:
            continue
        loc = number(keys.get("location"), 0)
        if loc in EDGES and keys.get("plugin") != SYSTRAY:
            found[g[1]] = {"location": loc, "screen": number(keys.get("lastScreen")),
                           "plugin": keys.get("plugin", "?"), "trays": trays.get(g[1], [])}
    return dict(sorted(found.items(), key=lambda kv: number(kv[0])))


def plan(groups, screens):
    """(moves, kept): moves are (panel id, from screen, containment ids to put on screen 0);
    kept are (panel id, from screen, why) for lost panels left alone."""
    found = panels(groups)
    taken = {(p["screen"], p["location"]) for p in found.values() if 0 <= p["screen"] < screens}
    moves, kept = [], []
    for pid, p in found.items():
        if p["screen"] < screens:
            continue  # on a screen this desktop has (Plasma puts -1 on the first one itself)
        if (0, p["location"]) in taken:
            kept.append((pid, p["screen"], f"the first screen already has a {EDGES[p['location']]} panel"))
            continue
        taken.add((0, p["location"]))
        moves.append((pid, p["screen"], [pid, *p["trays"]]))
    return moves, kept


def default_screens():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), "..", "layout"))
    import ft_layout
    return ft_layout.screen_count()


def backup(path, dst):
    """Copy path to dst whole or not at all. The copy goes to dst.tmp, reaches the disk, and
    only then takes dst's name, so a copy cut off partway (a full disk, a crash, the battery)
    never leaves a short dst behind, and a dst that's there already stays as it was."""
    tmp = dst + ".tmp"
    try:
        shutil.copy2(path, tmp)
        with open(tmp, "rb") as f:
            os.fsync(f.fileno())
        os.replace(tmp, dst)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--screens", type=int)
    ap.add_argument("--file")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    config = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config/frametop")
    path = a.file or os.path.join(config, "plasma-org.kde.plasma.desktop-appletsrc")
    screens = a.screens if a.screens else default_screens()
    try:
        with open(path) as f:
            groups = parse(f.read())
    except FileNotFoundError:
        if a.check:
            print("no Plasma config yet (Plasma makes the default taskbar)")
        return 0
    moves, kept = plan(groups, screens)

    if a.check:
        found = panels(groups)
        for pid, p in found.items():
            lost = " (lost: no such screen)" if p["screen"] >= screens else ""
            print(f"panel {pid}: screen {p['screen']}, {EDGES[p['location']]}{lost}")
        if not found:
            print("no panels")
        print(f"{screens} screen(s)")
        return 1 if moves or kept else 0

    bak = path + ".ft-bak"
    if moves:
        try:
            if not os.path.exists(bak):
                # The config as it was before the first repair: a later run never overwrites
                # it with an already-repaired state.
                backup(path, bak)
            # The config as it was just before this repair, so this one can be undone too,
            # keeping what changed since the first (a moved panel's old screen is only here).
            backup(path, bak + ".last")
        except OSError as e:
            # No repair without a backup; the next start tries again.
            print(f"frametop: couldn't back up {path} ({e}); left the panels as they are", file=sys.stderr)
            return 1
    for pid, was, ids in moves:
        for cid in ids:
            subprocess.run(["kwriteconfig6", "--file", os.path.abspath(path), "--group", "Containments",
                            "--group", cid, "--key", "lastScreen", "0"], check=True)
        print(f"frametop: panel {pid} was saved on screen {was}, which this desktop doesn't have "
              f"({screens} screen(s)); moved it to the first screen (backups: {bak}.last from before this "
              f"repair, {bak} from before the first)", file=sys.stderr)
    for pid, was, why in kept:
        print(f"frametop: panel {pid} is saved on screen {was}, which this desktop doesn't have; "
              f"left there: {why}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
