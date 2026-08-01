#include "ModulesPage.h"

#include "Theme.h"
#include "core/ConfigProject.h"
#include "core/NixFile.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace nixm {
namespace {

constexpr int kRoleAbsPath = Qt::UserRole + 1;

struct Template {
    QString name;
    QString body;
};

QVector<Template> templates()
{
    return {
        { QObject::tr("Empty module"), QStringLiteral("{ ... }:\n\n{\n\n}\n") },
        { QObject::tr("Package list"),
            QStringLiteral("{ pkgs, ... }:\n\n{\n    environment.systemPackages = with pkgs; [\n"
                           "\n    ];\n}\n") },
        { QObject::tr("Service"),
            QStringLiteral("{ pkgs, ... }:\n\n{\n    # services.example.enable = true;\n\n"
                           "    environment.systemPackages = with pkgs; [\n\n    ];\n}\n") },
        { QObject::tr("Option-providing module"),
            QStringLiteral(
                "{ config, lib, pkgs, ... }:\n\n{\n    options.nixos.feature.example = {\n"
                "        enable = lib.mkOption {\n            type = lib.types.bool;\n"
                "            default = false;\n            description = \"Enable example.\";\n"
                "        };\n    };\n\n    config = lib.mkIf config.nixos.feature.example.enable {\n"
                "        environment.systemPackages = with pkgs; [\n\n        ];\n    };\n}\n") },
    };
}

} // namespace

ModulesPage::ModulesPage(const AppContext &ctx, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    buildUi();
}

void ModulesPage::buildUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(6, 6, 6, 6);

    m_filter = new QLineEdit(left);
    m_filter->setPlaceholderText(tr("Filter modules…"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &ModulesPage::applyFilter);
    leftLayout->addWidget(m_filter);

    m_tree = new QTreeWidget(left);
    m_tree->setHeaderLabels({ tr("Module"), tr("Used by") });
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setStretchLastSection(false);
    m_tree->setColumnWidth(1, 70);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &ModulesPage::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        const QString path = item->data(0, kRoleAbsPath).toString();
        if (!path.isEmpty())
            emit openFileRequested(path);
    });
    leftLayout->addWidget(m_tree, 1);

    auto *buttons = new QHBoxLayout;
    auto *create = new QPushButton(tr("New module…"), left);
    create->setIcon(Theme::icon(QStringLiteral("add"), Theme::colors().textOnBrand));
    Theme::makePrimary(create);
    connect(create, &QPushButton::clicked, this, &ModulesPage::createModule);
    buttons->addWidget(create);
    buttons->addStretch(1);
    leftLayout->addLayout(buttons);

    splitter->addWidget(left);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 8, 8, 8);

    auto *header = new QHBoxLayout;
    m_title = new QLabel(right);
    m_title->setTextFormat(Qt::RichText);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(m_title, 1);
    m_open = new QPushButton(tr("Open in editor"), right);
    m_open->setIcon(Theme::icon(QStringLiteral("editor")));
    m_open->setEnabled(false);
    connect(m_open, &QPushButton::clicked, this, [this] {
        auto *item = m_tree->currentItem();
        if (!item)
            return;
        const QString path = item->data(0, kRoleAbsPath).toString();
        if (!path.isEmpty())
            emit openFileRequested(path);
    });
    header->addWidget(m_open);
    rightLayout->addLayout(header);

    m_details = new QTextBrowser(right);
    m_details->setOpenExternalLinks(true);
    m_details->document()->setDefaultStyleSheet(Theme::richTextCss());
    rightLayout->addWidget(m_details, 1);

    splitter->addWidget(right);
    splitter->setSizes({ 320, 700 });
}

void ModulesPage::refresh()
{
    const QString previous
        = m_tree->currentItem() ? m_tree->currentItem()->data(0, kRoleAbsPath).toString() : QString();

    m_tree->clear();
    if (!m_ctx.project || !m_ctx.project->isOpen())
        return;

    QHash<QString, QTreeWidgetItem *> categories;
    QTreeWidgetItem *toSelect = nullptr;
    for (const ModuleInfo &m : m_ctx.project->modules()) {
        QTreeWidgetItem *parent = categories.value(m.category);
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree, { m.category });
            QFont f = parent->font(0);
            f.setBold(true);
            parent->setFont(0, f);
            parent->setExpanded(true);
            categories.insert(m.category, parent);
        }
        auto *item = new QTreeWidgetItem(parent, { m.name });
        item->setData(0, kRoleAbsPath, m.absPath);
        const int uses = hostsUsing(m.absPath).size();
        item->setText(1, uses > 0 ? QString::number(uses) : QStringLiteral("–"));
        if (uses == 0)
            item->setForeground(0, Theme::colors().textMuted);
        if (m.absPath == previous)
            toSelect = item;
    }

    applyFilter(m_filter->text());
    if (toSelect)
        m_tree->setCurrentItem(toSelect);
    else
        onSelectionChanged();
}

void ModulesPage::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *category = m_tree->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < category->childCount(); ++j) {
            QTreeWidgetItem *child = category->child(j);
            const bool matches = needle.isEmpty()
                || child->text(0).contains(needle, Qt::CaseInsensitive)
                || category->text(0).contains(needle, Qt::CaseInsensitive);
            child->setHidden(!matches);
            if (matches)
                ++visible;
        }
        category->setHidden(visible == 0);
    }
}

QStringList ModulesPage::hostsUsing(const QString &absModulePath) const
{
    QStringList out;
    if (!m_ctx.project)
        return out;
    if (m_ctx.project->isGlobalModule(absModulePath)) {
        for (const HostInfo &h : m_ctx.project->hosts())
            out << h.name;
        return out;
    }
    for (const HostInfo &h : m_ctx.project->hosts()) {
        NixFile *f = m_ctx.project->file(h.entryFile);
        if (!f)
            continue;
        for (const ImportEntry &e : f->imports()) {
            if (!e.enabled)
                continue;
            if (QFileInfo(ConfigProject::resolveNixPath(h.entryFile, e.text)).absoluteFilePath()
                == absModulePath) {
                out << h.name;
                break;
            }
        }
    }
    return out;
}

void ModulesPage::onSelectionChanged()
{
    auto *item = m_tree->currentItem();
    const QString path = item ? item->data(0, kRoleAbsPath).toString() : QString();
    m_open->setEnabled(!path.isEmpty());

    if (path.isEmpty()) {
        m_title->clear();
        m_details->clear();
        return;
    }

    NixFile *file = m_ctx.project->file(path);
    if (!file) {
        m_title->setText(tr("<b>%1</b>").arg(QFileInfo(path).fileName()));
        m_details->setPlainText(tr("Could not read %1").arg(path));
        return;
    }

    const QString rel = QDir(m_ctx.project->root()).relativeFilePath(path);
    const ThemeColors &tc = Theme::colors();
    m_title->setText(
        QStringLiteral("<div style='font-size:13pt;font-weight:800;color:%1;'>%2</div>"
                       "<div style='font-size:8.5pt;color:%3;'>%4</div>")
            .arg(tc.dark ? tc.lightBlue.name() : tc.primary.name(),
                QFileInfo(path).completeBaseName().toHtmlEscaped(), tc.textMuted.name(),
                rel.toHtmlEscaped()));

    QString html;
    const QStringList users = hostsUsing(path);
    const QString usedBy = m_ctx.project->isGlobalModule(path)
        ? tr("every host — added by <code>flake.nix</code>")
        : (users.isEmpty() ? tr("<i>no host imports this module</i>")
                           : users.join(QStringLiteral(", ")).toHtmlEscaped());
    html += QStringLiteral("<p><b>%1</b> %2</p>").arg(tr("Used by:"), usedBy);

    const auto lists = file->packageLists();
    if (!lists.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("Packages"));
        for (const NixList *l : lists) {
            html += QStringLiteral("<li><code>%1</code>").arg(l->path.toHtmlEscaped());
            if (!l->withExpr.isEmpty())
                html += QStringLiteral(" <small>(with %1)</small>").arg(l->withExpr.toHtmlEscaped());
            html += QStringLiteral("<ul>");
            for (const PackageEntry &e : l->entries) {
                html += QStringLiteral("<li>%1<code>%2</code>%3%4</li>")
                            .arg(e.enabled ? QString() : QStringLiteral("<s>"),
                                e.expr.toHtmlEscaped(),
                                e.enabled ? QString() : QStringLiteral("</s>"),
                                e.comment.isEmpty()
                                    ? QString()
                                    : QStringLiteral(" — <i>%1</i>").arg(e.comment.toHtmlEscaped()));
            }
            html += QStringLiteral("</ul></li>");
        }
        html += QStringLiteral("</ul>");
    }

    QStringList settings;
    for (const AttrEntry &a : file->attrs()) {
        // The wrapper bindings (`config = lib.mkIf … { … }`) and the innards of
        // an option declaration are noise; both are reported separately below.
        if (a.path == QLatin1String("config") || a.path == QLatin1String("options"))
            continue;
        bool insideOptionDecl = false;
        for (const OptionDecl &o : file->optionDecls()) {
            if (a.path == o.path || a.path.startsWith(o.path + QLatin1Char('.'))) {
                insideOptionDecl = true;
                break;
            }
        }
        if (insideOptionDecl)
            continue;
        settings << QStringLiteral("<li><code>%1</code> = <code>%2</code></li>")
                        .arg(a.path.toHtmlEscaped(), a.rawValue.left(90).toHtmlEscaped());
    }
    if (!settings.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p><ul>%2</ul>")
                    .arg(tr("Settings"), settings.join(QString()));
    }

    if (!file->optionDecls().isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("Options declared"));
        for (const OptionDecl &o : file->optionDecls())
            html += QStringLiteral("<li><code>%1</code> — %2</li>")
                        .arg(o.path.toHtmlEscaped(), o.description.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }

    if (!file->imports().isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p><ul>").arg(tr("Imports"));
        for (const ImportEntry &e : file->imports())
            html += QStringLiteral("<li><code>%1</code>%2</li>")
                        .arg(e.text.toHtmlEscaped(),
                            e.enabled ? QString() : QStringLiteral(" <i>(commented out)</i>"));
        html += QStringLiteral("</ul>");
    }

    m_details->setHtml(html);
}

void ModulesPage::createModule()
{
    if (!m_ctx.project || !m_ctx.project->isOpen())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("New module"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *name = new QLineEdit(&dialog);
    name->setPlaceholderText(QStringLiteral("my-module"));
    form->addRow(tr("Name:"), name);

    auto *category = new QComboBox(&dialog);
    category->setEditable(true);
    QStringList categories;
    for (const ModuleInfo &m : m_ctx.project->modules())
        if (!categories.contains(m.category))
            categories << m.category;
    categories.sort();
    category->addItems(categories);
    form->addRow(tr("Category:"), category);

    auto *tmpl = new QComboBox(&dialog);
    const auto tpls = templates();
    for (const Template &t : tpls)
        tmpl->addItem(t.name);
    form->addRow(tr("Template:"), tmpl);

    auto *preview = new QLabel(&dialog);
    preview->setEnabled(false);
    form->addRow(tr("Creates:"), preview);

    auto updatePreview = [&] {
        const QString cat = category->currentText().trimmed();
        const QString base = cat.isEmpty() ? QStringLiteral("modules")
                                           : QStringLiteral("modules/") + cat;
        preview->setText(QStringLiteral("%1/%2.nix")
                             .arg(base, name->text().trimmed().isEmpty()
                                     ? QStringLiteral("…")
                                     : name->text().trimmed()));
    };
    connect(name, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(category, &QComboBox::currentTextChanged, &dialog, updatePreview);
    updatePreview();

    layout->addLayout(form);
    auto *buttons
        = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString moduleName = name->text().trimmed();
    if (moduleName.isEmpty()) {
        QMessageBox::warning(this, tr("New module"), tr("Please give the module a name."));
        return;
    }

    const QString cat = category->currentText().trimmed();
    const QString relDir
        = cat.isEmpty() ? QStringLiteral("modules") : QStringLiteral("modules/") + cat;
    QDir root(m_ctx.project->root());
    if (!root.mkpath(relDir)) {
        QMessageBox::warning(this, tr("New module"),
            tr("Could not create %1. Is the configuration tree writable?").arg(relDir));
        return;
    }

    const QString absPath
        = root.absoluteFilePath(relDir + QLatin1Char('/') + moduleName + QStringLiteral(".nix"));
    if (QFileInfo::exists(absPath)) {
        QMessageBox::warning(this, tr("New module"), tr("%1 already exists.").arg(absPath));
        return;
    }

    QFile f(absPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("New module"),
            tr("Could not write %1: %2").arg(absPath, f.errorString()));
        return;
    }
    QTextStream(&f) << tpls.at(tmpl->currentIndex()).body;
    f.close();

    m_ctx.project->rescan();
    emit statusMessage(tr("Created %1").arg(QDir(m_ctx.project->root()).relativeFilePath(absPath)));
    emit configModified();
    emit openFileRequested(absPath);
}

void ModulesPage::applyTheme()
{
    m_details->document()->setDefaultStyleSheet(Theme::richTextCss());
    m_open->setIcon(Theme::icon(QStringLiteral("editor")));
    refresh();
}

} // namespace nixm
