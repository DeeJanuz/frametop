{
  description = "Frametop: a multi-screen Plasma desktop in VR on the Steam Frame (Nix packages and Home Manager module)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      # The Frame is aarch64-linux; x86_64-linux builds and evaluates the same derivations
      # for CI and for checking changes on a PC.
      systems = [
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAllSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The repo's sources, without the Nix files (editing those rebuilds nothing) and the
      # parts not packaged yet (gaze, hand tracking, Bluetooth fixes). ft-screens and
      # ft-pointer include the hand tracker's headers, so those stay.
      src = lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.difference ./. (
          lib.fileset.unions [
            ./flake.nix
            (lib.fileset.maybeMissing ./flake.lock)
            ./nix
            ./gaze
            (lib.fileset.difference ./hands ./hands/include)
            ./setup
          ]
        );
      };

      mkPackages =
        pkgs:
        let
          vrclientDeps = import ./nix/packages/vrclient-deps.nix { inherit (pkgs) libGL libuuid; };
          callPackage = lib.callPackageWith (pkgs // packages // { inherit src vrclientDeps; });
          packages = {
            ft-screens = callPackage ./nix/packages/ft-screens.nix { };
            ft-pointer = callPackage ./nix/packages/ft-pointer.nix { };
            ft-powerd = callPackage ./nix/packages/ft-powerd.nix { };
            ft-pointer-driver = callPackage ./nix/packages/ft-pointer-driver.nix { };
            # Everything: scripts, Python tools, settings apps, and the programs above.
            frametop-apps = callPackage ./nix/packages/frametop.nix { };
            # The same without the settings apps (no Qt).
            frametop-scripts = callPackage ./nix/packages/frametop.nix { withSettingsApps = false; };
          };
        in
        packages;
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          packages = mkPackages pkgs;
        in
        packages
        // {
          frametop = packages.frametop-apps;
          default = packages.frametop-apps;
        }
      );

      homeManagerModules.default = import ./nix/hm-module.nix self;
      homeManagerModules.frametop = self.homeManagerModules.default;

      checks = forAllSystems (pkgs: {
        inherit (self.packages.${pkgs.stdenv.hostPlatform.system})
          ft-pointer-driver
          frametop-scripts
          ;
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);
    };
}
