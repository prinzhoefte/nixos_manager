#pragma once

#include "AppContext.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;
class QWidget;

namespace nixm {

/// Deleting files out of the configuration tree, with the confirmation step
/// that goes with it. Kept in one place because the Modules, Hosts and Editor
/// pages all offer the same operation on different selections.
class FileOps
{
    Q_DECLARE_TR_FUNCTIONS(nixm::FileOps)

public:
    struct DeleteOutcome {
        /// Something on disk or in a buffer actually changed.
        bool changed = false;
        /// Buffers were rewritten and the caller should ask for a save.
        bool needsSave = false;
        /// One line for the status bar; empty when the user cancelled.
        QString status;
        /// Paths that are gone from disk.
        QStringList deleted;
    };

    /// Asks what should happen to `paths` and their imports, then carries it
    /// out. `extraNote` is shown in the dialog for consequences the caller
    /// handles itself, such as unregistering a host from `flake.nix`.
    static DeleteOutcome deletePaths(QWidget *parent, const AppContext &ctx,
        const QStringList &paths, const QString &extraNote = QString());

    /// Runs `action` as soon as the CommandRunner is free, or straight away
    /// when it already is. A deletion on a root-owned tree leaves an elevated
    /// `rm` running, and a save queued behind it would be refused.
    static void whenIdle(const AppContext &ctx, QObject *context,
        const std::function<void()> &action);
};

} // namespace nixm
