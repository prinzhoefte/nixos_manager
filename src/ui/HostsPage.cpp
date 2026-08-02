#include "HostsPage.h"

#include "Theme.h"
#include "core/ConfigProject.h"
#include "core/NixFile.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QMessageBox>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace nixm {
namespace {

constexpr int kRoleAbsPath = Qt::UserRole + 1;
constexpr int kRoleImportLiteral = Qt::UserRole + 2;
constexpr int kRoleCategory = Qt::UserRole + 3;
constexpr int kRoleOptionPath = Qt::UserRole + 4;
constexpr int kRoleOptionIsBool = Qt::UserRole + 5;

QString statusText(bool present, bool enabled)
{
    if (!present)
        return QObject::tr("not imported");
    return enabled ? QObject::tr("active") : QObject::tr("commented out");
}

/// Colour of the dot in the Status column. Amber flags "present but switched
/// off", which is the state worth noticing at a glance.
QColor statusColour(bool present, bool enabled, bool global)
{
    const ThemeColors &c = Theme::colors();
    if (global)
        return c.accent;
    if (!present)
        return c.line;
    return enabled ? c.success : c.amber;
}

} // namespace

HostsPage::HostsPage(const AppContext &ctx, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    buildUi();
}

void HostsPage::buildUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    // ── Host list ────────────────────────────────────────────────────────────
    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(6, 6, 6, 6);
    auto *hostsCaption = new QLabel(tr("Hosts"), left);
    Theme::makeEyebrow(hostsCaption);
    leftLayout->addWidget(hostsCaption);

    m_hostList = new QListWidget(left);
    connect(m_hostList, &QListWidget::currentRowChanged, this, [this](int) { onHostSelected(); });
    leftLayout->addWidget(m_hostList, 1);

    m_summary = new QLabel(left);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    leftLayout->addWidget(m_summary);

    m_newHost = new QPushButton(tr("New host…"), left);
    m_newHost->setIcon(Theme::icon(QStringLiteral("add"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_newHost);
    connect(m_newHost, &QPushButton::clicked, this, &HostsPage::createHost);
    leftLayout->addWidget(m_newHost);

    splitter->addWidget(left);

    // ── Host details ─────────────────────────────────────────────────────────
    auto *scroll = new QScrollArea(splitter);
    scroll->setWidgetResizable(true);
    auto *right = new QWidget(scroll);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 8, 8, 8);

    auto *identity = new QGroupBox(tr("Identity"), right);
    auto *form = new QFormLayout(identity);
    m_hostName = new QLineEdit(identity);
    m_hostName->setPlaceholderText(QStringLiteral("networking.hostName"));
    connect(m_hostName, &QLineEdit::editingFinished, this, &HostsPage::commitIdentity);
    form->addRow(tr("Host name:"), m_hostName);

    m_stateVersion = new QLineEdit(identity);
    m_stateVersion->setPlaceholderText(QStringLiteral("system.stateVersion"));
    connect(m_stateVersion, &QLineEdit::editingFinished, this, &HostsPage::commitIdentity);
    form->addRow(tr("State version:"), m_stateVersion);

    auto *entryRow = new QHBoxLayout;
    m_entryLabel = new QLabel(identity);
    m_entryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    entryRow->addWidget(m_entryLabel, 1);
    m_openHostFile = new QPushButton(tr("Open in editor"), identity);
    m_openHostFile->setIcon(Theme::icon(QStringLiteral("editor")));
    connect(m_openHostFile, &QPushButton::clicked, this, [this] {
        const HostInfo *h = m_ctx.project->host(currentHost());
        if (h)
            emit openFileRequested(h->entryFile);
    });
    entryRow->addWidget(m_openHostFile);
    m_rebuild = new QPushButton(tr("Rebuild this host…"), identity);
    m_rebuild->setIcon(Theme::icon(QStringLiteral("run"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_rebuild);
    connect(m_rebuild, &QPushButton::clicked, this,
        [this] { emit rebuildRequested(currentHost()); });
    entryRow->addWidget(m_rebuild);
    form->addRow(tr("Entry file:"), entryRow);
    rightLayout->addWidget(identity);

    // ── Modules ──────────────────────────────────────────────────────────────
    auto *modules = new QGroupBox(tr("Modules"), right);
    auto *modLayout = new QVBoxLayout(modules);

    auto *hint = new QLabel(
        tr("Unticking a module comments its import out instead of deleting the line, so nothing "
           "is lost and the diff stays readable."),
        modules);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    modLayout->addWidget(hint);

    m_moduleFilter = new QLineEdit(modules);
    m_moduleFilter->setPlaceholderText(tr("Filter modules…"));
    m_moduleFilter->setClearButtonEnabled(true);
    connect(m_moduleFilter, &QLineEdit::textChanged, this, &HostsPage::applyModuleFilter);
    modLayout->addWidget(m_moduleFilter);

    m_moduleTree = new QTreeWidget(modules);
    m_moduleTree->setColumnCount(3);
    m_moduleTree->setHeaderLabels({ tr("Module"), tr("Status"), tr("Path") });
    m_moduleTree->setRootIsDecorated(true);
    m_moduleTree->setUniformRowHeights(true);
    m_moduleTree->setAlternatingRowColors(true);
    m_moduleTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_moduleTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_moduleTree->header()->setStretchLastSection(true);
    m_moduleTree->setMinimumHeight(260);
    connect(m_moduleTree, &QTreeWidget::itemChanged, this, &HostsPage::onModuleItemChanged);
    connect(m_moduleTree, &QTreeWidget::itemDoubleClicked, this,
        [this](QTreeWidgetItem *item, int) {
            const QString path = item->data(0, kRoleAbsPath).toString();
            if (!path.isEmpty())
                emit openFileRequested(path);
        });
    modLayout->addWidget(m_moduleTree, 1);
    rightLayout->addWidget(modules, 1);

    // ── Feature options ──────────────────────────────────────────────────────
    auto *options = new QGroupBox(tr("Feature options declared by your modules"), right);
    auto *optLayout = new QVBoxLayout(options);
    m_optionTree = new QTreeWidget(options);
    m_optionTree->setColumnCount(3);
    m_optionTree->setHeaderLabels({ tr("Option"), tr("Value"), tr("Description") });
    m_optionTree->setRootIsDecorated(false);
    m_optionTree->setAlternatingRowColors(true);
    m_optionTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_optionTree->header()->setStretchLastSection(true);
    m_optionTree->setMaximumHeight(160);
    connect(m_optionTree, &QTreeWidget::itemChanged, this, &HostsPage::onOptionItemChanged);
    optLayout->addWidget(m_optionTree);
    rightLayout->addWidget(options);

    // ── Users ────────────────────────────────────────────────────────────────
    auto *users = new QGroupBox(tr("Users defined in this host"), right);
    auto *userLayout = new QVBoxLayout(users);
    m_userTree = new QTreeWidget(users);
    m_userTree->setColumnCount(3);
    m_userTree->setHeaderLabels({ tr("User"), tr("Description"), tr("Groups") });
    m_userTree->setRootIsDecorated(false);
    m_userTree->header()->setStretchLastSection(true);
    m_userTree->setMaximumHeight(120);
    userLayout->addWidget(m_userTree);
    rightLayout->addWidget(users);

    scroll->setWidget(right);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 200, 800 });
}

QString HostsPage::currentHost() const
{
    auto *item = m_hostList->currentItem();
    return item ? item->text() : QString();
}

void HostsPage::selectHost(const QString &name)
{
    for (int i = 0; i < m_hostList->count(); ++i) {
        if (m_hostList->item(i)->text() == name) {
            m_hostList->setCurrentRow(i);
            return;
        }
    }
}

void HostsPage::refresh()
{
    const QString previous = currentHost();

    m_updating = true;
    m_hostList->clear();
    if (m_ctx.project && m_ctx.project->isOpen()) {
        for (const HostInfo &h : m_ctx.project->hosts())
            m_hostList->addItem(h.name);
    }
    m_updating = false;

    if (m_hostList->count() == 0) {
        reloadHostDetails();
        return;
    }
    if (!previous.isEmpty())
        selectHost(previous);
    if (m_hostList->currentRow() < 0)
        m_hostList->setCurrentRow(0);
    else
        onHostSelected();
}

void HostsPage::onHostSelected()
{
    if (m_updating)
        return;
    reloadHostDetails();
}

void HostsPage::reloadHostDetails()
{
    m_updating = true;

    const HostInfo *host = m_ctx.project ? m_ctx.project->host(currentHost()) : nullptr;
    const bool valid = host != nullptr;

    m_hostName->setEnabled(valid);
    m_stateVersion->setEnabled(valid);
    m_openHostFile->setEnabled(valid);
    m_rebuild->setEnabled(valid);

    if (!valid) {
        m_hostName->clear();
        m_stateVersion->clear();
        m_entryLabel->clear();
        m_moduleTree->clear();
        m_optionTree->clear();
        m_userTree->clear();
        m_summary->clear();
        m_updating = false;
        return;
    }

    m_entryLabel->setText(host->relEntry);

    NixFile *file = m_ctx.project->file(host->entryFile);
    if (file) {
        const AttrEntry *name = file->findAttr(QStringLiteral("networking.hostName"));
        m_hostName->setText(name ? name->unquoted() : QString());
        const AttrEntry *sv = file->findAttr(QStringLiteral("system.stateVersion"));
        m_stateVersion->setText(sv ? sv->unquoted() : QString());
    }

    m_updating = false;
    reloadModuleTree();
    reloadOptions();
    reloadUsers();
}

QString HostsPage::sectionForCategory(const QString &category) const
{
    const HostInfo *host = m_ctx.project->host(currentHost());
    if (!host)
        return QString();
    NixFile *file = m_ctx.project->file(host->entryFile);
    if (!file)
        return QString();

    for (const QString &section : file->importSections()) {
        if (section.compare(category, Qt::CaseInsensitive) == 0)
            return section;
        // "packages" -> "Packages", "optional" -> "Optional", and so on.
        if (section.startsWith(category, Qt::CaseInsensitive)
            || category.startsWith(section, Qt::CaseInsensitive))
            return section;
    }
    return QString();
}

void HostsPage::reloadModuleTree()
{
    const HostInfo *host = m_ctx.project->host(currentHost());
    if (!host)
        return;
    NixFile *file = m_ctx.project->file(host->entryFile);
    if (!file)
        return;

    m_updating = true;
    m_moduleTree->clear();

    // Map every import of this host to the file it resolves to.
    QHash<QString, const ImportEntry *> byAbsPath;
    for (const ImportEntry &e : file->imports()) {
        const QString abs = ConfigProject::resolveNixPath(host->entryFile, e.text);
        if (!abs.isEmpty())
            byAbsPath.insert(QFileInfo(abs).absoluteFilePath(), &e);
    }

    int active = 0;
    QHash<QString, QTreeWidgetItem *> categories;
    for (const ModuleInfo &m : m_ctx.project->modules()) {
        QTreeWidgetItem *parent = categories.value(m.category);
        if (!parent) {
            parent = new QTreeWidgetItem(m_moduleTree, { m.category });
            QFont f = parent->font(0);
            f.setBold(true);
            parent->setFont(0, f);
            parent->setExpanded(true);
            parent->setFlags(parent->flags() & ~Qt::ItemIsUserCheckable);
            categories.insert(m.category, parent);
        }

        const ImportEntry *entry = byAbsPath.value(m.absPath, nullptr);
        const bool present = entry != nullptr;
        const bool enabled = present && entry->enabled;
        const bool global = !present && m_ctx.project->isGlobalModule(m.absPath);
        if (enabled || global)
            ++active;

        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, m.name);
        item->setText(1, global ? tr("active on every host") : statusText(present, enabled));
        item->setIcon(1, Theme::dot(statusColour(present, enabled, global)));
        if (!present && !global)
            item->setForeground(1, Theme::colors().textMuted);
        item->setText(2, m.relPath);
        item->setData(0, kRoleAbsPath, m.absPath);
        item->setData(0, kRoleCategory, m.category);
        item->setData(0, kRoleImportLiteral,
            present ? entry->text : ConfigProject::relativeNixPath(host->entryFile, m.absPath));
        if (global) {
            // Added by the flake for all hosts, so it is not this host's to toggle.
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setToolTip(0,
                tr("Added to every host by flake.nix, not by this host's imports."));
            QFont f = item->font(1);
            f.setItalic(true);
            item->setFont(1, f);
        } else {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
            if (present && !enabled) {
                QFont f = item->font(1);
                f.setItalic(true);
                item->setFont(1, f);
            }
        }
    }

    // Imports that do not correspond to a module file (hardware-configuration,
    // out-of-tree paths) are listed read-only so nothing is hidden.
    QTreeWidgetItem *others = nullptr;
    for (const ImportEntry &e : file->imports()) {
        const QString abs
            = QFileInfo(ConfigProject::resolveNixPath(host->entryFile, e.text)).absoluteFilePath();
        bool known = false;
        for (const ModuleInfo &m : m_ctx.project->modules()) {
            if (m.absPath == abs) {
                known = true;
                break;
            }
        }
        if (known)
            continue;
        if (!others) {
            others = new QTreeWidgetItem(m_moduleTree, { tr("Other imports") });
            QFont f = others->font(0);
            f.setBold(true);
            others->setFont(0, f);
            others->setExpanded(true);
        }
        auto *item = new QTreeWidgetItem(others);
        item->setText(0, QFileInfo(e.text).fileName());
        item->setText(1, statusText(true, e.enabled));
        item->setIcon(1, Theme::dot(statusColour(true, e.enabled, false)));
        item->setText(2, e.text);
        item->setData(0, kRoleAbsPath, QFileInfo::exists(abs) ? abs : QString());
        item->setData(0, kRoleImportLiteral, e.text);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, e.enabled ? Qt::Checked : Qt::Unchecked);
    }

    m_updating = false;
    applyModuleFilter(m_moduleFilter->text());

    const ThemeColors &c = Theme::colors();
    m_summary->setText(
        QStringLiteral("<div style='font-size:12pt;font-weight:800;color:%1;'>%2</div>"
                       "<div style='color:%3;font-size:9pt;margin-top:2px;'>%4</div>"
                       "<div style='color:%5;font-size:8.5pt;margin-top:4px;'>%6</div>")
            .arg(c.dark ? c.lightBlue.name() : c.primary.name(), host->name.toHtmlEscaped(),
                c.accent.name(), tr("%n active module(s)", nullptr, active),
                c.textMuted.name(), host->relEntry.toHtmlEscaped()));
}

void HostsPage::applyModuleFilter(const QString &text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_moduleTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *category = m_moduleTree->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < category->childCount(); ++j) {
            QTreeWidgetItem *child = category->child(j);
            const bool matches = needle.isEmpty()
                || child->text(0).contains(needle, Qt::CaseInsensitive)
                || child->text(2).contains(needle, Qt::CaseInsensitive)
                || category->text(0).contains(needle, Qt::CaseInsensitive);
            child->setHidden(!matches);
            if (matches)
                ++visible;
        }
        category->setHidden(visible == 0);
    }
}

void HostsPage::onModuleItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_updating || column != 0 || !item->parent())
        return;

    const HostInfo *host = m_ctx.project->host(currentHost());
    if (!host)
        return;
    NixFile *file = m_ctx.project->file(host->entryFile);
    if (!file)
        return;

    const QString literal = item->data(0, kRoleImportLiteral).toString();
    const QString category = item->data(0, kRoleCategory).toString();
    const bool wanted = item->checkState(0) == Qt::Checked;

    bool present = false;
    for (const ImportEntry &e : file->imports()) {
        if (e.text == literal) {
            present = true;
            break;
        }
    }

    bool ok = false;
    if (present) {
        ok = file->setImportEnabled(literal, wanted);
        emit statusMessage(wanted ? tr("Enabled %1").arg(literal)
                                  : tr("Commented out %1").arg(literal));
    } else if (wanted) {
        ok = file->addImport(literal, sectionForCategory(category));
        emit statusMessage(tr("Added import %1").arg(literal));
    } else {
        ok = true;   // already absent
    }

    if (ok)
        emit configModified();

    // We are inside QTreeWidget::itemChanged; rebuilding the tree would delete
    // the item the view is still working with, so defer it to the event loop.
    QMetaObject::invokeMethod(
        this,
        [this] {
            reloadModuleTree();
            reloadOptions();
        },
        Qt::QueuedConnection);
}

void HostsPage::reloadOptions()
{
    const HostInfo *host = m_ctx.project->host(currentHost());
    if (!host)
        return;
    NixFile *file = m_ctx.project->file(host->entryFile);
    if (!file)
        return;

    m_updating = true;
    m_optionTree->clear();

    for (const OptionDecl &o : m_ctx.project->declaredOptions()) {
        const AttrEntry *set = file->findAttr(o.path);
        const bool isBool = o.type.contains(QLatin1String("bool"))
            || o.defaultValue == QLatin1String("true") || o.defaultValue == QLatin1String("false");

        auto *item = new QTreeWidgetItem(m_optionTree);
        item->setText(0, o.path);
        item->setData(0, kRoleOptionPath, o.path);
        item->setData(0, kRoleOptionIsBool, isBool);
        item->setToolTip(0,
            tr("Declared in %1\nDefault: %2")
                .arg(QFileInfo(o.file).fileName(),
                    o.defaultValue.isEmpty() ? tr("(none)") : o.defaultValue));
        item->setText(2, o.description);

        if (isBool) {
            const bool on = set ? set->rawValue.trimmed() == QLatin1String("true")
                                : o.defaultValue.trimmed() == QLatin1String("true");
            item->setCheckState(1, on ? Qt::Checked : Qt::Unchecked);
            item->setText(1, set ? QString() : tr("(default)"));
        } else {
            item->setText(1, set ? set->rawValue : o.defaultValue);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
    }

    if (m_optionTree->topLevelItemCount() == 0) {
        auto *item = new QTreeWidgetItem(m_optionTree);
        item->setText(0, tr("None of your modules declare options with lib.mkOption."));
        item->setFlags(Qt::NoItemFlags);
    }

    m_updating = false;
}

void HostsPage::onOptionItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_updating)
        return;
    const QString path = item->data(0, kRoleOptionPath).toString();
    if (path.isEmpty())
        return;

    const HostInfo *host = m_ctx.project->host(currentHost());
    if (!host)
        return;
    NixFile *file = m_ctx.project->file(host->entryFile);
    if (!file)
        return;

    const bool isBool = item->data(0, kRoleOptionIsBool).toBool();
    if (isBool && column == 1) {
        const bool on = item->checkState(1) == Qt::Checked;
        file->setAttribute(path, on ? QStringLiteral("true") : QStringLiteral("false"));
        emit statusMessage(tr("Set %1 = %2").arg(path, on ? QStringLiteral("true")
                                                          : QStringLiteral("false")));
        emit configModified();
    } else if (!isBool && column == 1) {
        const QString value = item->text(1).trimmed();
        if (value.isEmpty())
            return;
        file->setAttribute(path, value);
        emit statusMessage(tr("Set %1 = %2").arg(path, value));
        emit configModified();
    }
    QMetaObject::invokeMethod(this, [this] { reloadOptions(); }, Qt::QueuedConnection);
}

void HostsPage::reloadUsers()
{
    const HostInfo *host = m_ctx.project->host(currentHost());
    if (!host)
        return;
    NixFile *file = m_ctx.project->file(host->entryFile);
    if (!file)
        return;

    m_updating = true;
    m_userTree->clear();

    QStringList names;
    for (const AttrEntry &a : file->attrs()) {
        if (!a.path.startsWith(QLatin1String("users.users.")))
            continue;
        const QString user = a.path.section(QLatin1Char('.'), 2, 2);
        if (!user.isEmpty() && !names.contains(user))
            names << user;
    }

    for (const QString &user : std::as_const(names)) {
        auto *item = new QTreeWidgetItem(m_userTree);
        item->setText(0, user);
        if (const AttrEntry *d
            = file->findAttr(QStringLiteral("users.users.%1.description").arg(user)))
            item->setText(1, d->unquoted());
        if (const NixList *g
            = file->findList(QStringLiteral("users.users.%1.extraGroups").arg(user))) {
            QStringList groups;
            for (const PackageEntry &e : g->entries)
                groups << e.expr.mid(1, e.expr.size() - 2);
            item->setText(2, groups.join(QStringLiteral(", ")));
        }
    }

    m_updating = false;
}

void HostsPage::commitIdentity()
{
    if (m_updating)
        return;
    const HostInfo *host = m_ctx.project->host(currentHost());
    if (!host)
        return;
    NixFile *file = m_ctx.project->file(host->entryFile);
    if (!file)
        return;

    bool changed = false;

    const AttrEntry *name = file->findAttr(QStringLiteral("networking.hostName"));
    const QString newName = m_hostName->text().trimmed();
    if (!newName.isEmpty() && (!name || name->unquoted() != newName)) {
        file->setAttribute(QStringLiteral("networking.hostName"), NixFile::quoteNixString(newName));
        changed = true;
    }

    const AttrEntry *sv = file->findAttr(QStringLiteral("system.stateVersion"));
    const QString newSv = m_stateVersion->text().trimmed();
    if (!newSv.isEmpty() && (!sv || sv->unquoted() != newSv)) {
        file->setAttribute(QStringLiteral("system.stateVersion"), NixFile::quoteNixString(newSv));
        changed = true;
    }

    if (changed) {
        emit statusMessage(tr("Updated identity of %1").arg(host->name));
        emit configModified();
    }
}

void HostsPage::createHost()
{
    ConfigProject *project = m_ctx.project;
    if (!project || !project->isOpen())
        return;

    const bool isFlake = project->kind() == ConfigProject::Flake;
    if (!isFlake) {
        QMessageBox::information(this, tr("New host"),
            tr("Adding hosts only makes sense for a flake with several "
               "nixosConfigurations. This configuration is a single "
               "configuration.nix."));
        return;
    }

    // ── Dialog ───────────────────────────────────────────────────────────────
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New host"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *name = new QLineEdit(&dialog);
    name->setPlaceholderText(QStringLiteral("laptop"));
    form->addRow(tr("Directory / attribute name:"), name);

    auto *hostName = new QLineEdit(&dialog);
    hostName->setPlaceholderText(tr("defaults to the name above"));
    form->addRow(tr("networking.hostName:"), hostName);

    auto *stateVersion = new QLineEdit(&dialog);
    // Copy the state version the rest of the fleet uses.
    for (const HostInfo &h : project->hosts()) {
        if (NixFile *f = project->file(h.entryFile)) {
            if (const AttrEntry *sv = f->findAttr(QStringLiteral("system.stateVersion"))) {
                stateVersion->setText(sv->unquoted());
                break;
            }
        }
    }
    form->addRow(tr("system.stateVersion:"), stateVersion);

    auto *copyFrom = new QComboBox(&dialog);
    copyFrom->addItem(tr("Nothing — start empty"), QString());
    for (const HostInfo &h : project->hosts())
        copyFrom->addItem(tr("Copy imports from %1").arg(h.name), h.name);
    form->addRow(tr("Modules:"), copyFrom);
    layout->addLayout(form);

    auto *registerInFlake = new QCheckBox(tr("Register in flake.nix"), &dialog);
    registerInFlake->setChecked(true);
    layout->addWidget(registerInFlake);

    auto *hardwareStub = new QCheckBox(tr("Write a hardware-configuration.nix placeholder"),
        &dialog);
    hardwareStub->setChecked(true);
    hardwareStub->setToolTip(
        tr("The real file has to come from nixos-generate-config on the target machine. The "
           "placeholder keeps the tree evaluable until you replace it."));
    layout->addWidget(hardwareStub);

    auto *buttons
        = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    // ── Validate ─────────────────────────────────────────────────────────────
    const QString hostDirName = name->text().trimmed();
    if (hostDirName.isEmpty()) {
        QMessageBox::warning(this, tr("New host"), tr("Please give the host a name."));
        return;
    }
    if (project->host(hostDirName)) {
        QMessageBox::warning(this, tr("New host"),
            tr("A host called %1 already exists.").arg(hostDirName));
        return;
    }

    QDir root(project->root());
    const QString relDir = QStringLiteral("hosts/") + hostDirName;
    if (QFileInfo::exists(root.absoluteFilePath(relDir))) {
        QMessageBox::warning(this, tr("New host"), tr("%1 already exists.").arg(relDir));
        return;
    }
    if (!root.mkpath(relDir)) {
        QMessageBox::warning(this, tr("New host"),
            tr("Could not create %1. Is the configuration tree writable?").arg(relDir));
        return;
    }

    const QString hostDir = root.absoluteFilePath(relDir);
    const QString entryFile = QDir(hostDir).absoluteFilePath(QStringLiteral("default.nix"));

    // ── default.nix ──────────────────────────────────────────────────────────
    QString imports = QStringLiteral("        ./hardware-configuration.nix\n");
    const QString copySource = copyFrom->currentData().toString();
    if (!copySource.isEmpty()) {
        if (const HostInfo *source = project->host(copySource)) {
            if (NixFile *sourceFile = project->file(source->entryFile)) {
                QString section;
                for (const ImportEntry &e : sourceFile->imports()) {
                    // The source host's own hardware file is not shared.
                    if (e.text.endsWith(QLatin1String("hardware-configuration.nix")))
                        continue;
                    if (e.section != section) {
                        section = e.section;
                        if (!section.isEmpty()) {
                            QString header = QStringLiteral("        # ── %1 ").arg(section);
                            while (header.size() < 83)
                                header += QChar(0x2500);
                            imports += QLatin1Char('\n') + header + QLatin1Char('\n');
                        }
                    }
                    imports += QStringLiteral("        %1%2\n")
                                   .arg(e.enabled ? QString() : QStringLiteral("#"), e.text);
                }
            }
        }
    }

    const QString effectiveHostName
        = hostName->text().trimmed().isEmpty() ? hostDirName : hostName->text().trimmed();
    const QString version
        = stateVersion->text().trimmed().isEmpty() ? QStringLiteral("24.05")
                                                   : stateVersion->text().trimmed();

    // A copied import like `inputs.foo.nixosModules.default` only resolves if
    // the module actually takes `inputs`.
    const QString formals = imports.contains(QLatin1String("inputs."))
        ? QStringLiteral("{ pkgs, inputs, ... }:")
        : QStringLiteral("{ pkgs, ... }:");

    const QString body = QStringLiteral("%0\n"
                                        "\n"
                                        "{\n"
                                        "    imports = [\n%1    ];\n"
                                        "\n"
                                        "    # ── Identity ──────────────────────"
                                        "───────────────────────────────────────────\n"
                                        "    networking.hostName = %2;\n"
                                        "\n"
                                        "    system.stateVersion = %3;\n"
                                        "}\n")
                             .arg(formals, imports, NixFile::quoteNixString(effectiveHostName),
                                 NixFile::quoteNixString(version));

    QFile out(entryFile);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("New host"),
            tr("Could not write %1: %2").arg(entryFile, out.errorString()));
        return;
    }
    QTextStream(&out) << body;
    out.close();

    // ── hardware-configuration.nix placeholder ───────────────────────────────
    if (hardwareStub->isChecked()) {
        QFile hardware(QDir(hostDir).absoluteFilePath(QStringLiteral("hardware-configuration.nix")));
        if (hardware.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(&hardware)
                << QStringLiteral(
                       "# PLACEHOLDER — replace this with the real thing.\n"
                       "#\n"
                       "# On the target machine run:\n"
                       "#     sudo nixos-generate-config --show-hardware-config \\\n"
                       "#         > hosts/%1/hardware-configuration.nix\n"
                       "#\n"
                       "# Until then this host will not boot: it declares no file systems and\n"
                       "# no boot device.\n"
                       "{ ... }:\n"
                       "\n"
                       "{\n"
                       "}\n")
                       .arg(hostDirName);
            hardware.close();
        }
    }

    // ── flake.nix registration ───────────────────────────────────────────────
    bool registered = false;
    if (registerInFlake->isChecked()) {
        if (NixFile *flake = project->file(project->flakeFile())) {
            // Reuse whatever the neighbouring entries do: `mkHost ./hosts/x`,
            // or a direct nixosSystem call if that is the house style.
            QString value = QStringLiteral("mkHost ./hosts/%1").arg(hostDirName);
            for (const AttrEntry &a : flake->attrs()) {
                if (!a.path.endsWith(QStringLiteral("nixosConfigurations.") + a.path.section(
                        QLatin1Char('.'), -1)))
                    continue;
                if (!a.path.contains(QLatin1String("nixosConfigurations.")))
                    continue;
                const QString existing = a.rawValue.simplified();
                const int slash = existing.indexOf(QLatin1String("./hosts/"));
                if (slash > 0) {
                    value = existing.left(slash) + QStringLiteral("./hosts/") + hostDirName;
                    break;
                }
            }

            QString setPath;
            for (const NixAttrSet &set : flake->attrSets()) {
                if (set.path == QLatin1String("nixosConfigurations")
                    || set.path.endsWith(QLatin1String(".nixosConfigurations"))) {
                    setPath = set.path;
                    break;
                }
            }
            if (!setPath.isEmpty())
                registered = flake->addToAttrSet(setPath, hostDirName, value);
        }
    }

    project->rescan();
    refresh();
    selectHost(hostDirName);

    if (registerInFlake->isChecked() && !registered) {
        QMessageBox::warning(this, tr("New host"),
            tr("%1 was created, but the nixosConfigurations set in flake.nix could not be "
               "found, so the host is not registered yet. Add it by hand in the Editor tab.")
                .arg(relDir));
    }

    emit statusMessage(tr("Created host %1").arg(hostDirName));
    emit configModified();
    emit openFileRequested(entryFile);
}

void HostsPage::applyTheme()
{
    m_newHost->setIcon(Theme::icon(QStringLiteral("add"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_newHost);
    m_rebuild->setIcon(Theme::icon(QStringLiteral("run"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_rebuild);
    m_openHostFile->setIcon(Theme::icon(QStringLiteral("editor")));
    reloadHostDetails();
}

} // namespace nixm
