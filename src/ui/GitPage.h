#pragma once

#include "AppContext.h"
#include "core/GitRepo.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace nixm {

class NixHighlighter;

/// Version control for the configuration tree: what changed, staging, commits,
/// history and syncing with a remote.
///
/// Every mutating command goes through the shared CommandRunner, so it shows up
/// in the log pane exactly as typed.
class GitPage : public QWidget
{
    Q_OBJECT

public:
    explicit GitPage(const AppContext &ctx, QWidget *parent = nullptr);

    const GitStatus &lastStatus() const { return m_status; }

public slots:
    void refresh();
    void applyTheme();

signals:
    void openFileRequested(const QString &absPath);
    void statusMessage(const QString &text);
    /// Emitted after a refresh so the header can show the branch.
    void repositoryStateChanged(const GitStatus &status);
    /// Asks MainWindow to flush dirty buffers before staging or committing.
    void saveRequested();

private:
    void buildUi();
    QWidget *buildSummary();
    QWidget *buildChanges();
    QWidget *buildCommitBox();
    QWidget *buildHistory();
    QWidget *buildSetupPage();

    void reloadChanges();
    void reloadHistory();
    void showDiffFor(QTreeWidgetItem *item);
    QStringList selectedPaths() const;
    QStringList checkedPaths() const;

    void runSteps(const QVector<CommandRunner::Step> &steps, const QString &done);
    void stageSelected();
    void unstageSelected();
    void discardSelected();
    void commitNow();
    void initRepository();
    void askIdentity();

    AppContext m_ctx;
    GitStatus m_status;
    bool m_updating = false;
    QString m_pendingMessage;

    QStackedWidget *m_stack = nullptr;

    QLabel *m_branchLabel = nullptr;
    QLabel *m_remoteLabel = nullptr;
    QLabel *m_warning = nullptr;
    QPushButton *m_fetchButton = nullptr;
    QPushButton *m_pullButton = nullptr;
    QPushButton *m_pushButton = nullptr;

    QTreeWidget *m_changes = nullptr;
    QPlainTextEdit *m_diff = nullptr;
    QPushButton *m_stageButton = nullptr;
    QPushButton *m_unstageButton = nullptr;
    QPushButton *m_discardButton = nullptr;

    QPlainTextEdit *m_message = nullptr;
    QCheckBox *m_amend = nullptr;
    QCheckBox *m_stageAll = nullptr;
    QPushButton *m_commitButton = nullptr;

    QTableWidget *m_history = nullptr;

    QLabel *m_setupText = nullptr;
    QPushButton *m_initButton = nullptr;
};

} // namespace nixm
