{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";

    slippi-desktop.url = "github:project-slippi/slippi-desktop-app";
    slippi-desktop.flake = false;
  };

  outputs = {
    nixpkgs,
    slippi-desktop,
    ...
  }: let
    supportedSystems = ["x86_64-linux"];
    forAllSystems = f:
      nixpkgs.lib.genAttrs supportedSystems (system:
        f system (import nixpkgs {inherit system;}));
  in {
    packages = forAllSystems (system: pkgs: let
      slippi-playback = pkgs.callPackage ./default.nix {
        inherit slippi-desktop;
        playbackSlippi = true;
      };
      slippi-netplay = pkgs.callPackage ./default.nix {
        inherit slippi-desktop;
        playbackSlippi = false;
      };
    in {
      default = slippi-netplay;
      netplay = slippi-netplay;
      playback = slippi-playback;
    });
  };
}
