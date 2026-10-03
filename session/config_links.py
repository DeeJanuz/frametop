#!/usr/bin/env python3
"""Give the Frametop desktop the user's config and state, apart from Plasma's own files.
The session runs it when SHARE_CONFIG=1 is set in ~/.config/frametop.conf.

The session points XDG_CONFIG_HOME at ~/.config/frametop and XDG_STATE_HOME at
~/.local/state/frametop, so this desktop and the stock one keep their own screen layout,
panels, and shortcuts. Every app started in the desktop inherits that, though, and would
otherwise see an empty config (no shell or editor settings, logged out of every app). So at each start,
everything in the real folder that isn't Plasma's (KEEP_CONFIG, KEEP_STATE) is linked in
(name -> ../name). A file or folder in the way, made here before, is moved to .replaced/.
Links whose real entry is gone are removed. Entries the user linked elsewhere are left.

Usage: config_links.py <real dir> <desktop's dir> config|state
"""
import os
import shutil
import sys

# The desktop's own: what both desktops' Plasma and KWin write for their own screens,
# panels, input, and session, and what Frametop writes for this one (the decoration in
# kwinrc, loginMode in ksmserverrc, its shortcuts, ft-floatd's autostart). kdeglobals
# (theme, fonts) is shared on purpose.
KEEP_CONFIG = {
    "autostart", "kactivitymanagerdrc", "kactivitymanagerd-statsrc", "kcminputrc", "kconf_updaterc",
    "kded5rc", "kded6rc", "kdedefaults", "kglobalshortcutsrc", "khotkeysrc", "ksmserverrc",
    "kwinoutputconfig.json", "kwinrc", "kwinrulesrc", "plasma-org.kde.plasma.desktop-appletsrc",
    "plasmashellrc", "powerdevilrc", "powermanagementprofilesrc",
}
KEEP_STATE = {"kwinstaterc", "plasmashellstaterc"}
KEEPS = {"config": KEEP_CONFIG, "state": KEEP_STATE}

REPLACED = ".replaced"  # entries moved out of the way, never deleted

# What an entry in the desktop's folder is.
OURS = "ours"    # a link to ../<name>, made here
OTHER = "other"  # a link to anywhere else: the user's, left alone
REAL = "real"    # a file or folder

# Actions.
LINK = "link"        # link <name> to ../<name>
REPLACE = "replace"  # move <name> to .replaced/, then link it
UNLINK = "unlink"    # remove our link: its real entry is gone


def plan(real, local, keep, own):
    """The actions that make the desktop's folder show the real one's entries.

    real: names in the real folder. local: name -> OURS, OTHER, or REAL for the desktop's
    folder. keep: the desktop's own names. own: the desktop folder's name in the real one."""
    skip = set(keep) | {own, REPLACED}
    actions = []
    for name in sorted(set(real) - skip):
        kind = local.get(name)
        if kind is None:
            actions.append((LINK, name))
        elif kind == REAL:
            actions.append((REPLACE, name))
    for name in sorted(set(local) - set(real) - skip):
        if local[name] == OURS:
            actions.append((UNLINK, name))
    return actions


def link_target(name):
    return os.path.join("..", name)


def kind(path, name):
    if not os.path.islink(path):
        return REAL
    return OURS if os.readlink(path) == link_target(name) else OTHER


def free_path(path):
    """path, or path.1, path.2, ... if it's taken."""
    if not os.path.lexists(path):
        return path
    n = 1
    while os.path.lexists(f"{path}.{n}"):
        n += 1
    return f"{path}.{n}"


def sync(real_dir, local_dir, keep):
    """Apply plan() to the folders. Returns the moves, as (name, where it went)."""
    real = set(os.listdir(real_dir))
    local = {n: kind(os.path.join(local_dir, n), n) for n in os.listdir(local_dir)}
    own = os.path.basename(os.path.normpath(local_dir))

    moved = []
    for action, name in plan(real, local, keep, own):
        path = os.path.join(local_dir, name)
        if action == UNLINK:
            os.unlink(path)
            continue

        if action == REPLACE:
            os.makedirs(os.path.join(local_dir, REPLACED), exist_ok=True)
            dest = free_path(os.path.join(local_dir, REPLACED, name))
            shutil.move(path, dest)
            moved.append((name, dest))

        os.symlink(link_target(name), path)
    return moved


def main():
    if len(sys.argv) != 4 or sys.argv[3] not in KEEPS:
        sys.exit(__doc__.split("Usage: ")[1].strip())
    real_dir, local_dir, which = sys.argv[1:]
    for name, dest in sync(real_dir, local_dir, KEEPS[which]):
        print(f"config_links: {name} was the desktop's own; moved to {dest}, now ../{name}")


if __name__ == "__main__":
    main()
