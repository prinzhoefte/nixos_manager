{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  qt6,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "nixos-manager";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../src
      ../tests
      ../share
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    qt6.wrapQtAppsHook
  ];

  buildInputs = [
    qt6.qtbase
    # Renders the brand mark, which is shipped as artwork rather than drawn in
    # code.
    qt6.qtsvg
    # Without this the app falls back to XWayland on a Wayland session.
    qt6.qtwayland
  ];

  cmakeFlags = [
    (lib.cmakeBool "NIXOS_MANAGER_BUILD_TESTS" true)
    (lib.cmakeBool "NIXOS_MANAGER_BUILD_GUI" true)
  ];

  doCheck = true;
  nativeCheckInputs = [ qt6.qtbase ];
  checkPhase = ''
    runHook preCheck
    ctest --output-on-failure
    runHook postCheck
  '';

  meta = {
    description = "Qt front end for browsing, editing and rebuilding NixOS configurations";
    longDescription = ''
      nixos-manager reads a NixOS configuration — a flake with several
      nixosConfigurations, or a plain configuration.nix — and lets you toggle
      modules per host, search nixpkgs and add packages, then rebuild, roll back
      or garbage-collect the system. Edits are written back as minimal in-place
      changes so existing formatting and comments survive.
    '';
    homepage = "https://github.com/prinzhoefte/nixos_manager";
    license = lib.licenses.mit;
    mainProgram = "nixos-manager";
    platforms = lib.platforms.linux;
  };
})
