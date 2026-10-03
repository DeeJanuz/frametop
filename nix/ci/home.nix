# A Home Manager configuration with programs.frametop on, for CI: building its activation
# package evaluates the module and builds everything it links, for this machine's system.
#   nix build --impure -f nix/ci/home.nix
# Impure: it reads this checkout as a flake, and takes home-manager's current master.
{
  system ? builtins.currentSystem,
  homeManager ? "github:nix-community/home-manager",
}:
let
  frametop = builtins.getFlake ("git+file://" + toString ../.. + "?shallow=1");
  home-manager = builtins.getFlake homeManager;
in
(home-manager.lib.homeManagerConfiguration {
  pkgs = frametop.inputs.nixpkgs.legacyPackages.${system};
  modules = [
    frametop.homeManagerModules.default
    {
      home.username = "steamos";
      home.homeDirectory = "/home/steamos";
      home.stateVersion = "25.11";
      targets.genericLinux.enable = true;
      programs.frametop.enable = true;
    }
  ];
}).activationPackage
