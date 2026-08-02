#pragma once

#include "CommandRunner.h"

#include <QString>
#include <QVector>

namespace nixm {

/// One entry of the NixOS system profile.
struct Generation {
    int number = 0;
    QString date;
    QString nixosVersion;
    QString kernel;
    bool current = false;
};

/// Builds the command sequences for every system-level action the UI offers.
/// Nothing here runs anything by itself; the caller hands the steps to a
/// CommandRunner, which keeps privilege handling and logging in one place.
namespace SystemOps {

constexpr const char *kSystemProfile = "/nix/var/nix/profiles/system";

enum class RebuildAction { Switch, Boot, Test, DryBuild, DryActivate, Build };

QString rebuildActionName(RebuildAction a);
QVector<RebuildAction> allRebuildActions();
/// True when the action changes the running or booted system.
bool rebuildNeedsRoot(RebuildAction a);

/// `nixos-rebuild <action> --flake <root>#<host>` (or without --flake for a
/// plain /etc/nixos tree).
CommandRunner::Step rebuild(RebuildAction action, const QString &root, const QString &host,
    bool isFlake, const QStringList &extraArgs = {});

/// Reads the system profile. Safe to call as a normal user.
///
/// The profile directory is read directly rather than shelling out to
/// `nix-env`, so this works with no `nix` on PATH, in any locale, and from a
/// desktop launcher with a minimal environment. `nix-env` is still tried as a
/// fallback for unusual layouts. When nothing is found, `diagnostic` explains
/// why instead of leaving the UI to guess.
QVector<Generation> listGenerations(QString *diagnostic = nullptr);

QVector<CommandRunner::Step> rollbackTo(int generation);
CommandRunner::Step deleteGenerations(const QVector<int> &generations);

/// Flake input handling.
CommandRunner::Step flakeUpdateAll(const QString &root, bool asRoot);
CommandRunner::Step flakeUpdateInput(const QString &root, const QString &input, bool asRoot);
/// Locked revisions from flake.lock / `nix flake metadata`, keyed by input name.
QHash<QString, QPair<QString, QString>> lockedInputs(const QString &root);

/// Cleanup, mirroring the usual `nix-collect-garbage -d` routine.
struct CleanupOptions {
    bool deleteSystemGenerations = true;
    int olderThanDays = 0;         ///< 0 means "all but the current generation"
    bool userProfiles = true;
    bool collectGarbage = true;
    bool optimiseStore = false;
    bool dryRun = false;
};
QVector<CommandRunner::Step> cleanup(const CleanupOptions &opts);

/// Best-effort disk usage of /nix/store, as a human readable string.
QString storeSize();

/// True when `nix` understands `nix flake update <input>` (Nix >= 2.19).
bool nixSupportsFlakeUpdateInput();

/// Installs a file with root privileges, used when the config tree is
/// root-owned and a plain write was refused.
CommandRunner::Step installFileAsRoot(const QString &tempPath, const QString &destination);

} // namespace SystemOps
} // namespace nixm
