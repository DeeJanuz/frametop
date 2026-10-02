# ft-powerd, which turns the headset's displays off while it isn't used (power/build.sh).
{
  lib,
  stdenv,
  openvr,
  src,
}:

stdenv.mkDerivation {
  pname = "ft-powerd";
  version = "0-unstable";
  inherit src;
  sourceRoot = "source/power";

  buildInputs = [ openvr ];

  buildPhase = ''
    runHook preBuild
    $CXX -std=c++17 -O2 -Wall -Wno-unused-parameter -I${openvr}/include/openvr \
      -o ft-powerd ft-powerd.cpp -lopenvr_api -ldl
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 -t $out/bin ft-powerd
    runHook postInstall
  '';

  meta = {
    description = "Frametop power: the headset's displays off while nobody uses it";
    mainProgram = "ft-powerd";
    platforms = lib.platforms.linux;
  };
}
