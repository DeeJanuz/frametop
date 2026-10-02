# ft-screens, Frametop's wlroots compositor (screens/build.sh), and ft-handtest.
# Linked against nixpkgs' openvr (the same v2.15.6 header screens/build.sh pins) instead of
# /opt/steamvr's libopenvr_api; libopenvr_api finds the runtime through
# ~/.config/openvr/openvrpaths.vrpath. GL comes from /run/opengl-driver (RUNPATH, via
# autoAddDriverRunpath), which Home Manager's targets.genericLinux.gpu provides.
{
  lib,
  stdenv,
  fetchurl,
  pkg-config,
  autoAddDriverRunpath,
  wlroots_0_20,
  wayland,
  wayland-protocols,
  libxkbcommon,
  libdrm,
  pixman,
  libGL,
  libgbm,
  openvr,
  vrclientDeps,
  src,
}:

let
  # keyboard.cpp's key labels; the same pin as screens/build.sh.
  stbTruetype = fetchurl {
    url = "https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_truetype.h";
    hash = "sha256-7NMLBeDdT+o6E8JoEN2eGZLcN5BJSCw5PVoZ5rUJCqs=";
  };
in
stdenv.mkDerivation {
  pname = "ft-screens";
  version = "0-unstable";
  inherit src;
  sourceRoot = "source/screens";

  nativeBuildInputs = [
    pkg-config
    autoAddDriverRunpath
  ];
  buildInputs = [
    wlroots_0_20
    wayland
    wayland-protocols
    libxkbcommon
    libdrm
    pixman
    libGL
    libgbm
    openvr
  ]
  ++ vrclientDeps.buildInputs;

  # The same commands as screens/build.sh, minus the downloads and the /opt/steamvr link.
  buildPhase = ''
    runHook preBuild
    mkdir -p build/include
    cp ${stbTruetype} build/include/stb_truetype.h
    $CC -std=c11 -O2 -Wall -Wno-unused-parameter -c -o build/compositor.o compositor.c \
      $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon libdrm pixman-1)
    cxx="$CXX -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include \
      $(pkg-config --cflags egl glesv2 gbm libdrm openvr)"
    $cxx -c -o build/vr.o vr.cpp
    $cxx -c -o build/keyboard.o keyboard.cpp
    $cxx -c -o build/handcut.o handcut.cpp
    $cxx -c -o build/handtest.o handtest.cpp
    vrlibs="$(pkg-config --libs egl glesv2 gbm openvr) ${vrclientDeps.ldflags}"
    $CXX -o build/ft-screens build/compositor.o build/vr.o build/keyboard.o build/handcut.o \
      $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon) $vrlibs
    $CXX -o build/ft-handtest build/handtest.o build/handcut.o $vrlibs
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 -t $out/bin build/ft-screens build/ft-handtest
    runHook postInstall
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    ${vrclientDeps.check}
    vrclient_check $out/bin/ft-screens
    vrclient_check $out/bin/ft-handtest
    runHook postInstallCheck
  '';

  meta = {
    description = "Frametop's compositor: each desktop screen as its own SteamVR panel";
    mainProgram = "ft-screens";
    platforms = lib.platforms.linux;
  };
}
