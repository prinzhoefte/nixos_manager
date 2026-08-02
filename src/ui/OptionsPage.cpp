#include "OptionsPage.h"

#include "Theme.h"
#include "core/ConfigProject.h"
#include "core/NixFile.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace nixm {
namespace {

constexpr int kRoleAbsPath = Qt::UserRole + 1;
constexpr int kRoleOptionPath = Qt::UserRole + 2;

/// Options the structured pages already own, or that are noise in this view.
bool isHiddenFromOptionList(const QString &path)
{
    static const QStringList prefixes = { QStringLiteral("nixosConfigurations."),
        QStringLiteral("inputs."), QStringLiteral("outputs."), QStringLiteral("description"),
        QStringLiteral("specialArgs") };
    for (const QString &prefix : prefixes) {
        if (path == prefix || path.startsWith(prefix))
            return true;
    }
    return false;
}

/// Best-effort Nix literal for a value the user typed. Anything that already
/// looks like Nix is passed straight through.
QString coerceToNix(const QString &input)
{
    const QString v = input.trimmed();
    if (v.isEmpty())
        return QStringLiteral("\"\"");
    if (v == QLatin1String("true") || v == QLatin1String("false") || v == QLatin1String("null"))
        return v;

    bool isNumber = false;
    v.toDouble(&isNumber);
    if (isNumber)
        return v;

    // Already a string, list, attrset, path, function call or interpolation.
    const QChar first = v.at(0);
    if (first == QLatin1Char('"') || first == QLatin1Char('[') || first == QLatin1Char('{')
        || first == QLatin1Char('(') || first == QLatin1Char('.') || first == QLatin1Char('/')
        || v.startsWith(QLatin1String("''")))
        return v;
    if (v.contains(QLatin1Char(' ')) || v.contains(QLatin1Char('.')))
        return v;   // `pkgs.foo`, `lib.mkForce 1`, …

    return NixFile::quoteNixString(v);
}

} // namespace

OptionsPage::OptionsPage(const AppContext &ctx, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    buildUi();

    connect(m_ctx.search, &PackageSearch::optionResultsReady, this, &OptionsPage::onResults);
    connect(m_ctx.search, &PackageSearch::failed, this,
        [this](const QString &message) { m_searchStatus->setText(message); });
}

void OptionsPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    // ── Search side ──────────────────────────────────────────────────────────
    auto *searchSide = new QWidget(splitter);
    auto *searchLayout = new QVBoxLayout(searchSide);
    searchLayout->setContentsMargins(6, 6, 6, 6);

    auto *queryRow = new QHBoxLayout;
    m_query = new QLineEdit(searchSide);
    m_query->setPlaceholderText(tr("Search NixOS options — services.openssh, hostName, …"));
    m_query->setClearButtonEnabled(true);
    connect(m_query, &QLineEdit::returnPressed, this, &OptionsPage::runSearch);
    queryRow->addWidget(m_query, 1);

    m_searchButton = new QPushButton(tr("Search"), searchSide);
    m_searchButton->setIcon(Theme::icon(QStringLiteral("search"), Theme::colors().textOnBrand));
    Theme::makePrimary(m_searchButton);
    connect(m_searchButton, &QPushButton::clicked, this, &OptionsPage::runSearch);
    queryRow->addWidget(m_searchButton);
    searchLayout->addLayout(queryRow);

    m_searchStatus = new QLabel(searchSide);
    m_searchStatus->setWordWrap(true);
    m_searchStatus->setEnabled(false);
    searchLayout->addWidget(m_searchStatus);

    m_resultTable = new QTableWidget(searchSide);
    m_resultTable->setColumnCount(3);
    m_resultTable->setHorizontalHeaderLabels({ tr("Option"), tr("Type"), tr("Default") });
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->verticalHeader()->setVisible(false);
    m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    // Type strings can be paragraph-long ("list of (optionally null) …"), so
    // give them a fixed slice and let the cell elide.
    m_resultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_resultTable->horizontalHeader()->resizeSection(1, 150);
    m_resultTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_resultTable->horizontalHeader()->resizeSection(2, 120);
    m_resultTable->setTextElideMode(Qt::ElideRight);
    m_resultTable->setWordWrap(false);
    m_resultTable->setAlternatingRowColors(true);
    connect(m_resultTable, &QTableWidget::itemSelectionChanged, this,
        &OptionsPage::onResultSelected);
    connect(m_resultTable, &QTableWidget::itemDoubleClicked, this,
        [this](QTableWidgetItem *) { applySelectedOption(); });
    searchLayout->addWidget(m_resultTable, 1);

    m_details = new QTextBrowser(searchSide);
    m_details->setOpenExternalLinks(true);
    m_details->document()->setDefaultStyleSheet(Theme::richTextCss());
    m_details->setMaximumHeight(160);
    m_details->setPlaceholderText(tr("Select an option to see its type, default and description."));
    searchLayout->addWidget(m_details);

    auto *form = new QFormLayout;
    m_targetBox = new QComboBox(searchSide);
    m_targetBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    form->addRow(tr("Set in:"), m_targetBox);

    m_valueEdit = new QLineEdit(searchSide);
    m_valueEdit->setPlaceholderText(tr("true · \"de\" · [ \"wheel\" ] · pkgs.foo"));
    connect(m_valueEdit, &QLineEdit::returnPressed, this, &OptionsPage::applySelectedOption);
    form->addRow(tr("Value:"), m_valueEdit);
    searchLayout->addLayout(form);

    auto *applyRow = new QHBoxLayout;
    m_applyButton = new QPushButton(tr("Set option"), searchSide);
    m_applyButton->setIcon(Theme::icon(QStringLiteral("add"), Theme::colors().textOnBrand));
    m_applyButton->setEnabled(false);
    Theme::makePrimary(m_applyButton);
    connect(m_applyButton, &QPushButton::clicked, this, &OptionsPage::applySelectedOption);
    applyRow->addWidget(m_applyButton);

    m_manualButton = new QPushButton(tr("Add by name…"), searchSide);
    m_manualButton->setToolTip(
        tr("Set an option the index does not know about, such as one your own modules declare."));
    connect(m_manualButton, &QPushButton::clicked, this, &OptionsPage::addManually);
    applyRow->addWidget(m_manualButton);
    applyRow->addStretch(1);
    searchLayout->addLayout(applyRow);

    splitter->addWidget(searchSide);

    // ── "Set in this configuration" side ─────────────────────────────────────
    auto *setSide = new QWidget(splitter);
    auto *setLayout = new QVBoxLayout(setSide);
    setLayout->setContentsMargins(6, 6, 6, 6);

    auto *caption = new QLabel(tr("Options set in this configuration"), setSide);
    Theme::makeEyebrow(caption);
    setLayout->addWidget(caption);

    m_filter = new QLineEdit(setSide);
    m_filter->setPlaceholderText(tr("Filter…"));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &OptionsPage::applyFilter);
    setLayout->addWidget(m_filter);

    m_setTree = new QTreeWidget(setSide);
    m_setTree->setColumnCount(2);
    m_setTree->setHeaderLabels({ tr("Option"), tr("Value") });
    m_setTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_setTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_setTree->header()->resizeSection(1, 200);
    m_setTree->header()->setStretchLastSection(false);
    m_setTree->setAlternatingRowColors(true);
    m_setTree->setEditTriggers(QAbstractItemView::DoubleClicked
        | QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
    connect(m_setTree, &QTreeWidget::itemChanged, this, &OptionsPage::onSetItemChanged);
    connect(m_setTree, &QTreeWidget::currentItemChanged, this, [this] {
        auto *item = m_setTree->currentItem();
        const bool isOption = item && !item->data(0, kRoleOptionPath).toString().isEmpty();
        m_editButton->setEnabled(isOption);
        m_removeButton->setEnabled(isOption);
    });
    setLayout->addWidget(m_setTree, 1);

    auto *hint = new QLabel(
        tr("Double-click a value to edit it in place. Options are written back exactly where "
           "they already live in the file."),
        setSide);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    setLayout->addWidget(hint);

    auto *buttons = new QHBoxLayout;
    m_editButton = new QPushButton(tr("Edit value…"), setSide);
    m_editButton->setIcon(Theme::icon(QStringLiteral("editor")));
    m_editButton->setEnabled(false);
    connect(m_editButton, &QPushButton::clicked, this, &OptionsPage::editSelected);
    buttons->addWidget(m_editButton);

    m_removeButton = new QPushButton(tr("Remove"), setSide);
    m_removeButton->setIcon(Theme::icon(QStringLiteral("trash")));
    m_removeButton->setProperty("danger", true);
    m_removeButton->setEnabled(false);
    connect(m_removeButton, &QPushButton::clicked, this, &OptionsPage::removeSelected);
    buttons->addWidget(m_removeButton);
    buttons->addStretch(1);
    setLayout->addLayout(buttons);

    splitter->addWidget(setSide);
    splitter->setSizes({ 620, 440 });
}

void OptionsPage::refresh()
{
    reloadTargets();
    reloadSetOptions();
    if (m_searchStatus->text().isEmpty() && m_ctx.project && m_ctx.project->isOpen()) {
        m_searchStatus->setText(
            tr("Searching the %1 option index.").arg(m_ctx.project->nixpkgsChannel()));
    }
}

void OptionsPage::applyTheme()
{
    const ThemeColors &c = Theme::colors();
    m_details->document()->setDefaultStyleSheet(Theme::richTextCss());
    m_searchButton->setIcon(Theme::icon(QStringLiteral("search"), c.textOnBrand));
    m_applyButton->setIcon(Theme::icon(QStringLiteral("add"), c.textOnBrand));
    m_editButton->setIcon(Theme::icon(QStringLiteral("editor")));
    m_removeButton->setIcon(Theme::icon(QStringLiteral("trash")));
    Theme::makePrimary(m_searchButton);
    Theme::makePrimary(m_applyButton);
    onResultSelected();
    reloadSetOptions();
}

// ── Searching ────────────────────────────────────────────────────────────────

void OptionsPage::runSearch()
{
    const QString q = m_query->text().trimmed();
    if (q.isEmpty())
        return;
    m_searchStatus->setText(tr("Searching…"));
    m_ctx.search->searchOptions(q);
}

void OptionsPage::onResults(const QString &query, const QVector<OptionResult> &results,
    bool fromCache)
{
    m_results = results;
    m_resultTable->setRowCount(results.size());
    for (int row = 0; row < results.size(); ++row) {
        const OptionResult &r = results.at(row);
        m_resultTable->setItem(row, 0, new QTableWidgetItem(r.name));
        m_resultTable->setItem(row, 1, new QTableWidgetItem(r.type));
        m_resultTable->setItem(row, 2, new QTableWidgetItem(r.defaultValue));
    }
    m_searchStatus->setText(results.isEmpty()
            ? tr("No options matched “%1”.").arg(query)
            : tr("%n option(s) for “%1”%2", nullptr, int(results.size()))
                  .arg(query, fromCache ? tr(" (cached)") : QString()));
    if (!results.isEmpty())
        m_resultTable->selectRow(0);
    else
        m_details->clear();
}

const OptionResult *OptionsPage::selectedResult() const
{
    const int row = m_resultTable->currentRow();
    if (row < 0 || row >= m_results.size())
        return nullptr;
    return &m_results.at(row);
}

void OptionsPage::onResultSelected()
{
    const OptionResult *r = selectedResult();
    m_applyButton->setEnabled(r != nullptr && m_targetBox->count() > 0);
    if (!r) {
        m_details->clear();
        return;
    }

    QString html = QStringLiteral("<h3>%1</h3>").arg(r->name.toHtmlEscaped());
    if (!r->type.isEmpty())
        html += QStringLiteral("<p><b>%1</b> <code>%2</code></p>")
                    .arg(tr("Type:"), r->type.toHtmlEscaped());
    if (!r->description.isEmpty())
        html += QStringLiteral("<p>%1</p>").arg(r->description.left(900).toHtmlEscaped());
    if (!r->defaultValue.isEmpty())
        html += QStringLiteral("<p><b>%1</b> <code>%2</code></p>")
                    .arg(tr("Default:"), r->defaultValue.toHtmlEscaped());
    if (!r->example.isEmpty())
        html += QStringLiteral("<p><b>%1</b> <code>%2</code></p>")
                    .arg(tr("Example:"), r->example.toHtmlEscaped());
    if (!r->source.isEmpty())
        html += QStringLiteral("<p><small>%1 %2</small></p>")
                    .arg(tr("Declared in"), r->source.toHtmlEscaped());
    m_details->setHtml(html);

    // Pre-fill the value box with something sensible to edit.
    if (m_valueEdit->text().isEmpty() || m_valueEdit->property("auto").toBool()) {
        QString suggestion = r->example.isEmpty() ? r->defaultValue : r->example;
        if (r->type.contains(QLatin1String("bool")) && suggestion.isEmpty())
            suggestion = QStringLiteral("true");
        if (suggestion == QLatin1String("false"))
            suggestion = QStringLiteral("true");
        m_valueEdit->setText(suggestion);
        m_valueEdit->setProperty("auto", true);
    }
}

// ── Targets ──────────────────────────────────────────────────────────────────

void OptionsPage::reloadTargets()
{
    const QString remembered = QSettings().value(QStringLiteral("options/lastTarget")).toString();
    const QString current = m_targetBox->currentText();

    m_targetFiles.clear();
    m_targetBox->clear();
    if (!m_ctx.project || !m_ctx.project->isOpen())
        return;

    QDir root(m_ctx.project->root());
    for (const HostInfo &h : m_ctx.project->hosts()) {
        m_targetFiles << h.entryFile;
        m_targetBox->addItem(tr("host: %1  ·  %2").arg(h.name, root.relativeFilePath(h.entryFile)));
    }
    for (const ModuleInfo &m : m_ctx.project->modules()) {
        m_targetFiles << m.absPath;
        m_targetBox->addItem(m.relPath);
    }

    const QString wanted = current.isEmpty() ? remembered : current;
    const int index = m_targetBox->findText(wanted);
    if (index >= 0)
        m_targetBox->setCurrentIndex(index);

    m_applyButton->setEnabled(selectedResult() != nullptr && m_targetBox->count() > 0);
}

QString OptionsPage::targetFile() const
{
    const int index = m_targetBox->currentIndex();
    if (index < 0 || index >= m_targetFiles.size())
        return QString();
    return m_targetFiles.at(index);
}

// ── Applying ─────────────────────────────────────────────────────────────────

void OptionsPage::applySelectedOption()
{
    const OptionResult *r = selectedResult();
    if (!r)
        return;

    const QString path = targetFile();
    if (path.isEmpty()) {
        QMessageBox::information(this, tr("Set option"), tr("Choose a file to write to first."));
        return;
    }
    NixFile *file = m_ctx.project->file(path);
    if (!file)
        return;

    const QString value = coerceToNix(m_valueEdit->text());
    const bool existed = file->findAttr(r->name) != nullptr;
    if (!file->setAttribute(r->name, value)) {
        QMessageBox::warning(this, tr("Set option"),
            tr("Could not write %1 into %2.").arg(r->name, QFileInfo(path).fileName()));
        return;
    }

    QSettings().setValue(QStringLiteral("options/lastTarget"), m_targetBox->currentText());
    emit statusMessage(existed ? tr("Updated %1 = %2").arg(r->name, value)
                               : tr("Added %1 = %2").arg(r->name, value));
    emit configModified();
    reloadSetOptions();
}

void OptionsPage::addManually()
{
    const QString path = targetFile();
    if (path.isEmpty()) {
        QMessageBox::information(this, tr("Add option"), tr("Choose a file to write to first."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add option"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *name = new QLineEdit(&dialog);
    name->setPlaceholderText(QStringLiteral("services.example.enable"));
    form->addRow(tr("Option:"), name);

    auto *value = new QLineEdit(&dialog);
    value->setText(QStringLiteral("true"));
    form->addRow(tr("Value:"), value);

    auto *where = new QLabel(QDir(m_ctx.project->root()).relativeFilePath(path), &dialog);
    where->setEnabled(false);
    form->addRow(tr("File:"), where);
    layout->addLayout(form);

    auto *buttons
        = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString optionPath = name->text().trimmed();
    if (optionPath.isEmpty())
        return;

    NixFile *file = m_ctx.project->file(path);
    if (!file || !file->setAttribute(optionPath, coerceToNix(value->text()))) {
        QMessageBox::warning(this, tr("Add option"), tr("Could not write %1.").arg(optionPath));
        return;
    }

    emit statusMessage(tr("Added %1").arg(optionPath));
    emit configModified();
    reloadSetOptions();
}

// ── The options this configuration already sets ──────────────────────────────

void OptionsPage::reloadSetOptions()
{
    m_updating = true;
    m_setTree->clear();

    if (!m_ctx.project || !m_ctx.project->isOpen()) {
        m_updating = false;
        return;
    }

    QStringList files;
    for (const HostInfo &h : m_ctx.project->hosts())
        files << h.entryFile;
    for (const ModuleInfo &m : m_ctx.project->modules())
        files << m.absPath;

    QDir root(m_ctx.project->root());
    int total = 0;
    for (const QString &absPath : std::as_const(files)) {
        NixFile *file = m_ctx.project->file(absPath);
        if (!file)
            continue;

        // Values that belong to an option *declaration* are the module's own
        // API, not settings, so they are listed on the Modules page instead.
        QStringList declared;
        for (const OptionDecl &o : file->optionDecls())
            declared << o.path;

        QVector<const AttrEntry *> visible;
        for (const AttrEntry &a : file->attrs()) {
            if (isHiddenFromOptionList(a.path))
                continue;
            bool insideDecl = false;
            for (const QString &d : std::as_const(declared)) {
                if (a.path == d || a.path.startsWith(d + QLatin1Char('.'))) {
                    insideDecl = true;
                    break;
                }
            }
            if (insideDecl)
                continue;
            visible.push_back(&a);
        }
        if (visible.isEmpty())
            continue;

        auto *fileItem = new QTreeWidgetItem(m_setTree, { root.relativeFilePath(absPath) });
        QFont bold = fileItem->font(0);
        bold.setBold(true);
        fileItem->setFont(0, bold);
        fileItem->setData(0, kRoleAbsPath, absPath);
        fileItem->setExpanded(true);
        fileItem->setFlags(fileItem->flags() & ~Qt::ItemIsEditable);

        for (const AttrEntry *a : std::as_const(visible)) {
            auto *item = new QTreeWidgetItem(fileItem);
            item->setText(0, a->path);
            item->setText(1, a->rawValue.simplified().left(200));
            item->setData(0, kRoleAbsPath, absPath);
            item->setData(0, kRoleOptionPath, a->path);
            item->setToolTip(1, a->rawValue);
            // Only the value column is editable.
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            ++total;
        }
    }

    m_setTree->setHeaderLabels({ tr("Option (%1)").arg(total), tr("Value") });
    m_updating = false;
    applyFilter(m_filter->text());
}

void OptionsPage::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_setTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *fileItem = m_setTree->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < fileItem->childCount(); ++j) {
            QTreeWidgetItem *child = fileItem->child(j);
            const bool matches = needle.isEmpty()
                || child->text(0).contains(needle, Qt::CaseInsensitive)
                || child->text(1).contains(needle, Qt::CaseInsensitive)
                || fileItem->text(0).contains(needle, Qt::CaseInsensitive);
            child->setHidden(!matches);
            if (matches)
                ++visible;
        }
        fileItem->setHidden(visible == 0);
    }
}

void OptionsPage::onSetItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_updating || column != 1)
        return;
    const QString optionPath = item->data(0, kRoleOptionPath).toString();
    const QString absPath = item->data(0, kRoleAbsPath).toString();
    if (optionPath.isEmpty() || absPath.isEmpty())
        return;

    NixFile *file = m_ctx.project->file(absPath);
    if (!file)
        return;
    const AttrEntry *existing = file->findAttr(optionPath);
    const QString typed = item->text(1).trimmed();
    if (!existing || typed.isEmpty() || existing->rawValue.simplified() == typed)
        return;

    file->setAttribute(optionPath, coerceToNix(typed));
    emit statusMessage(tr("Set %1 = %2").arg(optionPath, typed));
    emit configModified();

    // We are inside itemChanged; rebuilding now would delete the live item.
    QMetaObject::invokeMethod(this, [this] { reloadSetOptions(); }, Qt::QueuedConnection);
}

void OptionsPage::editSelected()
{
    auto *item = m_setTree->currentItem();
    if (!item)
        return;
    const QString optionPath = item->data(0, kRoleOptionPath).toString();
    const QString absPath = item->data(0, kRoleAbsPath).toString();
    if (optionPath.isEmpty())
        return;

    NixFile *file = m_ctx.project->file(absPath);
    if (!file)
        return;
    const AttrEntry *existing = file->findAttr(optionPath);
    if (!existing)
        return;

    bool accepted = false;
    const QString value = QInputDialog::getText(this, tr("Edit option"),
        tr("Nix value for %1:").arg(optionPath), QLineEdit::Normal, existing->rawValue, &accepted);
    if (!accepted || value.trimmed().isEmpty())
        return;

    file->setAttribute(optionPath, value.trimmed());
    emit statusMessage(tr("Set %1 = %2").arg(optionPath, value.trimmed()));
    emit configModified();
    reloadSetOptions();
}

void OptionsPage::removeSelected()
{
    auto *item = m_setTree->currentItem();
    if (!item)
        return;
    const QString optionPath = item->data(0, kRoleOptionPath).toString();
    const QString absPath = item->data(0, kRoleAbsPath).toString();
    if (optionPath.isEmpty())
        return;

    const auto answer = QMessageBox::question(this, tr("Remove option"),
        tr("Delete %1 from %2?").arg(optionPath, QFileInfo(absPath).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    NixFile *file = m_ctx.project->file(absPath);
    if (!file || !file->removeAttribute(optionPath))
        return;

    emit statusMessage(tr("Removed %1").arg(optionPath));
    emit configModified();
    reloadSetOptions();
}

} // namespace nixm
