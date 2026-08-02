#include "MainWindow.h"

#include "BrandHeader.h"
#include "EditorPage.h"
#include "GitPage.h"
#include "HostsPage.h"
#include "LogPane.h"
#include "ModulesPage.h"
#include "OptionsPage.h"
#include "PackagesPage.h"
#include "SystemPage.h"
#include "Theme.h"
#include "core/CommandRunner.h"
#include "core/ConfigProject.h"
#include "core/GitRepo.h"
#include "core/PackageSearch.h"
#include "core/SystemOps.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

namespace nixm {
namespace {

QString currentUserName()
{
    const QByteArray fromEnv = qgetenv("USER");
    if (!fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
    return QStringLiteral("root");
}

QStringList defaultProjectCandidates()
{
    QStringList out;
    const QByteArray fromEnv = qgetenv("NIXOS_MANAGER_CONFIG");
    if (!fromEnv.isEmpty())
        out << QString::fromLocal8Bit(fromEnv);
    out << QStringLiteral("/etc/nixos");
    return out;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_ctx.project = new ConfigProject(this);
    m_ctx.runner = new CommandRunner(this);
    m_ctx.search = new PackageSearch(this);

    QSettings settings;
    const QString helper
        = settings.value(QStringLiteral("system/escalationHelper")).toString();
    if (!helper.isEmpty())
        m_ctx.runner->setEscalationHelper(helper);

    // sudo asks for a password; give it somewhere to ask.
    m_ctx.runner->setPasswordPrompt([this](const QString &reason) {
        bool accepted = false;
        const QString password = QInputDialog::getText(this, tr("Authentication"),
            reason + QStringLiteral("\n\n") + tr("Password for %1:").arg(currentUserName()),
            QLineEdit::Password, QString(), &accepted);
        return accepted ? password : QString();
    });

    buildUi();
    buildActions();

    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());

    updateWindowTitle();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);

    m_hosts = new HostsPage(m_ctx, this);
    m_modules = new ModulesPage(m_ctx, this);
    m_packages = new PackagesPage(m_ctx, this);
    m_options = new OptionsPage(m_ctx, this);
    m_editor = new EditorPage(m_ctx, this);
    m_git = new GitPage(m_ctx, this);
    m_system = new SystemPage(m_ctx, this);

    m_tabs->addTab(m_hosts, Theme::icon(QStringLiteral("host")), tr("&Hosts"));
    m_tabs->addTab(m_modules, Theme::icon(QStringLiteral("module")), tr("&Modules"));
    m_tabs->addTab(m_packages, Theme::icon(QStringLiteral("package")), tr("&Packages"));
    m_tabs->addTab(m_options, Theme::icon(QStringLiteral("options")), tr("&Options"));
    m_tabs->addTab(m_editor, Theme::icon(QStringLiteral("editor")), tr("&Editor"));
    m_tabs->addTab(m_git, Theme::icon(QStringLiteral("branch")), tr("&Git"));
    m_tabs->addTab(m_system, Theme::icon(QStringLiteral("system")), tr("&System"));
    m_tabs->setIconSize(QSize(16, 16));

    m_header = new BrandHeader(this);
    connect(m_header, &BrandHeader::openRequested, this, &MainWindow::chooseProject);
    connect(m_header, &BrandHeader::saveRequested, this, [this] { saveAll(); });
    connect(m_header, &BrandHeader::reloadRequested, this, &MainWindow::reloadProject);
    connect(m_header, &BrandHeader::themeToggleRequested, this, &MainWindow::toggleTheme);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_header);

    auto *tabHost = new QWidget(central);
    auto *tabLayout = new QVBoxLayout(tabHost);
    tabLayout->setContentsMargins(12, 8, 12, 10);
    tabLayout->addWidget(m_tabs);
    centralLayout->addWidget(tabHost, 1);

    setCentralWidget(central);

    m_log = new LogPane(m_ctx.runner, this);
    m_logDock = new QDockWidget(tr("Command output"), this);
    m_logDock->setObjectName(QStringLiteral("logDock"));
    m_logDock->setWidget(m_log);
    m_logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    m_logDock->hide();

    m_statusPath = new QLabel(this);
    statusBar()->addWidget(m_statusPath, 1);
    m_statusDirty = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusDirty);

    // ── Cross-page wiring ────────────────────────────────────────────────────
    auto openInEditor = [this](const QString &path) {
        m_editor->openFile(path);
        m_tabs->setCurrentWidget(m_editor);
    };
    connect(m_hosts, &HostsPage::openFileRequested, this, openInEditor);
    connect(m_modules, &ModulesPage::openFileRequested, this, openInEditor);
    connect(m_packages, &PackagesPage::openFileRequested, this, openInEditor);
    connect(m_options, &OptionsPage::openFileRequested, this, openInEditor);
    connect(m_git, &GitPage::openFileRequested, this, openInEditor);

    auto onModified = [this] {
        updateDirtyState();
        m_editor->syncFromBuffer();
        m_modules->refresh();
        m_packages->refresh();
        m_options->refresh();
    };
    connect(m_hosts, &HostsPage::configModified, this, onModified);
    connect(m_modules, &ModulesPage::configModified, this, onModified);
    connect(m_packages, &PackagesPage::configModified, this, onModified);
    connect(m_options, &OptionsPage::configModified, this, onModified);
    connect(m_editor, &EditorPage::configModified, this, [this] {
        updateDirtyState();
        m_hosts->refresh();
        m_modules->refresh();
        m_packages->refresh();
        m_options->refresh();
    });

    auto showStatus = [this](const QString &text) { statusBar()->showMessage(text, 6000); };
    connect(m_hosts, &HostsPage::statusMessage, this, showStatus);
    connect(m_modules, &ModulesPage::statusMessage, this, showStatus);
    connect(m_packages, &PackagesPage::statusMessage, this, showStatus);
    connect(m_options, &OptionsPage::statusMessage, this, showStatus);
    connect(m_git, &GitPage::statusMessage, this, showStatus);
    connect(m_editor, &EditorPage::statusMessage, this, showStatus);
    connect(m_system, &SystemPage::statusMessage, this, showStatus);

    connect(m_hosts, &HostsPage::rebuildRequested, this, [this](const QString &host) {
        m_system->startRebuildFor(host);
        m_tabs->setCurrentWidget(m_system);
        m_logDock->show();
    });
    connect(m_system, &SystemPage::saveRequested, this, [this] { saveAll(); });
    connect(m_git, &GitPage::saveRequested, this, [this] { saveAll(); });
    connect(m_git, &GitPage::repositoryStateChanged, this, [this](const GitStatus &status) {
        m_header->setGitState(status.isRepository ? status.branch : QString(),
            int(status.changes.size()));
    });

    connect(m_ctx.runner, &CommandRunner::runningChanged, this, [this](bool running) {
        if (running)
            m_logDock->show();
    });
    connect(m_ctx.search, &PackageSearch::failed, this,
        [this](const QString &message) { statusBar()->showMessage(message, 10000); });
}

void MainWindow::buildActions()
{
    auto *fileMenu = menuBar()->addMenu(tr("&File"));

    auto *openAction = fileMenu->addAction(tr("&Open configuration…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::chooseProject);

    m_recentMenu = fileMenu->addMenu(tr("Open &recent"));

    m_saveAction = fileMenu->addAction(tr("&Save all changes"));
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setEnabled(false);
    connect(m_saveAction, &QAction::triggered, this, [this] { saveAll(); });

    m_reloadAction = fileMenu->addAction(tr("&Reload from disk"));
    m_reloadAction->setShortcut(QKeySequence::Refresh);
    connect(m_reloadAction, &QAction::triggered, this, &MainWindow::reloadProject);

    fileMenu->addSeparator();
    auto *quitAction = fileMenu->addAction(tr("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_logDock->toggleViewAction());
    viewMenu->addSeparator();
    auto *themeAction = viewMenu->addAction(tr("Toggle &light / dark theme"));
    themeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    connect(themeAction, &QAction::triggered, this, &MainWindow::toggleTheme);

    auto *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    auto *settingsAction = toolsMenu->addAction(tr("&Settings…"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);
    auto *clearCacheAction = toolsMenu->addAction(tr("Clear package search &cache"));
    connect(clearCacheAction, &QAction::triggered, this, [this] {
        m_ctx.search->clearCache();
        statusBar()->showMessage(tr("Package search cache cleared."), 5000);
    });

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto *aboutAction = helpMenu->addAction(tr("&About"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

    // The header carries the same three actions, so no separate tool bar.
    openAction->setIcon(Theme::icon(QStringLiteral("open")));
    m_saveAction->setIcon(Theme::icon(QStringLiteral("save")));
    m_reloadAction->setIcon(Theme::icon(QStringLiteral("reload")));

    rebuildRecentMenu();
}

void MainWindow::toggleTheme()
{
    const Theme::Mode next = Theme::isDark() ? Theme::Light : Theme::Dark;
    Theme::setMode(qApp, next);
    QSettings().setValue(QStringLiteral("appearance/theme"),
        next == Theme::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
    restyle();
}

void MainWindow::restyle()
{
    // Anything that bakes a colour into a pixmap or into rich text has to be
    // rebuilt when the palette changes.
    m_header->applyTheme();
    m_tabs->setTabIcon(0, Theme::icon(QStringLiteral("host")));
    m_tabs->setTabIcon(1, Theme::icon(QStringLiteral("module")));
    m_tabs->setTabIcon(2, Theme::icon(QStringLiteral("package")));
    m_tabs->setTabIcon(3, Theme::icon(QStringLiteral("options")));
    m_tabs->setTabIcon(4, Theme::icon(QStringLiteral("editor")));
    m_tabs->setTabIcon(5, Theme::icon(QStringLiteral("branch")));
    m_tabs->setTabIcon(6, Theme::icon(QStringLiteral("system")));
    QApplication::setWindowIcon(Theme::logo());

    m_hosts->applyTheme();
    m_modules->applyTheme();
    m_packages->applyTheme();
    m_options->applyTheme();
    m_editor->applyTheme();
    m_git->applyTheme();
    m_system->applyTheme();
    m_log->applyTheme();

    refreshAll();
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QStringList recent
        = QSettings().value(QStringLiteral("recentProjects")).toStringList();
    if (recent.isEmpty()) {
        m_recentMenu->addAction(tr("(none yet)"))->setEnabled(false);
        return;
    }
    for (const QString &path : recent) {
        QAction *action = m_recentMenu->addAction(path);
        connect(action, &QAction::triggered, this, [this, path] { openProject(path); });
    }
}

void MainWindow::rememberRecent(const QString &path)
{
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recentProjects")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 8)
        recent.removeLast();
    settings.setValue(QStringLiteral("recentProjects"), recent);
    settings.setValue(QStringLiteral("lastProject"), path);
    rebuildRecentMenu();
}

void MainWindow::openInitialProject(const QString &fromCommandLine)
{
    QStringList candidates;
    if (!fromCommandLine.isEmpty())
        candidates << fromCommandLine;
    const QString last = QSettings().value(QStringLiteral("lastProject")).toString();
    if (!last.isEmpty())
        candidates << last;
    candidates << defaultProjectCandidates();

    for (const QString &candidate : std::as_const(candidates)) {
        if (candidate.isEmpty() || !QFileInfo::exists(candidate))
            continue;
        if (openProject(candidate))
            return;
    }

    statusBar()->showMessage(
        tr("Open a NixOS configuration to get started (File ▸ Open configuration…)."));
}

bool MainWindow::openProject(const QString &path)
{
    if (m_ctx.project->hasUnsavedChanges()) {
        const auto answer = QMessageBox::question(this, tr("Unsaved changes"),
            tr("The current configuration has unsaved changes. Save them before opening another?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save && !saveAll())
            return false;
    }

    QString err;
    if (!m_ctx.project->open(path, &err)) {
        QMessageBox::warning(this, tr("Open configuration"), err);
        return false;
    }

    rememberRecent(m_ctx.project->root());
    m_ctx.search->setChannel(m_ctx.project->nixpkgsChannel());
    refreshAll();

    if (!m_ctx.project->isWritable()) {
        statusBar()->showMessage(
            tr("%1 is not writable by you; saving will ask for elevated privileges.")
                .arg(m_ctx.project->root()),
            12000);
    }
    return true;
}

void MainWindow::chooseProject()
{
    const QString start = m_ctx.project->isOpen() ? m_ctx.project->root() : QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(this,
        tr("Choose a directory containing flake.nix or configuration.nix"), start);
    if (!dir.isEmpty())
        openProject(dir);
}

void MainWindow::refreshAll()
{
    m_hosts->refresh();
    m_modules->refresh();
    m_packages->refresh();
    m_options->refresh();
    m_editor->refresh();
    m_git->refresh();
    m_system->refresh();
    updateWindowTitle();
    updateDirtyState();
}

void MainWindow::reloadProject()
{
    if (!m_ctx.project->isOpen())
        return;
    if (m_ctx.project->hasUnsavedChanges()) {
        const auto answer = QMessageBox::question(this, tr("Reload"),
            tr("Discard unsaved changes and re-read every file from disk?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    m_ctx.project->reloadAll();
    refreshAll();
    statusBar()->showMessage(tr("Reloaded from disk."), 5000);
}

bool MainWindow::saveAll()
{
    if (!m_ctx.project->isOpen() || !m_ctx.project->hasUnsavedChanges())
        return true;

    QVector<QPair<QString, QString>> needsPrivilege;
    QString err;
    const bool ok = m_ctx.project->saveAll(&needsPrivilege, &err);

    if (!needsPrivilege.isEmpty()) {
        QStringList destinations;
        for (const auto &pair : std::as_const(needsPrivilege))
            destinations << pair.second;

        const auto answer = QMessageBox::question(this, tr("Elevated write required"),
            tr("These files are not writable by your user:\n\n%1\n\nWrite them as root?")
                .arg(destinations.join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            QVector<CommandRunner::Step> steps;
            for (const auto &pair : std::as_const(needsPrivilege))
                steps << SystemOps::installFileAsRoot(pair.first, pair.second);
            m_logDock->show();
            m_ctx.runner->run(steps);
            // The temp files hold exactly what these buffers contain, so treat
            // them as saved. A failed install shows up in the log and can be
            // retried; buffers that failed for other reasons stay dirty.
            for (NixFile *f : m_ctx.project->dirtyFiles())
                if (destinations.contains(f->path()))
                    f->markClean();
        } else {
            for (const auto &pair : std::as_const(needsPrivilege))
                QFile::remove(pair.first);
            updateDirtyState();
            return false;
        }
    }

    if (!ok && needsPrivilege.isEmpty()) {
        QMessageBox::warning(this, tr("Save failed"), err);
        updateDirtyState();
        return false;
    }

    updateDirtyState();
    m_git->refresh();
    statusBar()->showMessage(tr("Saved."), 5000);
    return true;
}

void MainWindow::updateWindowTitle()
{
    // The "[*]" is Qt's placeholder for the modified marker; setWindowModified
    // needs it to be present.
    if (!m_ctx.project->isOpen()) {
        setWindowTitle(tr("NixOS Manager[*]"));
        m_header->clearProject();
        m_statusPath->clear();
        return;
    }
    const QString kind = m_ctx.project->kind() == ConfigProject::Flake ? tr("flake")
                                                                      : tr("single file");
    setWindowTitle(tr("%1[*] — NixOS Manager").arg(m_ctx.project->root()));
    m_header->setProject(m_ctx.project->root(), kind, int(m_ctx.project->hosts().size()),
        m_ctx.project->nixpkgsChannel());
    m_statusPath->clear();
}

void MainWindow::updateDirtyState()
{
    const int dirty = int(m_ctx.project->dirtyFiles().size());
    m_saveAction->setEnabled(dirty > 0);
    m_header->setSaveEnabled(dirty > 0);
    m_header->setDirtyCount(dirty);
    m_statusDirty->clear();
    setWindowModified(dirty > 0);
}

void MainWindow::showSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Settings"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *helper = new QComboBox(&dialog);
    helper->addItems(CommandRunner::availableEscalationHelpers());
    const int index = helper->findText(m_ctx.runner->escalationHelper());
    if (index >= 0)
        helper->setCurrentIndex(index);
    helper->setToolTip(tr("sudo asks for your password in a dialog, then caches it the way "
                          "sudo normally does. pkexec goes through your desktop's polkit "
                          "agent instead."));
    form->addRow(tr("Privilege helper:"), helper);

    auto *ttl = new QSpinBox(&dialog);
    ttl->setRange(0, 720);
    ttl->setValue(m_ctx.search->cacheTtlHours());
    ttl->setSuffix(tr(" hours"));
    ttl->setSpecialValueText(tr("never expire"));
    form->addRow(tr("Package cache lifetime:"), ttl);

    layout->addLayout(form);

    auto *note = new QLabel(
        tr("Package data comes from the same index as search.nixos.org. Set "
           "<code>NIXOS_MANAGER_SEARCH_URL</code> to use your own mirror."),
        &dialog);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_ctx.runner->setEscalationHelper(helper->currentText());
    m_ctx.search->setCacheTtlHours(ttl->value());

    QSettings settings;
    settings.setValue(QStringLiteral("system/escalationHelper"), helper->currentText());
    settings.setValue(QStringLiteral("search/cacheTtlHours"), ttl->value());
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About NixOS Manager"),
        tr("<h3>NixOS Manager %1</h3>"
           "<p>A Qt front end for NixOS configurations: browse hosts and modules, toggle imports, "
           "search nixpkgs and add packages, then rebuild, roll back or clean up — without "
           "leaving the app.</p>"
           "<p>Edits are applied as minimal in-place changes, so your formatting and comments "
           "survive. Anything the app does not model can still be edited by hand in the Editor "
           "tab.</p>")
            .arg(QStringLiteral(NIXOS_MANAGER_VERSION)));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_ctx.project->hasUnsavedChanges()) {
        const auto answer = QMessageBox::question(this, tr("Unsaved changes"),
            tr("Save your configuration changes before quitting?"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save && !saveAll()) {
            event->ignore();
            return;
        }
    }

    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    QMainWindow::closeEvent(event);
}

} // namespace nixm
