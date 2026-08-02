# nixos-manager

A Qt 6 desktop front end for NixOS configurations. It reads a flake with several
`nixosConfigurations` — or a plain `configuration.nix` — and lets you toggle
modules per host, search nixpkgs and add packages, then rebuild, roll back or
garbage-collect the system, without leaving the app.

Edits are written back as **minimal in-place changes**. Your indentation,
alignment and `# ── Section ──` comments survive; a diff after adding a package
is one line.

```diff
         google-chrome
         vlc
         libreoffice
+        firefox
     ];
```

## What it does

**Hosts** — pick a host and see its identity (`networking.hostName`,
`system.stateVersion`), every module in the tree with a tick box, and the
feature options your own modules declare with `lib.mkOption`.

Unticking a module **comments its import out** rather than deleting the line, so
nothing is lost and the change reads clearly in `git diff`:

```diff
-        ../../modules/common/networking.nix
+        #../../modules/common/networking.nix
```

Ticking a module that is not imported yet adds it, and it lands in the right
`# ── Section ──` block for its category rather than at the bottom of the list.

Modules the flake adds to *every* host (a shared `modules = [ … ]` inside a
`mkHost` helper, for instance) are shown as such and are not togglable per host.

**New host…** creates `hosts/<name>/default.nix`, optionally copying another
host's imports (section comments and commented-out lines included), writes a
clearly-marked `hardware-configuration.nix` placeholder, and registers the host
in `flake.nix` — matching whatever the neighbouring entries do, right down to
their `=` alignment:

```diff
             wladi-server        = mkHost ./hosts/wladi-server;
+            laptop              = mkHost ./hosts/laptop;
```

**Modules** — browse everything under `modules/`, grouped by category: which
hosts use each one, the packages it installs, the settings it makes and the
options it declares. Creates new modules from a few templates.

**Packages** — a package manager UI. Searches the same index as
search.nixos.org, so results are instant and carry versions, descriptions,
homepages, licences and the binaries each package provides. Pick any package
list in the tree as the destination and click *Add package*; the attribute is
written with the right prefix for that list's `with` expression. The right-hand
pane lists every package already in the configuration, grouped by file, and can
remove or comment out individual entries.

Results are cached on disk, so repeat searches work offline.

**Options** — add, edit and remove NixOS options. The left half searches the
NixOS option index — the same data as the *Options* tab on search.nixos.org —
so you can check an option's type, default, example and declaring module before
setting it. Pick the file to write to, give a value, and it is inserted into
that file. The right half lists every option your configuration actually sets,
grouped by file: double-click a value to edit it in place, or remove the binding
entirely. Options your own modules declare with `lib.mkOption` are filtered out
of the list, since those are the module's API rather than a setting.

Values are passed through as-is when they already look like Nix
(`[ "wheel" ]`, `pkgs.foo`, `lib.mkForce 1`), and quoted when they are plainly
a bare string.

**Editor** — a syntax-highlighted Nix editor over the very same buffers the
other pages edit. Anything the app does not model can still be changed by hand,
and the structured views pick the change up immediately.

**System** — `nixos-rebuild switch/boot/test/dry-activate/dry-build/build` with
live streaming output and a cancel button; the system generation list with
rollback and deletion; flake inputs with their locked revisions and per-input
updates; and a cleanup panel that does what `nix-env --delete-generations old &&
nix-collect-garbage -d` does, with a dry-run mode and an optional store
optimise pass.

Every privileged command is shown in full before it runs, and escalation goes
through `pkexec` (or `sudo -n`, configurable in *Tools ▸ Settings*).

## Look and feel

The interface follows the **JR-IT Services** corporate design: Manrope, the
blue scale from ink `#042C53` through primary `#0C447C` to accent `#378ADD`,
and amber `#F4A93C` used only where something wants your attention — the
unsaved-changes marker, a module that is present but commented out, and the
currently active system generation. Roughly the 70 / 20 / 10 blue / neutral /
accent split the guide asks for.

Both a light and a dark variant ship; the app follows your desktop on first
start and *View ▸ Toggle light / dark theme* (`Ctrl+Shift+T`) switches, with the
choice remembered.

Everything is drawn at runtime with `QPainter` — the logo, the tab and button
icons, the check marks, the chevrons — so the app needs neither an installed
icon theme nor Qt's SVG plugin, and every glyph picks up the current palette.

Manrope is not bundled. If it is not installed the app falls back to Inter,
Cantarell or your system sans and still looks consistent; the NixOS module
installs it for you (see `installBrandFont` below).

## Installing

### Try it without installing

```sh
nix run github:prinzhoefte/nixos_manager -- /etc/nixos
```

### Add it to an existing NixOS configuration

Add the flake as an input and enable the module:

```nix
{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    nixos-manager.url = "github:prinzhoefte/nixos_manager";
    # Optional: build against the same nixpkgs as the rest of your system.
    nixos-manager.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, nixos-manager, ... }@inputs: {
    nixosConfigurations.my-host = nixpkgs.lib.nixosSystem {
      specialArgs = { inherit inputs; };
      modules = [
        ./hosts/my-host
        nixos-manager.nixosModules.default
        {
          programs.nixos-manager = {
            enable = true;
            # Where the app opens by default. Point it at a checkout you own
            # rather than a root-owned /etc/nixos and it will never have to ask
            # for a password just to save a file.
            configPath = "/home/justin/nixos";
          };
        }
      ];
    };
  };
}
```

Module options:

| Option | Default | Meaning |
| --- | --- | --- |
| `programs.nixos-manager.enable` | `false` | Install the app. |
| `programs.nixos-manager.package` | this flake's build | Override the package. |
| `programs.nixos-manager.configPath` | `null` | Sets `NIXOS_MANAGER_CONFIG`. |
| `programs.nixos-manager.installPolkitAgent` | `true` | Enables `security.polkit`, which `pkexec` needs. |
| `programs.nixos-manager.installBrandFont` | `true` | Adds Manrope to `fonts.packages` (only that family, not all of google-fonts). |

### As an overlay

```nix
nixpkgs.overlays = [ nixos-manager.overlays.default ];
environment.systemPackages = [ pkgs.nixos-manager ];
```

## Running it

```
nixos-manager [<directory containing flake.nix or configuration.nix>]
```

Without an argument it opens, in order: `$NIXOS_MANAGER_CONFIG`, the last tree
you used, then `/etc/nixos`.

### A note on file permissions

If your configuration lives in `/etc/nixos` it is owned by root, and saving
needs elevation. The app notices, offers to write the files through `pkexec`,
and shows you the exact `install` commands it runs. Keeping the tree in your
home directory and pointing `nixos-rebuild --flake` at it avoids the prompt
entirely.

## Building from source

On NixOS, either entry point gives you the same toolchain:

```sh
nix-shell              # shell.nix, uses your <nixpkgs> channel
nix develop            # the flake devShell, uses the pinned nixpkgs
```

Both put Qt 6, CMake, Ninja, clangd and gdb on `PATH`, and point `QT_PLUGIN_PATH`
at Qt's plugins so `./build/nixos-manager` runs straight out of the build tree
without `wrapQtAppsHook`. Then:

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/nixos-manager /etc/nixos
```

`nix-shell --arg withFonts false` skips pulling in Manrope if you would rather
not build it.

Requirements: Qt 6.4 or newer (`QtWidgets`, `QtNetwork`), CMake 3.21, a C++17
compiler.

Run the test suite with:

```sh
ctest --test-dir build --output-on-failure
```

## How the editing works

The interesting part is `src/core`, which has no UI dependency:

| File | Responsibility |
| --- | --- |
| `NixLexer.{h,cpp}` | Tokenizes Nix. Handles `''…''` strings, `${…}` interpolation (including nested strings and braces), path literals versus the `/` and `//` operators, and both comment forms. Strings come out as single tokens, so scanning for brackets never trips over their contents. |
| `NixFile.{h,cpp}` | Walks the token stream keeping an attribute-path prefix stack, so `users.users.justin.description` is found whether it is written flat or nested. Recurses through `lib.mkIf` / `lib.mkMerge` wrappers and `let … in` preludes, records `lib.mkOption` declarations with their type, default and description, and captures the exact byte range of every import, list entry, attribute set and value. Mutations are range replacements on the original text. |
| `ConfigProject.{h,cpp}` | Discovers hosts from `nixosConfigurations`, resolves each one's entry file, catalogues modules by category, reads flake inputs and the nixpkgs channel, and owns the shared file buffers. |
| `PackageSearch.{h,cpp}` | Queries the search.nixos.org Elasticsearch index — packages and NixOS options — with an on-disk cache. Probes the index schema generation and remembers what answered, so an upstream bump does not break the app. |
| `CommandRunner.{h,cpp}` | Sequences external commands, merges their output, handles privilege escalation and cancellation. |
| `SystemOps.{h,cpp}` | Builds the argument lists for rebuilds, generations, flake updates and cleanup. Runs nothing itself. |

And on the UI side, `src/ui/Theme.{h,cpp}` holds the whole visual system: the
two palettes, the generated stylesheet, a `QProxyStyle` that paints check marks
and radio buttons in brand colours, and the runtime-painted logo and icon set.

Because mutation is range-based rather than print-based, the app never
reformats a file it did not need to touch.

## Configuration

| Environment variable | Purpose |
| --- | --- |
| `NIXOS_MANAGER_CONFIG` | Default configuration tree. |
| `NIXOS_MANAGER_SEARCH_URL` | Alternative package index endpoint (default `https://search.nixos.org/backend`). |
| `NIXOS_MANAGER_SEARCH_USER` / `_PASSWORD` | Credentials for that endpoint. |

Settings (theme, privilege helper, cache lifetime, recent trees, window layout)
live in `~/.config/nixos-manager/nixos-manager.conf`.

## Limitations

- The parser understands the shapes real NixOS configurations use, not the whole
  Nix language. Anything it cannot model is left alone and stays editable in the
  Editor tab — it is never silently rewritten.
- Package search shows what nixpkgs' index knows, which tracks the channel your
  flake pins but is not evaluated against your exact locked revision.
- Deploying to other machines over SSH is not implemented; rebuilds target the
  machine the app runs on.
- A new host's `hardware-configuration.nix` is a placeholder. Only
  `nixos-generate-config` on the target machine can produce the real one.

## Licence

MIT.
