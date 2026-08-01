{
  description = "nixos-manager — a Qt front end for browsing, editing and rebuilding NixOS configurations";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f {
            inherit system;
            pkgs = nixpkgs.legacyPackages.${system};
          }
        );
    in
    {
      # ── Package ──────────────────────────────────────────────────────────────
      packages = forAllSystems (
        { pkgs, ... }:
        let
          nixos-manager = pkgs.callPackage ./nix/package.nix { };
        in
        {
          inherit nixos-manager;
          default = nixos-manager;
        }
      );

      # Drop this into `nixpkgs.overlays` to get `pkgs.nixos-manager` everywhere.
      overlays.default = final: _prev: {
        nixos-manager = final.callPackage ./nix/package.nix { };
      };

      # ── NixOS module ─────────────────────────────────────────────────────────
      nixosModules.nixos-manager = import ./nix/module.nix self;
      nixosModules.default = self.nixosModules.nixos-manager;

      # ── nix run ──────────────────────────────────────────────────────────────
      apps = forAllSystems (
        { system, ... }:
        let
          app = {
            type = "app";
            program = "${self.packages.${system}.nixos-manager}/bin/nixos-manager";
          };
        in
        {
          nixos-manager = app;
          default = app;
        }
      );

      # ── Development ──────────────────────────────────────────────────────────
      devShells = forAllSystems (
        { pkgs, ... }:
        {
          default = pkgs.mkShell {
            name = "nixos-manager-dev";
            inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.nixos-manager ];
            packages = with pkgs; [
              clang-tools
              cmake
              gdb
              ninja
              qt6.qttools
            ];
            shellHook = ''
              echo "nixos-manager dev shell"
              echo "  cmake -S . -B build -G Ninja && cmake --build build && ./build/nixos-manager"
            '';
          };
        }
      );

      formatter = forAllSystems ({ pkgs, ... }: pkgs.nixfmt-rfc-style);
    };
}
