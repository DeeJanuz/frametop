#!/usr/bin/env python3
"""Check what Frametop needs from SteamOS, for after a SteamOS update.

On the Frame, a SteamOS update replaces SteamVR, KWin, gamescope, and the kernel along with
the rest of the OS image. Frametop lives in the home folder and survives it, but it uses
SteamVR and KWin interfaces that an update can change or drop, some of them undocumented.
This checks each one, and compares the versions with the last ones recorded as working.

  scripts/update-check.py              # check (scripts/doctor.sh runs this)
  scripts/update-check.py --mark-good  # check, then record these versions as working

From a PC: ssh frame python3 - [--mark-good] < scripts/update-check.py

It only reads. It connects to SteamVR as a background app (which never starts SteamVR),
runs `vrcmd --overlays` as the pointer helper does, and maps the eye tracker's shared
memory read-only. It never starts, stops, or restarts anything.
"""
import json
import math
import mmap
import os
import platform
import re
import struct
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
UID = os.getuid()
KNOWN_GOOD = os.path.join(HOME, ".local/state/frametop/known-good.json")
STEAMVR_BIN = "/opt/steamvr/bin/linuxarm64"
LAUNCHER = os.path.join(HOME, ".local/share/applications/deckard-nested-desktop.desktop")
VRPATHS = os.path.join(HOME, ".config/openvr/openvrpaths.vrpath")
BACKLIGHT = "/sys/class/backlight/ae94000.dsi.0/brightness"  # as in power/ft-powerd.cpp
EYE_MMAP = "/dev/shm/eye-server.mmap"
HOST_GLIBC = (2, 39)  # the newest the pointer driver may need (pointer/driver/build.sh)

# Packages in the OS image that Frametop depends on, and what to try by hand when one changes.
PACKAGES = {
    "deckard-steamvr-rel": "the 3D mouse on the dashboard, on SteamVR Settings, and on a SteamVR "
                           "window's grab bar; picking up a controller; mapped controller buttons; gaze",
    "kwin": "clicks near the far edge of a screen whose scale isn't 1; every screen comes back "
            "after a desktop restart; floating a window",
    "plasma-workspace": "the taskbar and panels after a desktop restart",
    "gamescope": "the headset's volume buttons with nothing focused; typing goes where you last clicked",
    "bluez": "a Bluetooth mouse reconnecting after it sleeps",
}
KERNEL_HINT = "display power (ft-powerd) and hand tracking"

# Frametop's user services, and the control socket each binds once it's up.
UNITS = {
    "frametop-input-relay": "@frametop_relay",
    "frametop-pointer": "@ft_pointer_helper",
    "frametop-gaze": "@ft_gazed",
    "frametop-power": "@ft_powerd",
    "frametop-camd": None,
    "frametop-hands": None,
}

# eye-server.mmap offsets, the same as gaze/ft-gaze.cpp's (packed, little-endian).
EYE_COUNTER, EYE_TIME, EYE_OPEN, EYE_NEED = 0x38, 0x157, 0x1CB, 0x1D3
EYE_VECTORS = (0x15F, 0x16B, 0x19B, 0x1A7)  # set 1 left, right; set 2 left, right

# Runs in a child process, so a SteamVR that hangs can't hang the check.
OPENVR_PROBE = r"""
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
lib.VR_InitInternal2.restype = ctypes.c_uint32
lib.VR_InitInternal2.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_char_p]
lib.VR_GetVRInitErrorAsSymbol.restype = ctypes.c_char_p
lib.VR_IsInterfaceVersionValid.restype = ctypes.c_bool
lib.VR_IsInterfaceVersionValid.argtypes = [ctypes.c_char_p]
err = ctypes.c_int(0)
lib.VR_InitInternal2(ctypes.byref(err), 3, None)  # VRApplication_Background
if err.value:
    print("init", lib.VR_GetVRInitErrorAsSymbol(err.value).decode())
    sys.exit(1)
for name in sys.argv[2:]:
    print(name, int(lib.VR_IsInterfaceVersionValid(name.encode())))
lib.VR_ShutdownInternal()
"""

failed = False


def report(state, label, detail=""):
    global failed
    failed = failed or state == "FAIL"
    print(f"{state:<6}{label}{': ' + detail if detail else ''}", flush=True)


def run(*cmd, timeout=15, env=None):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        return p.returncode, p.stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, str(e)


def systemctl(*args):
    return run("systemctl", "--user", *args)[1].strip()


def unix_sockets():
    with open("/proc/net/unix") as f:
        return {parts[7] for parts in map(str.split, f) if len(parts) > 7}


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


# --- versions

def current_versions():
    versions = {}
    with open("/etc/os-release") as f:
        for line in f:
            if line.startswith("BUILD_ID="):
                versions["SteamOS build"] = line.split("=", 1)[1].strip().strip('"')
    for line in run("pacman", "-Q", *PACKAGES)[1].splitlines():
        name, _, version = line.partition(" ")
        if name in PACKAGES:
            versions[name] = version
    versions["kernel"] = platform.release()
    return versions


def check_versions(versions):
    try:
        with open(KNOWN_GOOD) as f:
            good = json.load(f)
        date, old = good["date"], good["versions"]
    except (OSError, ValueError, KeyError, TypeError):
        report("warn", "known-good versions", "none recorded yet; run scripts/doctor.sh --mark-good "
               "once Frametop works")
        return
    changed = [k for k in sorted(set(versions) | set(old)) if old.get(k) != versions.get(k)]
    if not changed:
        report("ok", "versions", f"the same as when marked good ({date})")
    for k in changed:
        hint = PACKAGES.get(k) or (KERNEL_HINT if k == "kernel" else "")
        report("warn", f"{k} changed since {date}",
               f"{old.get(k, 'none')} -> {versions.get(k, 'none')}" + (f"; try {hint}" if hint else ""))


# --- the host, without SteamVR

def check_host():
    needed = [
        ("/usr/bin/kwin_wayland_wrapper", "FAIL", "the desktop can't start KWin"),
        ("/usr/bin/startplasma-wayland", "FAIL", "the desktop can't start Plasma"),
        ("/usr/bin/dbus-run-session", "FAIL", "the desktop can't start its session bus"),
        (f"{STEAMVR_BIN}/vrcmd", "FAIL", "the 3D mouse can't find panels"),
        (f"{STEAMVR_BIN}/vrpathreg", "warn", "the pointer driver can't be installed or removed"),
        ("/usr/share/deckard/mesavars.sh", "warn", "the desktop starts without SteamOS's Mesa settings"),
        ("/etc/profile.d/flatpak.sh", "warn", "Flatpak apps may open Discover instead of starting"),
        ("/usr/share/applications/deckard-nested-desktop.desktop", "warn",
         "SteamOS's Desktop launcher entry is gone or renamed, so Frametop's copy may not replace it"),
    ]
    missing = [n for n in needed if not os.path.exists(n[0])]
    for path, state, effect in missing:
        report(state, f"{path} missing", effect)
    if not missing:
        report("ok", "host files", f"all {len(needed)} the desktop and pointer use are there")

    try:
        with open("/usr/bin/kwin_wayland", "rb") as f:
            # Qt keeps the option name as a UTF-16 string literal.
            output_count = "output-count".encode("utf-16-le") in f.read()
    except OSError:
        output_count = False
    if output_count:
        report("ok", "KWin", "has --output-count (one output per screen)")
    else:
        report("FAIL", "KWin", "no --output-count option: the desktop gets one screen at most")

    if systemctl("cat", "steamvr.service"):
        report("ok", "steamvr.service", "Frametop's services start and stop with it")
    else:
        report("FAIL", "steamvr.service", "gone or renamed; Frametop's services are ordered on it")

    glibc = os.confstr("CS_GNU_LIBC_VERSION") or ""
    have = tuple(int(x) for x in re.findall(r"\d+", glibc)[:2])
    want = ".".join(map(str, HOST_GLIBC))
    if have >= HOST_GLIBC:
        report("ok", "glibc", f"{glibc}, the pointer driver needs {want}")
    else:
        report("FAIL", "glibc", f"{glibc}, older than the {want} the pointer driver needs")

    try:
        with open(VRPATHS) as f:
            drivers = json.load(f).get("external_drivers") or []
    except (OSError, ValueError):
        drivers = []
    driver = next((d for d in drivers if os.path.basename(d.rstrip("/")) == "ft_pointer"), None)
    if driver and os.path.isfile(os.path.join(driver, "bin/linuxarm64/driver_ft_pointer.so")):
        report("ok", "pointer driver", f"registered with SteamVR ({driver})")
    else:
        report("FAIL", "pointer driver", "not registered with SteamVR; run pointer/driver/install.sh install, "
               "then restart SteamVR")

    session = launcher_session()
    if session is None:
        report("warn", "launcher", "Launch a program -> Desktop starts the stock desktop "
               "(./desktops.sh install brings Frametop back)")
    elif os.path.isfile(session):
        report("ok", "launcher", f"Desktop starts {session}")
    else:
        report("FAIL", "launcher", f"Desktop starts {session}, which doesn't exist")

    check_nix_vrclient()

    if systemctl("is-enabled", "frametop-power") == "enabled":
        if os.access(BACKLIGHT, os.W_OK):
            report("ok", "backlight", "ft-powerd can turn the displays off")
        else:
            report("FAIL", "backlight", f"{BACKLIGHT} isn't writable, so ft-powerd can't turn the displays off")


def elf_needed(path):
    """The NEEDED entries of a 64-bit little-endian ELF file (None if it can't be read)."""
    try:
        with open(path, "rb") as f:
            data = f.read()
        if data[:6] != b"\x7fELF\x02\x01":
            return None
        phoff, = struct.unpack_from("<Q", data, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        loads, dynamic = [], None
        for i in range(phnum):
            ptype, _, offset, vaddr, _, filesz = struct.unpack_from("<IIQQQQ", data, phoff + i * phentsize)
            if ptype == 1:
                loads.append((vaddr, offset, filesz))
            elif ptype == 2:
                dynamic = (offset, filesz)
        entries = [struct.unpack_from("<qQ", data, dynamic[0] + i) for i in range(0, dynamic[1], 16)]
        strtab = next(v for tag, v in entries if tag == 5)  # DT_STRTAB, an address
        strtab = next(off + strtab - va for va, off, size in loads if va <= strtab < va + size)
        return [data[strtab + v:data.index(b"\0", strtab + v)].decode() for tag, v in entries if tag == 1]
    except (OSError, ValueError, StopIteration, TypeError, struct.error):
        return None


def check_nix_vrclient():
    """Programs from the Nix packages link nixpkgs' libopenvr_api, which loads SteamVR's
    vrclient.so into them. Their loader never looks in /usr/lib, so vrclient.so may only
    need libraries a Nix program has loaded already (glibc's, libstdc++, libgcc_s)."""
    session = launcher_session() or ""
    units = [systemctl("show", "-p", "ExecStart", "--value", u + ".service") for u in UNITS]
    if not any("/nix/store/" in x for x in [session] + units):
        return
    vrclient = f"{STEAMVR_BIN}/vrclient.so"
    needed = elf_needed(vrclient)
    if needed is None:
        report("warn", "vrclient.so", f"can't read {vrclient}'s libraries, for the Nix-built programs")
        return
    known = re.compile(r"(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|ld-linux-[\w-]+)\.so(\.\d+)*$")
    others = [n for n in needed if not known.match(n)]
    if others:
        report("FAIL", "vrclient.so", f"needs {', '.join(others)}, which the Nix-built programs can't load "
               "(nix/README.md, Known risks)")
    else:
        report("ok", "vrclient.so", "needs only libraries the Nix-built programs have")


def launcher_session():
    try:
        with open(LAUNCHER) as f:
            m = re.search(r"^Exec=(\S+)", f.read(), re.M)
    except OSError:
        return None
    return m.group(1) if m else None


# --- with SteamVR running

def installed_binaries():
    """The programs the services and the launcher run: {path: what runs it}."""
    found = {}
    for unit in UNITS:
        argv = re.search(r"argv\[\]=([^;]*)", systemctl("show", "-p", "ExecStart", "--value", unit + ".service"))
        for arg in (argv.group(1).split() if argv else []):
            # The repo's builds, or the Nix packages' (nix/README.md).
            if not arg.startswith((HOME, "/nix/store/")) or not os.path.isfile(arg):
                continue
            if is_elf(arg):
                found[arg] = unit
            else:
                # A script runs the programs built next to it (ft-gazed runs build/ft-gaze).
                build = os.path.join(os.path.dirname(arg), "build")
                for name in sorted(os.listdir(build)) if os.path.isdir(build) else []:
                    if is_elf(os.path.join(build, name)):
                        found[os.path.join(build, name)] = unit
    session = launcher_session()
    if session:
        screens = os.path.normpath(os.path.join(os.path.dirname(os.path.realpath(session)),
                                                "../screens/build/ft-screens"))
        if os.path.isfile(screens):
            found[screens] = "the desktop"
    return found


def check_openvr():
    needs = {}  # interface version: programs built against it
    for path in installed_binaries():
        with open(path, "rb") as f:
            for name in set(re.findall(rb"IVR[A-Za-z]+_\d{3}", f.read())):
                needs.setdefault(name.decode(), []).append(os.path.basename(path))
    if not needs:
        report("skip", "OpenVR interfaces", "no installed Frametop programs found")
        return
    code, out = run(sys.executable, "-c", OPENVR_PROBE, f"{STEAMVR_BIN}/libopenvr_api.so", *sorted(needs),
                    timeout=30)
    served = dict(line.split() for line in out.splitlines() if len(line.split()) == 2)
    if code != 0 or "init" in served:
        why = served.get("init") or out.strip() or f"exit {code}"
        report("FAIL", "OpenVR", f"can't connect to SteamVR as a background app ({why})")
        return
    gone = [n for n in sorted(needs) if served.get(n) != "1"]
    for name in gone:
        report("FAIL", f"OpenVR {name}", f"this SteamVR doesn't serve it; rebuild {', '.join(sorted(needs[name]))} "
               "against a newer OpenVR header")
    if not gone:
        programs = sorted({p for ps in needs.values() for p in ps})
        report("ok", "OpenVR interfaces", f"all {len(needs)} that {', '.join(programs)} use are served")


def check_overlays(desktop_up):
    code, out = run(f"{STEAMVR_BIN}/vrcmd", "--overlays",
                    env=dict(os.environ, LD_LIBRARY_PATH=STEAMVR_BIN))
    # The format ft-pointer.cpp and ft_layout.py parse: 'key' -- 'name', WxH visible VROverlayType_...
    keys = re.findall(r"^'([^']+)' -- '.*VROverlayType_", out, re.M)
    if not keys:
        report("FAIL", "vrcmd --overlays", "lists no overlays in the expected format, so the 3D mouse can't "
               "find panels (pointer/helper/ft-pointer.cpp and layout/ft_layout.py parse it)")
    elif desktop_up and not any(re.fullmatch(r"frametop\.screen\.\d+", k) for k in keys):
        report("FAIL", "vrcmd --overlays", f"lists {len(keys)} overlays, but none of the desktop's screens")
    else:
        report("ok", "vrcmd --overlays", f"{len(keys)} overlays in the format the pointer parses")


def check_services(sockets, desktop_up):
    for unit, socket in UNITS.items():
        if systemctl("is-enabled", unit) != "enabled":
            continue
        state = systemctl("is-active", unit)
        if state != "active":
            report("FAIL", unit, f"{state}; see journalctl --user -u {unit}")
        elif socket and socket not in sockets:
            report("FAIL", unit, f"runs, but hasn't opened {socket}")
        else:
            report("ok", unit, "running")
    if "@ft_pointer" in sockets:
        report("ok", "ft_pointer driver", "SteamVR loaded it")
    else:
        report("FAIL", "ft_pointer driver", "SteamVR didn't load it; see pointer/driver/install.sh log")
    if not desktop_up:
        report("skip", "desktop", "not running; start it and check again to cover the screens")
    elif "@ft_screens" in sockets:
        report("ok", "desktop", "ft-screens is running")
    else:
        report("FAIL", "desktop", "ft-screens runs, but hasn't opened @ft_screens")
    if systemctl("is-enabled", "frametop-camd") == "enabled" and run("pgrep", "-x", "XRService")[0] != 0:
        report("FAIL", "XRService", "not running, so hand tracking has no cameras")


def check_eye_tracker():
    gaze = systemctl("is-enabled", "frametop-gaze") == "enabled"
    try:
        with open(EYE_MMAP, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    except (OSError, ValueError):
        report("FAIL" if gaze else "skip", "eye tracker", f"{EYE_MMAP} isn't there")
        return
    if len(m) < EYE_NEED:
        report("FAIL", "eye tracker", f"{EYE_MMAP} is {len(m)} bytes, smaller than gaze/ft-gaze.cpp reads")
        return
    first = struct.unpack_from("<I", m, EYE_COUNTER)[0]
    time.sleep(0.3)
    if struct.unpack_from("<I", m, EYE_COUNTER)[0] == first:
        report("skip", "eye tracker", "idle, so its layout wasn't checked")
        return
    age = time.clock_gettime(time.CLOCK_MONOTONIC_RAW) - struct.unpack_from("<d", m, EYE_TIME)[0]
    units = sum(abs(math.sqrt(sum(x * x for x in struct.unpack_from("<3f", m, o))) - 1) < 0.02
                for o in EYE_VECTORS)
    # A lost eye can zero its vector, so two of the four are enough. The timestamp is the
    # strong check: a fresh CLOCK_MONOTONIC_RAW double doesn't land on that offset by chance.
    if abs(age) < 1 and units >= 2:
        report("ok", "eye tracker", "eye-server.mmap still has the layout gaze/ft-gaze.cpp reads")
    else:
        report("FAIL" if gaze else "warn", "eye tracker", f"eye-server.mmap layout changed (sample age "
               f"{age:.3g} s, {units} of 4 gaze vectors unit length); update the offsets in gaze/ft-gaze.cpp")


def main():
    args = sys.argv[1:]
    if args not in ([], ["--mark-good"]):
        sys.exit("usage: update-check.py [--mark-good]")
    # A terminal in the desktop has its session's runtime dir and bus; systemctl needs the real ones.
    os.environ["XDG_RUNTIME_DIR"] = f"/run/user/{UID}"
    os.environ["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path=/run/user/{UID}/bus"

    versions = current_versions()
    check_versions(versions)
    check_host()
    steamvr_up = run("pgrep", "-x", "vrserver")[0] == 0
    if not steamvr_up:
        report("skip", "SteamVR checks", "SteamVR isn't running")
    else:
        sockets = unix_sockets()
        desktop_up = run("pgrep", "-x", "ft-screens")[0] == 0
        check_openvr()
        check_overlays(desktop_up)
        check_services(sockets, desktop_up)
        check_eye_tracker()

    if args == ["--mark-good"]:
        if failed:
            print("Not recording these versions as working: fix the failures first.")
        elif not steamvr_up:
            print("Not recording these versions as working: start SteamVR, so its checks run too.")
            sys.exit(1)
        else:
            os.makedirs(os.path.dirname(KNOWN_GOOD), exist_ok=True)
            with open(KNOWN_GOOD, "w") as f:
                json.dump({"date": time.strftime("%Y-%m-%d"), "versions": versions}, f, indent=2)
            print(f"Recorded these versions as working in {KNOWN_GOOD}.")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
