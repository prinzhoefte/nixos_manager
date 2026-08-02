self:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.programs.nixos-manager;
  system = pkgs.stdenv.hostPlatform.system;
in
{
  options.programs.nixos-manager = {
    enable = lib.mkEnableOption "nixos-manager, a Qt UI for NixOS configurations";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${system}.nixos-manager;
      defaultText = lib.literalExpression "nixos-manager.packages.\${system}.nixos-manager";
      description = "The nixos-manager package to install.";
    };

    configPath = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "/home/alice/nixos";
      description = ''
        Configuration tree nixos-manager opens by default, exported as
        `NIXOS_MANAGER_CONFIG`. When null the app falls back to the last tree
        you opened, then to `/etc/nixos`.

        Point this at a checkout you own rather than at a root-owned
        `/etc/nixos` if you would rather not be prompted for a password on
        every save.
      '';
    };

    installPolkitAgent = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Ensure polkit is enabled. nixos-manager escalates through `pkexec`,
        which needs polkit plus an authentication agent — the big desktop
        environments ship one, minimal window managers usually do not.
      '';
    };

    logo = lib.mkOption {
      type = lib.types.nullOr (lib.types.either lib.types.path lib.types.str);
      default = null;
      example = lib.literalExpression "./assets/jr-it.png";
      description = ''
        Image used as the application mark, exported as `NIXOS_MANAGER_LOGO`.
        Either a PNG or an SVG. Leave null to use the artwork shipped with the
        package.
      '';
    };

    installBrandFont = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Install Manrope, the JR-IT brand typeface the interface is designed
        around. Without it the app falls back to Inter, Cantarell or whatever
        sans your system provides, which works but looks slightly off.

        Only the Manrope family is built, not the whole google-fonts set.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [ cfg.package ];

    security.polkit.enable = lib.mkIf cfg.installPolkitAgent true;

    fonts.packages = lib.mkIf cfg.installBrandFont [
      (pkgs.google-fonts.override { fonts = [ "Manrope" ]; })
    ];

    environment.sessionVariables =
      (lib.optionalAttrs (cfg.configPath != null) { NIXOS_MANAGER_CONFIG = cfg.configPath; })
      // (lib.optionalAttrs (cfg.logo != null) { NIXOS_MANAGER_LOGO = toString cfg.logo; });
  };
}
