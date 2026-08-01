#pragma once

#include "AppContext.h"
#include "core/SystemOps.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace nixm {

/// Everything that touches the running system: rebuilds, generations, flake
/// inputs and cleanup. Each button assembles a list of commands and hands it to
/// the shared CommandRunner, so the log pane always shows what actually ran.
class SystemPage : public QWidget
{
    Q_OBJECT

public:
    explicit SystemPage(const AppContext &ctx, QWidget *parent = nullptr);

    void startRebuildFor(const QString &host);

public slots:
    void refresh();
    void applyTheme();
    void refreshGenerations();
    void refreshFlakeInputs();

signals:
    /// Asks MainWindow to flush dirty buffers before a rebuild.
    void saveRequested();
    void statusMessage(const QString &text);

private:
    QWidget *buildRebuildGroup();
    QWidget *buildGenerationsGroup();
    QWidget *buildFlakeGroup();
    QWidget *buildCleanupGroup();

    void runRebuild();
    void runRollback();
    void runDeleteGenerations();
    void runFlakeUpdate(bool selectedOnly);
    void runCleanup();
    void setControlsEnabled(bool enabled);
    QVector<int> selectedGenerations() const;

    AppContext m_ctx;

    QComboBox *m_hostBox = nullptr;
    QComboBox *m_actionBox = nullptr;
    QLineEdit *m_extraArgs = nullptr;
    QCheckBox *m_saveFirst = nullptr;
    QPushButton *m_rebuildButton = nullptr;

    QTableWidget *m_generations = nullptr;
    QPushButton *m_rollbackButton = nullptr;
    QPushButton *m_deleteGenButton = nullptr;

    QTableWidget *m_inputs = nullptr;
    QCheckBox *m_flakeAsRoot = nullptr;
    QPushButton *m_updateAll = nullptr;
    QPushButton *m_updateSelected = nullptr;

    QCheckBox *m_cleanSystemGenerations = nullptr;
    QCheckBox *m_cleanUserProfiles = nullptr;
    QCheckBox *m_cleanCollectGarbage = nullptr;
    QCheckBox *m_cleanOptimise = nullptr;
    QCheckBox *m_cleanDryRun = nullptr;
    QSpinBox *m_cleanDays = nullptr;
    QPushButton *m_cleanupButton = nullptr;
    QLabel *m_storeSize = nullptr;
};

} // namespace nixm
