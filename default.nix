{
  slippi-desktop,
  playbackSlippi,
  pkgs,
}: let
  netplay-desktop = pkgs.makeDesktopItem {
    name = "Slippi Online";
    exec = "slippi-netplay";
    comment = "Play Melee Online!";
    desktopName = "Slippi-Netplay";
    genericName = "Wii/GameCube Emulator";
    categories = ["Game" "Emulator"];
    startupNotify = false;
  };

  playback-desktop = pkgs.makeDesktopItem {
    name = "Slippi Playback";
    exec = "slippi-playback";
    comment = "Watch Your Slippi Replays";
    desktopName = "Slippi-Playback";
    genericName = "Wii/GameCube Emulator";
    categories = ["Game" "Emulator"];
    startupNotify = false;
  };
in
  pkgs.stdenv.mkDerivation rec {
    pname = "slippi-ishiiruka";
    version = "3.2.0";
    name = "${pname}-${version}-${
      if playbackSlippi
      then "playback"
      else "netplay"
    }";

    src = ./.;

    outputs = ["out"];
    makeFlags = ["VERSION=us" "-s" "VERBOSE=1"];
    hardeningDisable = ["format"];

    cmakeFlags =
      [
        "-DLINUX_LOCAL_DEV=true"
        "-DGTK3_GLIBCONFIG_INCLUDE_DIR=${pkgs.glib.out}/lib/glib-3.0/include"
        "-DGTK3_GLIBCONFIG_INCLUDE_DIR=${pkgs.glib-networking.out}/lib/glib-3.0/include"
        "-DGTK3_GDKCONFIG_INCLUDE_DIR=${pkgs.gtk3.out}/lib/gtk-3.0/include"
        "-DGTK3_INCLUDE_DIRS=${pkgs.gtk3.out}/lib/gtk-3.0"
        "-DENABLE_LTO=True"
        "-DGTK2_GLIBCONFIG_INCLUDE_DIR=${pkgs.glib.out}/lib/glib-2.0/include"
        "-DGTK2_GLIBCONFIG_INCLUDE_DIR=${pkgs.glib-networking.out}/lib/glib-2.0/include"
        "-DGTK2_GDKCONFIG_INCLUDE_DIR=${pkgs.gtk2.out}/lib/gtk-2.0/include"
        "-DGTK2_INCLUDE_DIRS=${pkgs.gtk2}/lib/gtk-2.0"
      ]
      ++ pkgs.lib.optional playbackSlippi "-DIS_PLAYBACK=true";

    postBuild = with pkgs.lib;
      optionalString playbackSlippi ''
        rm -rf ../Data/Sys/GameSettings
        cp -r "${slippi-desktop}/app/dolphin-dev/overwrite/Sys/GameSettings" ../Data/Sys
      ''
      + ''
        cp -r -n ../Data/Sys/ Binaries/
        cp -r Binaries/ $out
        mkdir -p $out/bin
      '';

    installPhase =
      if playbackSlippi
      then ''
        wrapProgram "$out/dolphin-emu" \
          --set "GDK_BACKEND" "x11" \
          --prefix GIO_EXTRA_MODULES : "${pkgs.glib-networking}/lib/gio/modules" \
          --prefix LD_LIBRARY_PATH : "${pkgs.vulkan-loader}/lib" \
          --prefix PATH : "${pkgs.xdg-utils}/bin"
        ln -s $out/dolphin-emu $out/bin/slippi-playback
        ln -s ${playback-desktop}/share/applications $out/share
      ''
      else ''
        wrapProgram "$out/dolphin-emu" \
          --set "GDK_BACKEND" "x11" \
          --prefix GIO_EXTRA_MODULES : "${pkgs.glib-networking}/lib/gio/modules" \
          --prefix LD_LIBRARY_PATH : "${pkgs.vulkan-loader}/lib" \
          --prefix PATH : "${pkgs.xdg-utils}/bin"
        ln -s $out/dolphin-emu $out/bin/slippi-netplay
        ln -s ${netplay-desktop}/share/applications $out/share
      '';

    nativeBuildInputs = with pkgs; [pkg-config cmake wrapGAppsHook];
    buildInputs = pkgs.callPackage ./deps.nix {};
  }
