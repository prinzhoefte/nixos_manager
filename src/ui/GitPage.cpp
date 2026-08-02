#include "GitPage.h"

#include "Theme.h"
#include "core/CommandRunner.h"
#include "core/ConfigProject.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextCharFormat>
#include <QTextBlock>
#include <QTextCursor>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace nixm {
namespace {

constexpr int kRolePath = Qt::UserRole + 1;
constexpr int kRoleStaged = Qt::UserRole + 2;

} // namespace

GitPage::GitPage(const AppContext &ctx, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    buildUi();

    connect(m_ctx.runner, &CommandRunner::runningChanged, this, [this](bool running) {
        for (QWidget *w : { static_cast<QWidget *>(m_commitButton),
                 static_cast<QWidget *>(m_stageButton), static_cast<QWidget *>(m_unstageButton),
                 static_cast<QWidget *>(m_discardButton), static_cast<QWidget *>(m_pushButton),
                 static_cast<QWidget *>(m_pullButton), static_cast<QWidget *>(m_fetchButton) }) {
            if (w)
                w->setEnabled(!running);
        }
    });
}

void GitPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack);

    // ── Page 0: a working repository ─────────────────────────────────────────
    auto *repoPage = new QWidget(m_stack);
    auto *repoLayout = new QVBoxLayout(repoPage);
    repoLayout->setContentsMargins(8, 8, 8, 8);
    repoLayout->addWidget(buildSummary());

    auto *splitter = new QSplitter(Qt::Horizontal, repoPage);
    splitter->addWidget(buildChanges());

    auto *rightSide = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightSide);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(buildCommitBox());
    rightLayout->addWidget(buildHistory(), 1);
    splitter->addWidget(rightSide);
    splitter->setSizes({ 620, 460 });
    repoLayout->addWidget(splitter, 1);

    m_stack->addWidget(repoPage);

    // ── Page 1: no repository yet ────────────────────────────────────────────
    m_stack->addWidget(buildSetupPage());
}

QWidget *GitPage::buildSummary()
{
    auto *group = new QGroupBox(tr("Repository"), this);
    auto *layout = new QVBoxLayout(group);

    auto *row = new QHBoxLayout;
    m_branchLabel = new QLabel(group);
    m_branchLabel->setTextFormat(Qt::RichText);
    row->addWidget(m_branchLabel);
    row->addStretch(1);

    m_fetchButton = new QPushButton(tr("Fetch"), group);
    m_fetchButton->setIcon(Theme::icon(QStringLiteral("reload")));
    connect(m_fetchButton, &QPushButton::clicked, this, [this] {
        runSteps({ GitRepo::fetch(m_status.root) }, tr("Fetched from the remote."));
    });
    row->addWidget(m_fetchButton);

    m_pullButton = new QPushButton(tr("Pull"), group);
    m_pullButton->setIcon(Theme::icon(QStringLiteral("pull")));
    m_pullButton->setToolTip(tr("git pull --ff-only — refuses rather than creating a merge."));
    connect(m_pullButton, &QPushButton::clicked, this, [this] {
        runSteps({ GitRepo::pull(m_status.root) }, tr("Pulled."));
    });
    row->addWidget(m_pullButton);

    m_pushButton = new QPushButton(tr("Push"), group);
    m_pushButton->setIcon(Theme::icon(QStringLiteral("push"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_pushButton);
    connect(m_pushButton, &QPushButton::clicked, this, [this] {
        const bool needsUpstream = m_status.upstream.isEmpty();
        runSteps({ GitRepo::push(m_status.root, m_status.branch, needsUpstream) },
            tr("Pushed."));
    });
    row->addWidget(m_pushButton);
    layout->addLayout(row);

    m_remoteLabel = new QLabel(group);
    m_remoteLabel->setEnabled(false);
    m_remoteLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_remoteLabel);

    m_warning = new QLabel(group);
    m_warning->setWordWrap(true);
    m_warning->setTextFormat(Qt::RichText);
    m_warning->setVisible(false);
    layout->addWidget(m_warning);

    return group;
}

QWidget *GitPage::buildChanges()
{
    auto *group = new QGroupBox(tr("Changes"), this);
    auto *layout = new QVBoxLayout(group);

    m_changes = new QTreeWidget(group);
    m_changes->setColumnCount(2);
    m_changes->setHeaderLabels({ tr("File"), tr("Status") });
    m_changes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_changes->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_changes->header()->resizeSection(1, 150);
    m_changes->header()->setStretchLastSection(false);
    m_changes->setRootIsDecorated(false);
    m_changes->setAlternatingRowColors(true);
    m_changes->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(m_changes, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem *item, QTreeWidgetItem *) { showDiffFor(item); });
    connect(m_changes, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        const QString path = item->data(0, kRolePath).toString();
        if (path.isEmpty())
            return;
        const QString abs = QDir(m_status.root).absoluteFilePath(path);
        if (QFileInfo(abs).isFile())
            emit openFileRequested(abs);
    });
    layout->addWidget(m_changes, 1);

    auto *buttons = new QHBoxLayout;
    m_stageButton = new QPushButton(tr("Stage"), group);
    m_stageButton->setIcon(Theme::icon(QStringLiteral("add")));
    connect(m_stageButton, &QPushButton::clicked, this, &GitPage::stageSelected);
    buttons->addWidget(m_stageButton);

    m_unstageButton = new QPushButton(tr("Unstage"), group);
    m_unstageButton->setIcon(Theme::icon(QStringLiteral("remove")));
    connect(m_unstageButton, &QPushButton::clicked, this, &GitPage::unstageSelected);
    buttons->addWidget(m_unstageButton);

    m_discardButton = new QPushButton(tr("Discard…"), group);
    m_discardButton->setIcon(Theme::icon(QStringLiteral("trash")));
    m_discardButton->setProperty("danger", true);
    m_discardButton->setToolTip(tr("Throw away the working-tree changes in the selected files."));
    connect(m_discardButton, &QPushButton::clicked, this, &GitPage::discardSelected);
    buttons->addWidget(m_discardButton);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    m_diff = new QPlainTextEdit(group);
    m_diff->setReadOnly(true);
    m_diff->setProperty("mono", true);
    m_diff->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_diff->setPlaceholderText(tr("Select a file to see what changed."));
    m_diff->setMinimumHeight(200);
    layout->addWidget(m_diff, 1);

    return group;
}

QWidget *GitPage::buildCommitBox()
{
    auto *group = new QGroupBox(tr("Commit"), this);
    auto *layout = new QVBoxLayout(group);

    m_message = new QPlainTextEdit(group);
    m_message->setPlaceholderText(
        tr("Describe the change — “Enable openssh on bedroom-pc”, “Bump nixpkgs”, …"));
    m_message->setMaximumHeight(90);
    layout->addWidget(m_message);

    m_stageAll = new QCheckBox(tr("Stage every change first"), group);
    m_stageAll->setChecked(true);
    m_stageAll->setToolTip(tr("Equivalent to git add -A before committing."));
    layout->addWidget(m_stageAll);

    m_amend = new QCheckBox(tr("Amend the previous commit"), group);
    layout->addWidget(m_amend);

    auto *row = new QHBoxLayout;
    m_commitButton = new QPushButton(tr("Commit"), group);
    m_commitButton->setIcon(Theme::icon(QStringLiteral("commit"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_commitButton);
    connect(m_commitButton, &QPushButton::clicked, this, &GitPage::commitNow);
    row->addWidget(m_commitButton);
    row->addStretch(1);
    layout->addLayout(row);

    return group;
}

QWidget *GitPage::buildHistory()
{
    auto *group = new QGroupBox(tr("History"), this);
    auto *layout = new QVBoxLayout(group);

    m_history = new QTableWidget(group);
    m_history->setColumnCount(4);
    m_history->setHorizontalHeaderLabels({ tr("Commit"), tr("Date"), tr("Author"), tr("Subject") });
    m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_history->verticalHeader()->setVisible(false);
    m_history->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_history->setAlternatingRowColors(true);
    m_history->setTextElideMode(Qt::ElideRight);
    layout->addWidget(m_history);

    return group;
}

QWidget *GitPage::buildSetupPage()
{
    auto *page = new QWidget(m_stack);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);

    auto *group = new QGroupBox(tr("Version control"), page);
    auto *inner = new QVBoxLayout(group);

    m_setupText = new QLabel(group);
    m_setupText->setWordWrap(true);
    m_setupText->setTextFormat(Qt::RichText);
    m_setupText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inner->addWidget(m_setupText);

    auto *row = new QHBoxLayout;
    m_initButton = new QPushButton(tr("Initialise a repository here"), group);
    m_initButton->setIcon(Theme::icon(QStringLiteral("branch"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_initButton);
    connect(m_initButton, &QPushButton::clicked, this, &GitPage::initRepository);
    row->addWidget(m_initButton);
    row->addStretch(1);
    inner->addLayout(row);

    layout->addWidget(group);
    layout->addStretch(1);
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────

void GitPage::refresh()
{
    if (!m_ctx.project || !m_ctx.project->isOpen()) {
        m_status = GitStatus();
        m_stack->setCurrentIndex(1);
        m_setupText->setText(tr("Open a configuration first."));
        m_initButton->setEnabled(false);
        emit repositoryStateChanged(m_status);
        return;
    }

    m_status = GitRepo::status(m_ctx.project->root());
    emit repositoryStateChanged(m_status);

    if (!m_status.isRepository) {
        m_stack->setCurrentIndex(1);
        const QString root = m_ctx.project->root().toHtmlEscaped();
        if (!GitRepo::available()) {
            m_setupText->setText(
                tr("<p>git is not installed, so this tab cannot do anything.</p>"
                   "<p>Add <code>git</code> to <code>environment.systemPackages</code> and "
                   "rebuild.</p>"));
            m_initButton->setEnabled(false);
        } else if (!m_status.error.isEmpty()) {
            m_setupText->setText(tr("<p><b>%1</b> is a repository, but git refused to read it:</p>"
                                    "<pre>%2</pre>"
                                    "<p>This normally means the tree belongs to another user — "
                                    "a root-owned <code>/etc/nixos</code>, for instance. Either "
                                    "run the app as that user, or tell git the directory is "
                                    "trusted.</p>")
                                     .arg(root, m_status.error.toHtmlEscaped()));
            m_initButton->setText(tr("Mark as a safe directory"));
            m_initButton->setEnabled(true);
        } else {
            m_setupText->setText(
                tr("<p><b>%1</b> is not under version control yet.</p>"
                   "<p>Putting your NixOS configuration in git gives you a readable history of "
                   "every change, and a way back when a rebuild goes wrong. The app writes "
                   "small, surgical diffs precisely so this stays useful.</p>"
                   "<p>Initialising creates a repository on branch <code>main</code>; nothing "
                   "is committed until you say so.</p>")
                    .arg(root));
            m_initButton->setText(tr("Initialise a repository here"));
            m_initButton->setEnabled(m_ctx.project->isWritable());
        }
        return;
    }

    m_stack->setCurrentIndex(0);

    // ── Summary line ─────────────────────────────────────────────────────────
    const ThemeColors &c = Theme::colors();
    QString branch = m_status.detached
        ? tr("detached HEAD")
        : (m_status.branch.isEmpty() ? tr("no branch") : m_status.branch);
    QString text = QStringLiteral("<span style='font-size:12pt;font-weight:800;color:%1;'>%2</span>")
                       .arg(c.dark ? c.lightBlue.name() : c.primary.name(), branch.toHtmlEscaped());
    if (!m_status.upstream.isEmpty()) {
        text += QStringLiteral("<span style='color:%1;'> → %2</span>")
                    .arg(c.textMuted.name(), m_status.upstream.toHtmlEscaped());
    }
    if (m_status.ahead > 0 || m_status.behind > 0) {
        text += QStringLiteral("<span style='color:%1;font-weight:700;'>  ↑%2 ↓%3</span>")
                    .arg(c.amber.name())
                    .arg(m_status.ahead)
                    .arg(m_status.behind);
    }
    const int dirty = m_status.changes.size();
    text += QStringLiteral("<span style='color:%1;'>  ·  %2</span>")
                .arg(dirty > 0 ? c.amber.name() : c.textMuted.name(),
                    dirty > 0 ? tr("%n change(s)", nullptr, dirty) : tr("clean"));
    m_branchLabel->setText(text);

    m_remoteLabel->setText(m_status.remoteUrl.isEmpty()
            ? tr("No remote configured — add one with: git remote add origin <url>")
            : m_status.remoteUrl);

    m_pushButton->setEnabled(!m_status.remoteUrl.isEmpty());
    m_pullButton->setEnabled(!m_status.upstream.isEmpty());
    m_fetchButton->setEnabled(!m_status.remoteUrl.isEmpty());

    if (!m_status.hasIdentity) {
        m_warning->setText(
            tr("<span style='color:%1;'><b>No commit identity.</b> git needs a "
               "<code>user.name</code> and <code>user.email</code> before it will let you "
               "commit. <a href='#identity'>Set one for this repository</a>.</span>")
                .arg(c.amber.name()));
        m_warning->setVisible(true);
        m_warning->setTextInteractionFlags(Qt::TextBrowserInteraction);
        disconnect(m_warning, &QLabel::linkActivated, nullptr, nullptr);
        connect(m_warning, &QLabel::linkActivated, this, &GitPage::askIdentity,
            Qt::UniqueConnection);
    } else {
        m_warning->setVisible(false);
    }

    reloadChanges();
    reloadHistory();
}

void GitPage::applyTheme()
{
    const ThemeColors &c = Theme::colors();
    m_fetchButton->setIcon(Theme::icon(QStringLiteral("reload")));
    m_pullButton->setIcon(Theme::icon(QStringLiteral("pull")));
    m_pushButton->setIcon(Theme::icon(QStringLiteral("push"), c.textOnBrand));
    m_commitButton->setIcon(Theme::icon(QStringLiteral("commit"), c.textOnBrand));
    m_stageButton->setIcon(Theme::icon(QStringLiteral("add")));
    m_unstageButton->setIcon(Theme::icon(QStringLiteral("remove")));
    m_discardButton->setIcon(Theme::icon(QStringLiteral("trash")));
    m_initButton->setIcon(Theme::icon(QStringLiteral("branch"), c.textOnBrand));
    Theme::makePrimary(m_pushButton);
    Theme::makePrimary(m_commitButton);
    Theme::makePrimary(m_initButton);
    refresh();
}

void GitPage::reloadChanges()
{
    const QString previous
        = m_changes->currentItem() ? m_changes->currentItem()->data(0, kRolePath).toString()
                                   : QString();

    m_updating = true;
    m_changes->clear();

    const ThemeColors &c = Theme::colors();
    QTreeWidgetItem *toSelect = nullptr;

    for (const GitFileChange &change : m_status.changes) {
        auto *item = new QTreeWidgetItem(m_changes);
        item->setText(0, change.originalPath.isEmpty()
                ? change.path
                : QStringLiteral("%1 ← %2").arg(change.path, change.originalPath));
        item->setText(1, change.describe());
        item->setData(0, kRolePath, change.path);
        item->setData(0, kRoleStaged, change.isStaged());

        // Green for staged, amber for pending, red for a conflict.
        QColor dot = c.amber;
        if (change.conflicted)
            dot = c.danger;
        else if (change.isStaged() && !change.isUnstaged())
            dot = c.success;
        item->setIcon(0, Theme::dot(dot));

        if (change.path == previous)
            toSelect = item;
    }

    if (m_status.changes.isEmpty()) {
        auto *item = new QTreeWidgetItem(m_changes);
        item->setText(0, tr("Nothing has changed since the last commit."));
        item->setFlags(Qt::NoItemFlags);
        m_diff->clear();
    }

    m_updating = false;
    if (toSelect)
        m_changes->setCurrentItem(toSelect);
    else if (m_changes->topLevelItemCount() > 0 && !m_status.changes.isEmpty())
        m_changes->setCurrentItem(m_changes->topLevelItem(0));

    const bool any = !m_status.changes.isEmpty();
    m_stageButton->setEnabled(any);
    m_unstageButton->setEnabled(any);
    m_discardButton->setEnabled(any);
}

void GitPage::reloadHistory()
{
    const auto commits = GitRepo::log(m_status.root, 40);
    m_history->setRowCount(commits.size());
    for (int row = 0; row < commits.size(); ++row) {
        const GitCommit &commit = commits.at(row);
        auto *hash = new QTableWidgetItem(commit.shortHash);
        QFont mono(Theme::monoFontFamily());
        hash->setFont(mono);
        m_history->setItem(row, 0, hash);
        m_history->setItem(row, 1, new QTableWidgetItem(commit.date));
        m_history->setItem(row, 2, new QTableWidgetItem(commit.author));
        m_history->setItem(row, 3, new QTableWidgetItem(commit.subject));
    }
    if (commits.isEmpty()) {
        m_history->setRowCount(1);
        auto *item = new QTableWidgetItem(tr("No commits yet."));
        item->setFlags(Qt::NoItemFlags);
        m_history->setItem(0, 0, item);
    }
}

void GitPage::showDiffFor(QTreeWidgetItem *item)
{
    if (m_updating || !item)
        return;
    const QString path = item->data(0, kRolePath).toString();
    if (path.isEmpty()) {
        m_diff->clear();
        return;
    }

    QString text = GitRepo::diff(m_status.root, path, false);
    if (text.trimmed().isEmpty())
        text = GitRepo::diff(m_status.root, path, true);

    m_diff->setPlainText(text);

    // Colour the +/- lines. The diff pane is small, so doing it by hand beats
    // pulling in a highlighter.
    const ThemeColors &c = Theme::colors();
    QTextCursor cursor(m_diff->document());
    for (QTextBlock block = m_diff->document()->begin(); block.isValid();
        block = block.next()) {
        const QString line = block.text();
        QTextCharFormat format;
        if (line.startsWith(QLatin1String("+++")) || line.startsWith(QLatin1String("---"))
            || line.startsWith(QLatin1String("diff "))
            || line.startsWith(QLatin1String("index "))) {
            format.setForeground(c.textMuted);
        } else if (line.startsWith(QLatin1String("@@"))) {
            format.setForeground(c.accent);
        } else if (line.startsWith(QLatin1Char('+'))) {
            format.setForeground(c.success);
        } else if (line.startsWith(QLatin1Char('-'))) {
            format.setForeground(c.danger);
        } else {
            continue;
        }
        cursor.setPosition(block.position());
        cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        cursor.setCharFormat(format);
    }
    m_diff->moveCursor(QTextCursor::Start);
}

QStringList GitPage::selectedPaths() const
{
    QStringList paths;
    const auto items = m_changes->selectedItems();
    for (QTreeWidgetItem *item : items) {
        const QString path = item->data(0, kRolePath).toString();
        if (!path.isEmpty() && !paths.contains(path))
            paths << path;
    }
    return paths;
}

// ── Actions ──────────────────────────────────────────────────────────────────

void GitPage::runSteps(const QVector<CommandRunner::Step> &steps, const QString &done)
{
    if (steps.isEmpty())
        return;
    m_pendingMessage = done;

    // One-shot: refresh and report once this batch finishes.
    auto *connection = new QMetaObject::Connection;
    *connection = connect(m_ctx.runner, &CommandRunner::batchFinished, this,
        [this, connection](bool ok) {
            disconnect(*connection);
            delete connection;
            if (ok && !m_pendingMessage.isEmpty())
                emit statusMessage(m_pendingMessage);
            refresh();
        });

    m_ctx.runner->run(steps);
}

void GitPage::stageSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) {
        emit statusMessage(tr("Select the files to stage."));
        return;
    }
    emit saveRequested();
    runSteps({ GitRepo::stage(m_status.root, paths) },
        tr("Staged %n file(s).", nullptr, int(paths.size())));
}

void GitPage::unstageSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    runSteps({ GitRepo::unstage(m_status.root, paths) },
        tr("Unstaged %n file(s).", nullptr, int(paths.size())));
}

void GitPage::discardSelected()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;

    const auto answer = QMessageBox::question(this, tr("Discard changes"),
        tr("Throw away your changes to:\n\n%1\n\nThis cannot be undone.")
            .arg(paths.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    runSteps({ GitRepo::discard(m_status.root, paths) },
        tr("Discarded changes in %n file(s).", nullptr, int(paths.size())));

    // The buffers still hold the old text; drop them so the UI re-reads disk.
    if (m_ctx.project)
        m_ctx.project->reloadAll();
}

void GitPage::commitNow()
{
    const QString message = m_message->toPlainText().trimmed();
    if (message.isEmpty()) {
        QMessageBox::information(this, tr("Commit"), tr("Please write a commit message."));
        m_message->setFocus();
        return;
    }
    if (!m_status.hasIdentity) {
        askIdentity();
        return;
    }

    // Uncommitted editor buffers would be silently left out otherwise.
    emit saveRequested();

    QVector<CommandRunner::Step> steps;
    if (m_stageAll->isChecked()) {
        CommandRunner::Step add;
        add.label = tr("stage every change");
        add.program = QStringLiteral("git");
        add.args = { QStringLiteral("add"), QStringLiteral("--all") };
        add.workDir = m_status.root;
        steps << add;
    }
    steps << GitRepo::commit(m_status.root, message, m_amend->isChecked());

    m_message->clear();
    m_amend->setChecked(false);
    runSteps(steps, tr("Committed."));
}

void GitPage::initRepository()
{
    if (!m_ctx.project || !m_ctx.project->isOpen())
        return;

    // The "safe directory" case reuses this button.
    if (!m_status.error.isEmpty()) {
        runSteps({ GitRepo::markSafeDirectory(m_ctx.project->root()) },
            tr("Marked as a safe directory."));
        return;
    }

    QVector<CommandRunner::Step> steps { GitRepo::init(m_ctx.project->root()) };

    // A NixOS tree almost always wants these ignored.
    const QString gitignorePath
        = QDir(m_ctx.project->root()).absoluteFilePath(QStringLiteral(".gitignore"));
    if (!QFileInfo::exists(gitignorePath)) {
        QFile f(gitignorePath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write("# Build outputs\nresult\nresult-*\n\n"
                    "# Editor and tooling leftovers\n.direnv/\n*.swp\n");
            f.close();
        }
    }

    runSteps(steps, tr("Repository initialised."));
}

void GitPage::askIdentity()
{
    if (!m_status.isRepository)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Commit identity"));
    auto *layout = new QVBoxLayout(&dialog);

    auto *intro = new QLabel(
        tr("git records who made each change. These are written to this repository only, "
           "not to your global git configuration."),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    auto *name = new QLineEdit(&dialog);
    name->setPlaceholderText(QStringLiteral("Justin Rauch"));
    form->addRow(tr("Name:"), name);
    auto *email = new QLineEdit(&dialog);
    email->setPlaceholderText(QStringLiteral("justin@example.com"));
    form->addRow(tr("Email:"), email);
    layout->addLayout(form);

    auto *buttons
        = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;
    if (name->text().trimmed().isEmpty() || email->text().trimmed().isEmpty())
        return;

    runSteps(GitRepo::setIdentity(m_status.root, name->text().trimmed(), email->text().trimmed()),
        tr("Commit identity set."));
}

} // namespace nixm
