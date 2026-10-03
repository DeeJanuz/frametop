# ft-powerd, which turns the headset's displays off while it isn't used (power/build.sh).
{
  lib,
  stdenv,
  openvr,
  vrclientDeps,
  src,
}:

stdenv.mkDerivation {
  pname = "ft-powerd";
  version = "0-unstable";
  inherit src;
  sourceRoot = "source/power";

  buildInputs = [ openvr ] ++ vrclientDeps.buildInputs;

  buildPhase = ''
    runHook preBuild
    $CXX -std=c++17 -O2 -Wall -Wno-unused-parameter -I${openvr}/include/openvr \
      -o ft-powerd ft-powerd.cpp -lopenvr_api -ldl ${vrclientDeps.ldflags}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 -t $out/bin ft-powerd
    runHook postInstall
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    ${vrclientDeps.check}
    vrclient_check $out/bin/ft-powerd
    runHook postInstallCheck
  '';

  meta = {
    description = "Frametop power: the headset's displays off while nobody uses it";
    mainProgram = "ft-powerd";
    platforms = lib.platforms.linux;
  };
}
