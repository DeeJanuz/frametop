# Frametop's scripts, Python tools, and settings apps, with the programs built by the other
# packages, as one tree in share/frametop that mirrors the repo: the scripts find each other
# by relative path ($here/../layout/ft-layout, <exe>/../../../layout/ft-layout from
# ft-screens), so they keep doing that here, and the only changes are the defaults of the
# repo's env hooks (FRAMETOP_SCREENS_BIN, FRAMETOP_PYTHON, FRAMETOP_STARTPLASMA): nothing is
# exported into the session, so the host Plasma it starts gets a clean environment.
#
# withSettingsApps adds Frametop Display Settings and Frametop Input Settings (PySide6 +
# Kirigami). They import ft_layout and call desktops.sh by relative path, so they live in
# the same tree. Without them (frametop-scripts) there's no Qt in the closure.
{
  lib,
  stdenvNoCC,
  makeWrapper,
  bash,
  python3,
  glib,
  gobject-introspection,
  qt6,
  kdePackages,
  ft-screens,
  ft-pointer,
  ft-powerd,
  src,
  withSettingsApps ? true,
  # The host's Plasma (Plasma isn't packaged here: the session nests the SteamOS one).
  startPlasma ? "/usr/bin/startplasma-wayland",
}:

let
  # ft-floatd and Launch as Standalone use dbus-python and PyGObject (GLib, Gio). The
  # typelib path is set for this interpreter only, not exported to the desktop.
  scriptPython = python3.withPackages (ps: [
    ps.dbus-python
    ps.pygobject3
  ]);
  typelibPath = lib.makeSearchPath "lib/girepository-1.0" [
    glib.out
    gobject-introspection
  ];
  appPython = python3.withPackages (ps: [ ps.pyside6 ]);
  share = "share/frametop";
in
stdenvNoCC.mkDerivation {
  pname = if withSettingsApps then "frametop" else "frametop-scripts";
  version = "0-unstable";
  inherit src;

  nativeBuildInputs = [ makeWrapper ] ++ lib.optional withSettingsApps qt6.wrapQtAppsHook;
  # For patchShebangs (the host's bash and python3 would do too, but these are pinned).
  buildInputs = [
    bash
    scriptPython
  ]
  ++ lib.optionals withSettingsApps [
    qt6.qtbase
    qt6.qtdeclarative
    qt6.qtwayland
    qt6.qtsvg
    kdePackages.kirigami
    kdePackages.qqc2-desktop-style
    kdePackages.breeze-icons
  ];

  dontConfigure = true;
  dontBuild = true;
  # wrapQtAppsHook would wrap everything executable in the tree; only the two apps get it.
  dontWrapQtApps = true;
  # ft-screens' copy is already fixed up; patchelf's --shrink-rpath would drop
  # /run/opengl-driver/lib, which isn't there at build time.
  dontPatchELF = true;

  installPhase = ''
    runHook preInstall
    tree=$out/${share}
    mkdir -p $tree
    cp -r README.md LICENSE desktops.sh decoration display-settings docs float input \
      input-settings layout remote scripts session $tree/
    mkdir -p $tree/pointer/helper $tree/screens/build $tree/power/build
    cp -r pointer/helper/actions $tree/pointer/helper/

    # The programs, where the scripts and their own lookups expect them. ft-screens is a
    # copy: it finds ft-layout from its own real path (/proc/self/exe).
    install -m755 ${ft-screens}/bin/ft-screens $tree/screens/build/ft-screens
    mkdir -p $tree/pointer/helper/build
    ln -s ${ft-pointer}/lib/ft-pointer/bin/ft-pointer $tree/pointer/helper/build/ft-pointer
    ln -s ${ft-powerd}/bin/ft-powerd $tree/power/build/ft-powerd

    makeWrapper ${scriptPython}/bin/python3 $out/libexec/frametop/ft-python \
      --prefix GI_TYPELIB_PATH : ${typelibPath}

    # Defaults of the repo's env hooks.
    substituteInPlace $tree/session/frametop-session.sh \
      --replace-fail 'screens_bin=''${FRAMETOP_SCREENS_BIN:-}' \
                     "screens_bin=\''${FRAMETOP_SCREENS_BIN:-$tree/screens/build/ft-screens}" \
      --replace-fail '"''${FRAMETOP_PYTHON:-python3}"' "\"\''${FRAMETOP_PYTHON:-$out/libexec/frametop/ft-python}\"" \
      --replace-fail '"''${FRAMETOP_STARTPLASMA:-startplasma-wayland}"' "\"\''${FRAMETOP_STARTPLASMA:-${startPlasma}}\""
    for f in layout/ft-layout float/ft-floatd float/ft-float-apps; do
      substituteInPlace $tree/$f \
        --replace-fail '"''${FRAMETOP_PYTHON:-python3}"' "\"\''${FRAMETOP_PYTHON:-$out/libexec/frametop/ft-python}\""
    done
    # ft-input-relay runs with /usr/bin/python3 upstream; the unit here names the interpreter.
    ln -s ${scriptPython}/bin/python3 $out/libexec/frametop/python3

    # Menu entries, filled in (Home Manager links them into ~/.local/share/applications).
    mkdir -p $out/${share}-entries
    sed "s|@SESSION@|$tree/session/frametop-session.sh|" session/deckard-nested-desktop.desktop \
      > $out/${share}-entries/deckard-nested-desktop.desktop
    for f in display-settings/ft-layout-reset display-settings/ft-screens-toggle \
      ${lib.optionalString withSettingsApps "display-settings/ft-display-settings input-settings/ft-input-settings"}; do
      sed "s|@REPO@|$tree|g" $f.desktop > $out/${share}-entries/$(basename $f).desktop
    done

    mkdir -p $out/bin
    ln -s ../${share}/layout/ft-layout $out/bin/ft-layout
    ln -s ../${share}/float/ft-float $out/bin/ft-float
    runHook postInstall
  '';

  postFixup = ''
    tree=$out/${share}
  ''
  # The settings apps' launchers run the app in the dev container. Here they run it directly
  # (same name, so the menu entries and anything calling them keep working).
  + lib.optionalString withSettingsApps ''
    for app in display-settings/ft-display-settings:ft_display_settings.py \
      input-settings/ft-input-settings:ft_input_settings.py; do
      launcher=$tree/''${app%%:*} script=$(dirname $tree/''${app%%:*})/''${app##*:}
      rm $launcher
      makeQtWrapper ${appPython}/bin/python3 $launcher \
        --add-flags $script \
        --set-default QT_QPA_PLATFORM 'wayland;xcb'
      ln -s ../${share}/''${app%%:*} $out/bin/$(basename $launcher)
    done
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    cmp ${ft-screens}/bin/ft-screens $out/${share}/screens/build/ft-screens \
      || { echo "the tree's ft-screens differs from the package's" >&2; exit 1; }
    runHook postInstallCheck
  '';

  passthru = {
    inherit scriptPython appPython withSettingsApps;
    tree = share;
  };

  meta = {
    description = "Frametop: a multi-screen Plasma desktop in VR on the Steam Frame";
    homepage = "https://github.com/JRMurr/frametop";
    platforms = lib.platforms.linux;
  };
}
