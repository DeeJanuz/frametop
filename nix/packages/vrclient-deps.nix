# The libraries SteamVR's vrclient.so needs beyond glibc, libstdc++, and libgcc_s, on the
# Frame (readelf -d /opt/steamvr/bin/linuxarm64/vrclient.so). nixpkgs' libopenvr_api loads
# vrclient.so into ft-screens, ft-pointer, and ft-powerd, and a library loaded that way finds
# its own dependencies only among libraries the process already has (by soname) or in the
# loader's default path, which for a Nix program is nixpkgs' glibc, never /usr/lib. The
# programs' RUNPATH doesn't apply to it. So each program links these itself, and they're
# loaded before vrclient.so needs them. scripts/update-check.py checks the list after a
# SteamVR update.
{ libGL, libuuid }:
let
  libs = [
    "GL"
    "EGL"
    "uuid"
  ];
  sonames = [
    "libGL.so.1"
    "libEGL.so.1"
    "libuuid.so.1"
  ];
in
{
  buildInputs = [
    libGL
    libuuid
  ];
  # Linked whether or not the program calls into them.
  ldflags = "-Wl,--push-state,--no-as-needed ${toString (map (l: "-l${l}") libs)} -Wl,--pop-state";
  # installCheckPhase lines: the program at $1 has them all as NEEDED.
  check = ''
    vrclient_check() {
      local needed; needed=$($READELF -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
      for so in ${toString sonames}; do
        grep -qx "$so" <<< "$needed" || { echo "$1 doesn't link $so, which vrclient.so needs" >&2; exit 1; }
      done
      echo "$1 links what vrclient.so needs: ${toString sonames}"
    }
  '';
}
