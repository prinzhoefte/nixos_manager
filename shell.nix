# Development shell for nixos-manager.
#
#   nix-shell                    # this file, uses your <nixpkgs> channel
#   nix-shell --arg withFonts false
#   nix develop                  # the flake's devShell (identical tooling)
#
# Then:
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ./build/nixos-manager /etc/nixos
{
  pkgs ? import <nixpkgs> { },
  # Pull in Manrope, the JR-IT brand typeface. It is only useful once it is
  # visible to fontconfig, which needs a system-wide install — see the hint
  # printed by the shell hook.
  withFonts ? true,
}:

let
  manrope = pkgs.google-fonts.override { fonts = [ "Manrope" ]; };
in
pkgs.mkShell {
  name = "nixos-manager-dev";

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config

    # Qt 6: qtbase carries Widgets and Network, which is all the app needs.
    qt6.qtbase
    qt6.wrapQtAppsHook
    qt6.qttools

    # Tooling
    clang-tools # clangd, clang-format
    gdb
  ];

  buildInputs =
    (with pkgs; [
      qt6.qtbase
      qt6.qtwayland # native Wayland session rather than XWayland
    ])
    ++ pkgs.lib.optional withFonts manrope;

  # wrapQtAppsHook only wraps installed binaries; in a dev shell we run
  # ./build/nixos-manager directly, so point Qt at its plugins by hand.
  shellHook = ''
    export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}''${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
    export QML2_IMPORT_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtQmlPrefix}''${QML2_IMPORT_PATH:+:$QML2_IMPORT_PATH}"

    echo "nixos-manager dev shell — Qt ${pkgs.qt6.qtbase.version}, CMake ${pkgs.cmake.version}"
    echo
    echo "  cmake -S . -B build -G Ninja && cmake --build build"
    echo "  ./build/nixos-manager /etc/nixos"
    echo "  ctest --test-dir build --output-on-failure"
    echo
    if ! fc-list 2>/dev/null | grep -qi manrope; then
      echo "  note: Manrope is not registered with fontconfig, so the UI will fall"
      echo "        back to your system sans. To install it system-wide, add to"
      echo "        your configuration:"
      echo "          fonts.packages = [ (pkgs.google-fonts.override { fonts = [ \"Manrope\" ]; }) ];"
      echo "        or just enable programs.nixos-manager, which does it for you."
      echo
    fi
  '';
}
