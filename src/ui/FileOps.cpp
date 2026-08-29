#include "FileOps.h"

#include "Theme.h"
#include "core/CommandRunner.h"
#include "core/ConfigProject.h"
#include "core/SystemOps.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace nixm {
namespace {

/// "3 files", "1 directory and 2 files" — the phrase the dialog leads with.
/// Written out rather than left to %n, because the app ships no translations
/// and "1 file(s)" reads like a bug report.
QString subject(const QStringList &paths)
{
    int dirs = 0;
    for (const QString &p : paths)
        if (QFileInfo(p).isDir())
            ++dirs;
    const int files = int(paths.size()) - dirs;

    const QString fileText
        = files == 1 ? FileOps::tr("1 file") : FileOps::tr("%1 files").arg(files);
    const QString dirText
        = dirs == 1 ? FileOps::tr("1 directory") : FileOps::tr("%1 directories").arg(dirs);

    if (dirs == 0)
        return fileText;
    if (files == 0)
        return dirText;
    return FileOps::tr("%1 and %2").arg(dirText, fileText);
}

QString escape(const QString &s)
{
    return s.toHtmlEscaped();
}

/// The body of the dialog: what is about to disappear, and what still points
/// at it. Written as rich text because it is a report, not a form.
QString summaryHtml(ConfigProject *project, const QStringList &paths,
    const QVector<FileReference> &refs, const QString &extraNote)
{
    const QDir root(project->root());
    const ThemeColors &tc = Theme::colors();
    QString html;

    html += QStringLiteral("<p><b>%1</b></p><ul>").arg(FileOps::tr("Will be removed"));
    for (const QString &p : paths) {
        const QFileInfo info(p);
        QString line = QStringLiteral("<li><code>%1</code>").arg(escape(root.relativeFilePath(p)));
        if (info.isDir()) {
            const int count = int(project->nixFilesUnder(p).size());
            line += QStringLiteral(" <small>— %1</small>")
                        .arg(count == 1 ? FileOps::tr("directory, 1 Nix file inside")
                                        : FileOps::tr("directory, %1 Nix files inside")
                                              .arg(count));
        }
        const QString role = project->describeRole(p);
        if (!role.isEmpty()) {
            line += QStringLiteral("<br><span style='color:%1;'>⚠ %2</span>")
                        .arg(tc.amber.name(), escape(role));
        }
        html += line + QStringLiteral("</li>");
    }
    html += QStringLiteral("</ul>");

    if (refs.isEmpty()) {
        html += QStringLiteral("<p>%1</p>")
                    .arg(FileOps::tr("Nothing else in the tree refers to it."));
    } else {
        html += QStringLiteral("<p><b>%1</b></p><ul>")
                    .arg(FileOps::tr("Referred to from"));
        for (const FileReference &r : refs) {
            QString note;
            if (!r.rewritable()) {
                note = QStringLiteral(" <span style='color:%1;'>⚠ %2</span>")
                           .arg(tc.amber.name(),
                               FileOps::tr("in <code>%1</code> — fix this one by hand")
                                   .arg(escape(r.owner)));
            } else if (!r.enabled) {
                note = QStringLiteral(" <i>%1</i>").arg(FileOps::tr("(already commented out)"));
            }
            html += QStringLiteral("<li><code>%1</code> <small>(%2)</small> — <code>%3</code>%4</li>")
                        .arg(escape(r.relFile), FileOps::tr("line %1").arg(r.line + 1),
                            escape(r.literal), note);
        }
        html += QStringLiteral("</ul>");
    }

    if (!extraNote.isEmpty())
        html += QStringLiteral("<p>%1</p>").arg(escape(extraNote));

    return html;
}

} // namespace

FileOps::DeleteOutcome FileOps::deletePaths(QWidget *parent, const AppContext &ctx,
    const QStringList &paths, const QString &extraNote)
{
    DeleteOutcome outcome;
    ConfigProject *project = ctx.project;
    if (!project || !project->isOpen() || paths.isEmpty())
        return outcome;

    // Drop anything that is not ours to delete before the user is asked about it.
    QStringList targets;
    QStringList refused;
    for (const QString &raw : paths) {
        const QString abs = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
        if (abs.isEmpty() || targets.contains(abs))
            continue;
        if (!project->containsPath(abs) || !QFileInfo::exists(abs))
            refused << abs;
        else
            targets << abs;
    }
    if (targets.isEmpty()) {
        QMessageBox::warning(parent, tr("Delete"),
            refused.isEmpty()
                ? tr("There is nothing to delete.")
                : tr("These paths are not part of %1:\n\n%2")
                      .arg(project->root(), refused.join(QLatin1Char('\n'))));
        return outcome;
    }

    QVector<FileReference> refs;
    for (const QString &t : std::as_const(targets))
        refs += project->referencesTo(t);

    // ── Dialog ───────────────────────────────────────────────────────────────
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Delete from the configuration"));
    dialog.setMinimumWidth(560);
    auto *layout = new QVBoxLayout(&dialog);

    auto *heading = new QLabel(tr("Delete %1 from %2?")
                                   .arg(subject(targets), project->root()),
        &dialog);
    heading->setWordWrap(true);
    layout->addWidget(heading);

    auto *details = new QTextBrowser(&dialog);
    details->document()->setDefaultStyleSheet(Theme::richTextCss());
    details->setHtml(summaryHtml(project, targets, refs, extraNote));
    details->setMinimumHeight(180);
    layout->addWidget(details, 1);

    auto *form = new QFormLayout;

    auto *action = new QComboBox(&dialog);
    action->addItem(tr("Remove the import lines"), int(ReferenceAction::Remove));
    action->addItem(tr("Comment the import lines out"), int(ReferenceAction::Disable));
    action->addItem(tr("Leave them alone"), int(ReferenceAction::Leave));
    action->setEnabled(!refs.isEmpty());
    if (refs.isEmpty())
        action->setCurrentIndex(action->findData(int(ReferenceAction::Leave)));
    form->addRow(tr("Imports pointing at it:"), action);
    layout->addLayout(form);

    auto *toTrash = new QCheckBox(tr("Move to the trash instead of deleting for good"), &dialog);
    toTrash->setChecked(true);
    toTrash->setToolTip(tr("Falls back to a plain delete when the tree is on a filesystem "
                           "without a trash, or is not owned by you."));
    layout->addWidget(toTrash);

    auto *saveNow = new QCheckBox(tr("Save the configuration files straight away"), &dialog);
    saveNow->setChecked(true);
    saveNow->setToolTip(tr("The files disappear immediately, so leaving the edits unsaved would "
                           "mean the configuration on disk still imports something that is gone."));
    layout->addWidget(saveNow);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton *confirm = buttons->addButton(tr("Delete"), QDialogButtonBox::AcceptRole);
    confirm->setIcon(Theme::icon(QStringLiteral("trash")));
    confirm->setProperty("danger", true);
    // Deleting is the dangerous half of this dialog, so Return must not do it.
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return outcome;

    // ── Carry it out ─────────────────────────────────────────────────────────
    DeleteRequest request;
    request.paths = targets;
    request.toTrash = toTrash->isChecked();
    request.references = refs.isEmpty()
        ? ReferenceAction::Leave
        : ReferenceAction(action->currentData().toInt());

    DeleteResult result;
    outcome.changed = project->deleteFiles(request, &result);
    outcome.deleted = result.deleted;
    outcome.needsSave = outcome.changed && saveNow->isChecked();

    // ── Paths a plain unlink was refused for ─────────────────────────────────
    if (!result.needsPrivilege.isEmpty()) {
        QVector<CommandRunner::Step> steps;
        QStringList impossible;
        for (const QString &path : std::as_const(result.needsPrivilege)) {
            const CommandRunner::Step step = SystemOps::removePathAsRoot(path);
            if (step.program.isEmpty())
                impossible << path;
            else
                steps << step;
        }
        if (!impossible.isEmpty()) {
            result.errors << tr("Refusing to remove %1 as root: it is not a nested path.")
                                 .arg(impossible.join(QStringLiteral(", ")));
        }

        if (!steps.isEmpty()) {
            const auto answer = QMessageBox::question(parent, tr("Elevated removal required"),
                tr("These paths are not writable by your user:\n\n%1\n\nRemove them as root?")
                    .arg(result.needsPrivilege.join(QLatin1Char('\n'))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes) {
                const QStringList pending = result.needsPrivilege;
                // The buffers can only be dropped once `rm` has actually run.
                QObject::connect(
                    ctx.runner, &CommandRunner::batchFinished, project,
                    [project, pending](bool) {
                        for (const QString &path : pending)
                            if (!QFileInfo::exists(path))
                                project->forgetFile(path);
                        project->rescan();
                    },
                    Qt::SingleShotConnection);
                if (ctx.runner->run(steps)) {
                    outcome.changed = true;
                } else {
                    result.errors << tr("Another command is still running; "
                                        "try again once it has finished.");
                }
            }
        }
    }

    if (!result.leftBehind.isEmpty()) {
        QMessageBox::information(parent, tr("Delete"),
            tr("These references are not plain import entries, so they were left as they "
               "are:\n\n%1\n\nEdit them by hand in the Editor tab.")
                .arg(result.leftBehind.join(QLatin1Char('\n'))));
    }

    if (!result.errors.isEmpty()) {
        QMessageBox::warning(parent, tr("Delete"),
            tr("Not everything went through:\n\n%1").arg(result.errors.join(QLatin1Char('\n'))));
    }

    // ── Status line ──────────────────────────────────────────────────────────
    if (!result.deleted.isEmpty()) {
        const QDir root(project->root());
        QStringList names;
        for (const QString &p : std::as_const(result.deleted))
            names << root.relativeFilePath(p);
        outcome.status = result.trashed.size() == result.deleted.size()
            ? tr("Moved to trash: %1").arg(names.join(QStringLiteral(", ")))
            : tr("Deleted %1").arg(names.join(QStringLiteral(", ")));
        const int edited = int(result.editedFiles.size());
        if (edited > 0) {
            outcome.status += QStringLiteral(" — ")
                + (edited == 1 ? tr("1 file updated") : tr("%1 files updated").arg(edited));
        }
    } else if (!result.needsPrivilege.isEmpty()) {
        outcome.status = tr("Removing %1 as root…")
                             .arg(result.needsPrivilege.join(QStringLiteral(", ")));
    }

    return outcome;
}

void FileOps::whenIdle(const AppContext &ctx, QObject *context,
    const std::function<void()> &action)
{
    if (!ctx.runner || !ctx.runner->isRunning()) {
        action();
        return;
    }
    QObject::connect(
        ctx.runner, &CommandRunner::batchFinished, context, [action](bool) { action(); },
        Qt::SingleShotConnection);
}

} // namespace nixm
