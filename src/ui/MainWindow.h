#pragma once

#include "AppContext.h"

#include <QMainWindow>

class QAction;
class QDockWidget;
class QLabel;
class QTabWidget;

namespace nixm {

class BrandHeader;
class EditorPage;
class GitPage;
class HostsPage;
class LogPane;
class ModulesPage;
class OptionsPage;
class PackagesPage;
class SystemPage;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString &path);
    /// Opens the last used configuration, or guesses a sensible default.
    void openInitialProject(const QString &fromCommandLine);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildActions();
    void buildUi();
    void refreshAll();
    void updateWindowTitle();
    void updateDirtyState();

    void chooseProject();
    bool saveAll();
    void reloadProject();
    void showSettings();
    void showAbout();
    void rememberRecent(const QString &path);
    void rebuildRecentMenu();
    void toggleTheme();
    void restyle();
    /// Reattaches the log pane, whatever state it was saved in.
    void dockLogPane();
    void resetLayout();

    AppContext m_ctx;

    BrandHeader *m_header = nullptr;
    QTabWidget *m_tabs = nullptr;
    HostsPage *m_hosts = nullptr;
    ModulesPage *m_modules = nullptr;
    PackagesPage *m_packages = nullptr;
    OptionsPage *m_options = nullptr;
    GitPage *m_git = nullptr;
    EditorPage *m_editor = nullptr;
    SystemPage *m_system = nullptr;

    QDockWidget *m_logDock = nullptr;
    LogPane *m_log = nullptr;

    QAction *m_saveAction = nullptr;
    QAction *m_reloadAction = nullptr;
    QMenu *m_recentMenu = nullptr;
    QLabel *m_statusPath = nullptr;
    QLabel *m_statusDirty = nullptr;
};

} // namespace nixm
