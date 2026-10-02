# ft-pointer, the universal 3D mouse's helper (pointer/helper/build.sh). It reads its
# action manifest from <exe dir>/../actions, so the binary lives in lib/ft-pointer/bin next
# to lib/ft-pointer/actions; bin/ft-pointer links to it (/proc/self/exe resolves the link).
{
  lib,
  stdenv,
  openvr,
  src,
}:

stdenv.mkDerivation {
  pname = "ft-pointer";
  version = "0-unstable";
  inherit src;
  sourceRoot = "source/pointer";

  buildInputs = [ openvr ];

  buildPhase = ''
    runHook preBuild
    $CXX -std=c++17 -O2 -Wall -Wno-unused-parameter -I${openvr}/include/openvr -Icommon \
      -o ft-pointer helper/ft-pointer.cpp -lopenvr_api -ldl -lpthread
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 ft-pointer $out/lib/ft-pointer/bin/ft-pointer
    cp -r helper/actions $out/lib/ft-pointer/actions
    mkdir -p $out/bin
    ln -s ../lib/ft-pointer/bin/ft-pointer $out/bin/ft-pointer
    runHook postInstall
  '';

  meta = {
    description = "Frametop's 3D mouse helper: cursor, panel collision, the ft_pointer driver's input";
    mainProgram = "ft-pointer";
    platforms = lib.platforms.linux;
  };
}
