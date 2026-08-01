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

```sh
nix develop            # or install qt6 + cmake + ninja yourself
cmake -S . -B build -G Ninja
cmake --build build
./build/nixos-manager /etc/nixos
```

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
| `NixFile.{h,cpp}` | Walks the token stream keeping an attribute-path prefix stack, so `users.users.justin.description` is found whether it is written flat or nested. Recurses through `lib.mkIf` / `lib.mkMerge` wrappers, records `lib.mkOption` declarations with their type, default and description, and captures the exact byte range of every import, list entry and value. Mutations are range replacements on the original text. |
| `ConfigProject.{h,cpp}` | Discovers hosts from `nixosConfigurations`, resolves each one's entry file, catalogues modules by category, reads flake inputs and the nixpkgs channel, and owns the shared file buffers. |
| `PackageSearch.{h,cpp}` | Queries the search.nixos.org Elasticsearch index with an on-disk cache. Probes the index schema generation and remembers what answered, so an upstream bump does not break the app. |
| `CommandRunner.{h,cpp}` | Sequences external commands, merges their output, handles privilege escalation and cancellation. |
| `SystemOps.{h,cpp}` | Builds the argument lists for rebuilds, generations, flake updates and cleanup. Runs nothing itself. |

Because mutation is range-based rather than print-based, the app never
reformats a file it did not need to touch.

## Configuration

| Environment variable | Purpose |
| --- | --- |
| `NIXOS_MANAGER_CONFIG` | Default configuration tree. |
| `NIXOS_MANAGER_SEARCH_URL` | Alternative package index endpoint (default `https://search.nixos.org/backend`). |
| `NIXOS_MANAGER_SEARCH_USER` / `_PASSWORD` | Credentials for that endpoint. |

Settings (privilege helper, cache lifetime, recent trees, window layout) live in
`~/.config/nixos-manager/nixos-manager.conf`.

## Limitations

- The parser understands the shapes real NixOS configurations use, not the whole
  Nix language. Anything it cannot model is left alone and stays editable in the
  Editor tab — it is never silently rewritten.
- Package search shows what nixpkgs' index knows, which tracks the channel your
  flake pins but is not evaluated against your exact locked revision.
- Deploying to other machines over SSH is not implemented; rebuilds target the
  machine the app runs on.

## Licence

MIT.
