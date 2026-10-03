# Home Manager module for Frametop on a Steam Frame (SteamOS, not NixOS). What
# install.sh sets up, from the flake's packages: the input relay, pointer and power
# services, the ft_pointer SteamVR driver, the launcher's Desktop entry, and the settings
# apps' menu entries. See nix/README.md.
self:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  inherit (lib)
    mkEnableOption
    mkIf
    mkOption
    types
    literalExpression
    ;
  cfg = config.programs.frametop;
  system = pkgs.stdenv.hostPlatform.system;
  flakePackages =
    self.packages.${system} or (throw "programs.frametop: no Frametop packages for ${system}");

  pkg = cfg.package;
  tree = "${pkg}/${pkg.passthru.tree}";
  entries = "${pkg}/share/frametop-entries";
  platform = cfg.driverPackage.passthru.steamvrPlatform;
  steamvrBin = "${cfg.host.steamvr}/bin/${platform}";
  # The same path pointer/driver/install.sh uses, so SteamVR's registration matches.
  driverDir = "${config.xdg.dataHome}/frametop/ft_pointer";
  openvrPaths = "${config.xdg.configHome}/openvr/openvrpaths.vrpath";

  # SteamVR opens input devices only at startup, so the relay must not start for the first
  # time while SteamVR runs (it would take the mouse away from it). Home Manager starts new
  # services right away, so this skips that start. A restart (after a crash or an update) is
  # fine: systemd keeps the relay's virtual devices, so SteamVR keeps seeing them.
  relayCondition = pkgs.writeShellScript "frametop-input-relay-condition" ''
    systemctl --user is-active --quiet steamvr.service || exit 0
    n=$(systemctl --user show -p NFileDescriptorStore --value frametop-input-relay.service)
    [ "''${n:-0}" -gt 0 ] && exit 0
    echo "SteamVR is running: the input relay starts with it next time (restart SteamVR to start it now)"
    exit 1
  '';
in
{
  options.programs.frametop = {
    enable = mkEnableOption "Frametop, the multi-screen Plasma desktop in VR on the Steam Frame";

    package = mkOption {
      type = types.package;
      default = flakePackages.frametop-apps.override { startPlasma = cfg.host.startPlasma; };
      defaultText = literalExpression "frametop.packages.\${system}.frametop-apps.override { startPlasma = config.programs.frametop.host.startPlasma; }";
      description = ''
        Frametop's scripts, tools, and settings apps (with ft-screens, ft-pointer, and
        ft-powerd in their tree). `frametop-scripts` leaves out the settings apps.
      '';
    };

    driverPackage = mkOption {
      type = types.package;
      default = flakePackages.ft-pointer-driver;
      defaultText = literalExpression "frametop.packages.\${system}.ft-pointer-driver";
      description = "The ft_pointer SteamVR driver.";
    };

    launcher.enable = mkOption {
      type = types.bool;
      default = true;
      description = ''
        Make the VR launcher's Desktop entry start Frametop (an override of
        deckard-nested-desktop.desktop in ~/.local/share/applications, as
        `desktops.sh install` does). Off, the launcher opens the stock SteamOS desktop.
      '';
    };

    pointer.enable = mkOption {
      type = types.bool;
      default = true;
      description = ''
        The universal 3D mouse: the ft_pointer SteamVR driver (registered with vrpathreg)
        and the frametop-pointer service, which starts and stops with SteamVR.
      '';
    };

    power.enable = mkOption {
      type = types.bool;
      default = true;
      description = ''
        The frametop-power service (ft-powerd), which turns the displays off while the
        headset isn't used. It starts and stops with SteamVR.
      '';
    };

    shareConfig = mkOption {
      type = types.bool;
      default = true;
      description = ''
        Apps in the Frametop desktop use the normal ~/.config and ~/.local/state (all but
        Plasma's own files), so they keep their config and logins. Written to
        SHARE_CONFIG in ~/.config/frametop.conf at each activation; takes effect at the next
        desktop start.
      '';
    };

    host = {
      steamvr = mkOption {
        type = types.str;
        default = "/opt/steamvr";
        description = "The host's SteamVR runtime (for vrpathreg).";
      };
      startPlasma = mkOption {
        type = types.str;
        default = "/usr/bin/startplasma-wayland";
        description = ''
          The host's startplasma-wayland, which the session runs nested in ft-screens.
          Plasma isn't packaged here: don't put kdePackages in home.packages either, since
          ~/.nix-profile/bin comes first in PATH and the session would find them.
        '';
      };
      vrpathreg = mkOption {
        type = types.str;
        default = "${steamvrBin}/vrpathreg";
        defaultText = literalExpression ''"''${host.steamvr}/bin/linuxarm64/vrpathreg"'';
        description = "The host's vrpathreg, which registers the ft_pointer driver with SteamVR.";
      };
      kwriteconfig = mkOption {
        type = types.str;
        default = "/usr/bin/kwriteconfig6";
        description = "The host's kwriteconfig6, for the Frametop desktop's global shortcuts.";
      };
    };
  };

  config = mkIf cfg.enable {
    assertions = [
      (lib.hm.assertions.assertPlatform "programs.frametop" pkgs lib.platforms.linux)
    ];

    # Wrappers only (ft-layout, ft-float, the settings apps): no Qt or KDE libraries in
    # ~/.nix-profile/bin.
    home.packages = [ pkg ];

    systemd.user.services.frametop-input-relay = {
      Unit = {
        Description = "Frametop input relay: stable virtual mouse and keyboard for SteamVR";
        Documentation = [ "file://${tree}/README.md" ];
        # SteamVR opens input devices only at startup, so the virtual devices must exist first.
        Before = [ "steamvr.service" ];
      };
      Service = {
        # READY=1 is sent only after the virtual devices exist.
        Type = "notify";
        NotifyAccess = "main";
        ExecCondition = "${relayCondition}";
        ExecStart = "${pkg}/libexec/frametop/python3 ${tree}/input/input-relay.py";
        Restart = "on-failure";
        RestartSec = 1;
        # systemd holds the virtual devices' file descriptors across relay restarts, so
        # SteamVR keeps the same devices. Clear with:
        # systemctl --user clean --what=fdstore frametop-input-relay
        FileDescriptorStoreMax = 4;
        FileDescriptorStorePreserve = "yes";
      };
      Install.WantedBy = [
        "default.target"
        "steamvr.service"
      ];
    };

    systemd.user.services.frametop-pointer = mkIf cfg.pointer.enable {
      Unit = {
        Description = "Frametop pointer helper: the universal 3D mouse (cursor, collision, ft_pointer driver)";
        Documentation = [ "file://${tree}/README.md" ];
        # Needs SteamVR's IPC; it starts and stops with SteamVR.
        After = [
          "steamvr.service"
          "frametop-input-relay.service"
        ];
        PartOf = [ "steamvr.service" ];
        Requisite = [ "steamvr.service" ];
      };
      Service = {
        ExecStart = "${tree}/pointer/helper/build/ft-pointer";
        Restart = "on-failure";
        RestartSec = 3;
      };
      Install.WantedBy = [ "steamvr.service" ];
    };

    systemd.user.services.frametop-power = mkIf cfg.power.enable {
      Unit = {
        Description = "Frametop power: turns the headset's displays off while nobody is using it";
        Documentation = [ "file://${tree}/docs/reference.md" ];
        # Needs SteamVR's IPC; it starts and stops with SteamVR.
        After = [ "steamvr.service" ];
        PartOf = [ "steamvr.service" ];
      };
      Service = {
        # SIGTERM makes ft-powerd turn the displays back on before it exits.
        ExecStart = "${tree}/power/build/ft-powerd";
        Restart = "on-failure";
        RestartSec = 3;
      };
      Install.WantedBy = [ "steamvr.service" ];
    };

    xdg.dataFile = lib.mkMerge [
      (mkIf cfg.launcher.enable {
        "applications/deckard-nested-desktop.desktop".source = "${entries}/deckard-nested-desktop.desktop";
      })
      (lib.listToAttrs (
        map
          (name: lib.nameValuePair "applications/${name}.desktop" { source = "${entries}/${name}.desktop"; })
          (
            [
              "ft-layout-reset"
              "ft-screens-toggle"
            ]
            ++ lib.optionals (pkg.passthru.withSettingsApps or false) [
              "ft-display-settings"
              "ft-input-settings"
            ]
          )
      ))
      (mkIf cfg.pointer.enable {
        # A link to the store: the path SteamVR has registered stays the same across
        # generations. SteamVR loads drivers at startup, so a new driver takes a restart.
        "frametop/ft_pointer".source = "${cfg.driverPackage}/share/frametop/ft_pointer";
      })
    ];

    home.activation = {
      # ~/.config/frametop.conf stays the user's: the settings apps write it. Only a
      # missing one is created, from the example, as desktops.sh install does (and with the
      # 3D mouse on, as install.sh does).
      frametopConfig = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        conf=${lib.escapeShellArg config.xdg.configHome}/frametop.conf
        if [ ! -e "$conf" ]; then
          run mkdir -p "$(dirname "$conf")"
          run install -m 644 ${tree}/session/frametop.conf.example "$conf"
          ${lib.optionalString cfg.pointer.enable ''run sed -i 's/^POINTER=0/POINTER=1/' "$conf"''}
          verboseEcho "frametop: created $conf"
        fi
        share=${if cfg.shareConfig then "1" else "0"}
        if ! grep -q "^SHARE_CONFIG=$share" "$conf"; then
          run sed -i "s/^SHARE_CONFIG=[0-9]*/SHARE_CONFIG=$share/" "$conf"
          grep -q '^SHARE_CONFIG=' "$conf" || run sh -c "echo SHARE_CONFIG=$share >> '$conf'"
        fi
      '';

      # The ft_pointer driver's registration with SteamVR, once: the path doesn't change
      # between generations. (Turning pointer.enable off leaves it registered, pointing at
      # nothing, which SteamVR skips; `vrpathreg removedriver` removes it.)
      frametopDriver = mkIf cfg.pointer.enable (
        lib.hm.dag.entryAfter [ "linkGeneration" ] ''
          driver=${lib.escapeShellArg driverDir}
          if grep -qF "\"$driver\"" ${lib.escapeShellArg openvrPaths} 2>/dev/null; then
            verboseEcho "frametop: ft_pointer driver already registered"
          elif [ -x ${lib.escapeShellArg cfg.host.vrpathreg} ]; then
            run env LD_LIBRARY_PATH=${lib.escapeShellArg steamvrBin} ${lib.escapeShellArg cfg.host.vrpathreg} adddriver "$driver"
            echo "frametop: registered the ft_pointer driver; restart SteamVR to load it"
          else
            warnEcho "frametop: no ${cfg.host.vrpathreg}, so the ft_pointer driver isn't registered with SteamVR"
          fi
        ''
      );

      # Each profile's launcher entry (they point into the store, so they're rewritten for
      # each generation) and the Frametop desktop's global shortcuts, as
      # display-settings/install.sh does.
      frametopLaunchers = lib.hm.dag.entryAfter [ "linkGeneration" ] ''
        run ${tree}/layout/ft-layout launchers || warnEcho "frametop: ft-layout launchers failed"
        if [ -x ${lib.escapeShellArg cfg.host.kwriteconfig} ]; then
          shortcuts=${lib.escapeShellArg config.xdg.configHome}/frametop/kglobalshortcutsrc
          run mkdir -p "$(dirname "$shortcuts")"
          run ${cfg.host.kwriteconfig} --file "$shortcuts" --group services --group ft-layout-reset.desktop --key _launch 'Meta+Shift+R'
          run ${cfg.host.kwriteconfig} --file "$shortcuts" --group services --group ft-screens-toggle.desktop --key _launch 'Meta+Shift+H'
        fi
      '';
    };
  };
}
