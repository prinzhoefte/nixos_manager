#pragma once

#include "CommandRunner.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace nixm {

/// One path reported by `git status`.
struct GitFileChange {
    QString path;             // repo-relative
    QString originalPath;     // set for renames
    QChar indexStatus = QLatin1Char('.');      // staged side
    QChar worktreeStatus = QLatin1Char('.');   // unstaged side
    bool untracked = false;
    bool ignored = false;
    bool conflicted = false;

    bool isStaged() const;
    bool isUnstaged() const;
    /// "modified", "added", "deleted", "untracked", …
    QString describe() const;
};

struct GitCommit {
    QString hash;
    QString shortHash;
    QString author;
    QString date;
    QString subject;
};

struct GitStatus {
    bool isRepository = false;
    QString root;             // repository top level, may be above the config dir
    QString branch;
    QString upstream;
    QString remoteUrl;
    int ahead = 0;
    int behind = 0;
    bool detached = false;
    bool hasCommits = false;
    bool hasIdentity = false; // user.name and user.email are both set
    QVector<GitFileChange> changes;
    QString error;            // non-empty when git refused to answer

    int stagedCount() const;
    int unstagedCount() const;
};

/// Version control for the configuration tree.
///
/// Read-only queries run synchronously and return parsed results; everything
/// that changes the repository is handed back as a CommandRunner::Step so it
/// goes through the same log pane and privilege handling as nixos-rebuild.
namespace GitRepo {

bool available();

GitStatus status(const QString &dir);
/// Unified diff for one path, or for everything when `path` is empty.
QString diff(const QString &dir, const QString &path, bool staged);
QVector<GitCommit> log(const QString &dir, int limit = 25);
/// Contents of `.gitignore` at the repository root, empty when there is none.
QString readGitignore(const QString &dir);

CommandRunner::Step init(const QString &dir);
CommandRunner::Step stage(const QString &dir, const QStringList &paths);
CommandRunner::Step unstage(const QString &dir, const QStringList &paths);
CommandRunner::Step commit(const QString &dir, const QString &message, bool amend);
CommandRunner::Step push(const QString &dir, const QString &branch, bool setUpstream);
CommandRunner::Step pull(const QString &dir);
CommandRunner::Step fetch(const QString &dir);
CommandRunner::Step discard(const QString &dir, const QStringList &paths);
/// Two commands: user.name and user.email, written to this repository only.
QVector<CommandRunner::Step> setIdentity(const QString &dir, const QString &name,
    const QString &email);
/// `git config --global --add safe.directory <dir>`, for the "dubious
/// ownership" refusal you get on a root-owned tree.
CommandRunner::Step markSafeDirectory(const QString &dir);

} // namespace GitRepo
} // namespace nixm
