# driver_ft_pointer.so, the ft_pointer SteamVR driver (pointer/driver/build.sh), in the
# layout SteamVR expects: share/frametop/ft_pointer/{driver.vrdrivermanifest,resources,bin/<plat>}.
#
# It loads into the host's vrserver, so it must run against SteamOS's glibc (2.39), not
# nixpkgs'. zig c++ links against glibc 2.39's symbol versions and links its libc++
# statically, so the .so needs only libc/libm/libdl/libpthread and has no RUNPATH.
# (openvr_driver.h's std::string/std::vector helpers are inline wrappers over the
# char*-based interfaces: no std:: type crosses into vrserver.) installCheckPhase enforces
# all of it, the same check as pointer/driver/build.sh.
{
  lib,
  stdenv,
  zig,
  binutils,
  openvr,
  src,
  glibcVersion ? "2.39",
}:

let
  arch = stdenv.hostPlatform.parsed.cpu.name;
  # SteamVR's names for the platform directories.
  steamvrPlatform =
    {
      aarch64 = "linuxarm64";
      x86_64 = "linux64";
    }
    .${arch} or (throw "ft-pointer-driver: no SteamVR platform for ${arch}");
in
stdenv.mkDerivation {
  pname = "ft-pointer-driver";
  version = "0-unstable";
  inherit src;
  sourceRoot = "source/pointer/driver";

  # zig is a cross compiler: it runs on the build machine whatever the target.
  depsBuildBuild = [ zig ];
  nativeBuildInputs = [ binutils ];
  dontConfigure = true;
  # Nothing of nixpkgs' may end up in the .so: no RUNPATH, no store references.
  dontPatchELF = true;
  dontStrip = true;

  buildPhase = ''
    runHook preBuild
    export ZIG_GLOBAL_CACHE_DIR=$TMPDIR/zig-cache ZIG_LOCAL_CACHE_DIR=$TMPDIR/zig-cache
    # Only the driver's entry point is exported; without this the static libc++'s
    # operator new/delete would be too (build.sh uses --exclude-libs, which zig lacks).
    echo '{ global: HmdDriverFactory; local: *; };' > exports.map
    zig c++ -target ${arch}-linux-gnu.${glibcVersion} \
      -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fno-math-errno -Wall -Wno-unused-parameter \
      -Wno-nullability-completeness -s -Wl,--version-script=exports.map \
      -I${openvr}/include/openvr \
      -o driver_ft_pointer.so driver_ft_pointer.cpp -lpthread
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    dest=$out/share/frametop/ft_pointer
    mkdir -p $dest/bin/${steamvrPlatform}
    cp -r ft_pointer/. $dest/
    install -m755 driver_ft_pointer.so $dest/bin/${steamvrPlatform}/
    runHook postInstall
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    so=$out/share/frametop/ft_pointer/bin/${steamvrPlatform}/driver_ft_pointer.so
    max=$(objdump -T $so | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
    echo "newest glibc symbol: $max"
    [ "$(printf '%s\n' "$max" GLIBC_${glibcVersion} | sort -V | tail -1)" = GLIBC_${glibcVersion} ] \
      || { echo "needs newer glibc than ${glibcVersion}" >&2; exit 1; }
    needed=$(objdump -p $so | awk '$1 == "NEEDED" { print $2 }')
    echo "NEEDED: $(echo $needed)"
    for lib in $needed; do
      case $lib in
        libc.so.6 | libm.so.6 | libdl.so.2 | libpthread.so.0 | ld-linux-*.so.*) ;;
        *) echo "unexpected NEEDED $lib" >&2; exit 1 ;;
      esac
    done
    if objdump -p $so | grep -qE '^\s*(RUNPATH|RPATH)\s'; then
      echo "has a RUNPATH/RPATH" >&2; exit 1
    fi
    if grep -q /nix/store $so; then
      echo "references /nix/store" >&2; exit 1
    fi
    exports=$(objdump -T $so | awk '/^[0-9a-f]+ / && $0 !~ /\*UND\*/ { print $NF }' | sort -u)
    echo "exports: $(echo $exports)"
    [ "$exports" = HmdDriverFactory ] || { echo "should export HmdDriverFactory only" >&2; exit 1; }
    runHook postInstallCheck
  '';

  passthru = { inherit steamvrPlatform; };

  meta = {
    description = "Frametop's ft_pointer SteamVR driver (loads into the host's vrserver)";
    platforms = [
      "aarch64-linux"
      "x86_64-linux"
    ];
  };
}
