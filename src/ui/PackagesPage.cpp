#include "PackagesPage.h"

#include "core/ConfigProject.h"
#include "core/NixFile.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
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
constexpr int kRoleListPath = Qt::UserRole + 2;
constexpr int kRoleExpr = Qt::UserRole + 3;
constexpr int kRoleEnabled = Qt::UserRole + 4;

} // namespace

PackagesPage::PackagesPage(const AppContext &ctx, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    buildUi();

    connect(m_ctx.search, &PackageSearch::resultsReady, this, &PackagesPage::onResults);
    connect(m_ctx.search, &PackageSearch::failed, this, &PackagesPage::onSearchFailed);
    connect(m_ctx.search, &PackageSearch::busyChanged, this, [this](bool busy) {
        m_searchButton->setEnabled(!busy);
        if (busy)
            m_searchStatus->setText(tr("Searching…"));
    });
}

void PackagesPage::buildUi()
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
    m_query->setPlaceholderText(tr("Search nixpkgs — package name, binary or description…"));
    m_query->setClearButtonEnabled(true);
    connect(m_query, &QLineEdit::returnPressed, this, &PackagesPage::runSearch);
    queryRow->addWidget(m_query, 1);

    m_searchButton = new QPushButton(tr("Search"), searchSide);
    connect(m_searchButton, &QPushButton::clicked, this, &PackagesPage::runSearch);
    queryRow->addWidget(m_searchButton);
    searchLayout->addLayout(queryRow);

    m_searchStatus = new QLabel(searchSide);
    m_searchStatus->setWordWrap(true);
    m_searchStatus->setEnabled(false);
    searchLayout->addWidget(m_searchStatus);

    m_resultTable = new QTableWidget(searchSide);
    m_resultTable->setColumnCount(3);
    m_resultTable->setHorizontalHeaderLabels({ tr("Attribute"), tr("Version"), tr("Description") });
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultTable->verticalHeader()->setVisible(false);
    m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_resultTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_resultTable->horizontalHeader()->setStretchLastSection(true);
    m_resultTable->setAlternatingRowColors(true);
    connect(m_resultTable, &QTableWidget::itemSelectionChanged, this,
        &PackagesPage::onResultSelected);
    connect(m_resultTable, &QTableWidget::itemDoubleClicked, this,
        [this](QTableWidgetItem *) { addSelectedPackage(); });
    searchLayout->addWidget(m_resultTable, 1);

    m_details = new QTextBrowser(searchSide);
    m_details->setOpenExternalLinks(true);
    m_details->setMaximumHeight(170);
    searchLayout->addWidget(m_details);

    auto *addRow = new QHBoxLayout;
    addRow->addWidget(new QLabel(tr("Add to:"), searchSide));
    m_targetBox = new QComboBox(searchSide);
    m_targetBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    addRow->addWidget(m_targetBox, 1);
    m_addButton = new QPushButton(tr("Add package"), searchSide);
    m_addButton->setEnabled(false);
    connect(m_addButton, &QPushButton::clicked, this, &PackagesPage::addSelectedPackage);
    addRow->addWidget(m_addButton);
    searchLayout->addLayout(addRow);

    splitter->addWidget(searchSide);

    // ── Installed side ───────────────────────────────────────────────────────
    auto *installedSide = new QWidget(splitter);
    auto *instLayout = new QVBoxLayout(installedSide);
    instLayout->setContentsMargins(6, 6, 6, 6);
    instLayout->addWidget(new QLabel(tr("<b>Packages in this configuration</b>"), installedSide));

    m_installedFilter = new QLineEdit(installedSide);
    m_installedFilter->setPlaceholderText(tr("Filter…"));
    m_installedFilter->setClearButtonEnabled(true);
    connect(m_installedFilter, &QLineEdit::textChanged, this,
        &PackagesPage::applyInstalledFilter);
    instLayout->addWidget(m_installedFilter);

    m_installed = new QTreeWidget(installedSide);
    m_installed->setColumnCount(2);
    m_installed->setHeaderLabels({ tr("Package"), tr("Note") });
    m_installed->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_installed->header()->setStretchLastSection(true);
    m_installed->setAlternatingRowColors(true);
    connect(m_installed, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        const QString path = item->data(0, kRoleAbsPath).toString();
        if (!path.isEmpty() && !item->data(0, kRoleExpr).toString().isEmpty())
            emit openFileRequested(path);
    });
    connect(m_installed, &QTreeWidget::currentItemChanged, this, [this] {
        auto *item = m_installed->currentItem();
        const bool isPackage = item && !item->data(0, kRoleExpr).toString().isEmpty();
        m_removeButton->setEnabled(isPackage);
        m_toggleButton->setEnabled(isPackage);
        if (isPackage)
            m_toggleButton->setText(item->data(0, kRoleEnabled).toBool() ? tr("Comment out")
                                                                        : tr("Re-enable"));
    });
    instLayout->addWidget(m_installed, 1);

    auto *instButtons = new QHBoxLayout;
    m_toggleButton = new QPushButton(tr("Comment out"), installedSide);
    m_toggleButton->setEnabled(false);
    connect(m_toggleButton, &QPushButton::clicked, this, &PackagesPage::toggleSelectedInstalled);
    instButtons->addWidget(m_toggleButton);

    m_removeButton = new QPushButton(tr("Remove"), installedSide);
    m_removeButton->setEnabled(false);
    connect(m_removeButton, &QPushButton::clicked, this, &PackagesPage::removeSelectedInstalled);
    instButtons->addWidget(m_removeButton);
    instButtons->addStretch(1);
    instLayout->addLayout(instButtons);

    splitter->addWidget(installedSide);
    splitter->setSizes({ 640, 420 });
}

void PackagesPage::refresh()
{
    if (m_ctx.project && m_ctx.project->isOpen())
        m_ctx.search->setChannel(m_ctx.project->nixpkgsChannel());
    reloadTargets();
    reloadInstalled();

    if (m_ctx.project && m_ctx.project->isOpen()) {
        m_searchStatus->setText(
            tr("Searching the %1 package index.").arg(m_ctx.project->nixpkgsChannel()));
    }
}

void PackagesPage::runSearch()
{
    const QString q = m_query->text().trimmed();
    if (q.isEmpty())
        return;
    m_searchStatus->setText(tr("Searching…"));
    m_ctx.search->search(q);
}

void PackagesPage::onSearchFailed(const QString &message)
{
    m_searchStatus->setText(message);
}

void PackagesPage::onResults(const QString &query, const QVector<PackageResult> &results,
    bool fromCache)
{
    m_results = results;
    m_resultTable->setRowCount(results.size());
    for (int row = 0; row < results.size(); ++row) {
        const PackageResult &r = results.at(row);
        m_resultTable->setItem(row, 0, new QTableWidgetItem(r.attr));
        m_resultTable->setItem(row, 1, new QTableWidgetItem(r.version));
        m_resultTable->setItem(row, 2, new QTableWidgetItem(r.description));
    }
    m_searchStatus->setText(results.isEmpty()
            ? tr("No packages matched “%1”.").arg(query)
            : tr("%n result(s) for “%1”%2", nullptr, int(results.size()))
                  .arg(query, fromCache ? tr(" (cached)") : QString()));
    if (!results.isEmpty())
        m_resultTable->selectRow(0);
    else
        m_details->clear();
}

const PackageResult *PackagesPage::selectedResult() const
{
    const int row = m_resultTable->currentRow();
    if (row < 0 || row >= m_results.size())
        return nullptr;
    return &m_results.at(row);
}

void PackagesPage::onResultSelected()
{
    const PackageResult *r = selectedResult();
    m_addButton->setEnabled(r != nullptr && m_targetBox->count() > 0);
    if (!r) {
        m_details->clear();
        return;
    }

    QString html = QStringLiteral("<h3>%1</h3>").arg(r->attr.toHtmlEscaped());
    if (!r->version.isEmpty())
        html += QStringLiteral("<p><b>%1</b> %2</p>").arg(tr("Version:"), r->version.toHtmlEscaped());
    if (!r->description.isEmpty())
        html += QStringLiteral("<p>%1</p>").arg(r->description.toHtmlEscaped());
    if (!r->longDescription.isEmpty())
        html += QStringLiteral("<p><small>%1</small></p>")
                    .arg(r->longDescription.left(700).toHtmlEscaped());
    if (!r->homepage.isEmpty())
        html += QStringLiteral("<p><b>%1</b> <a href=\"%2\">%2</a></p>")
                    .arg(tr("Homepage:"), r->homepage.toHtmlEscaped());
    if (!r->license.isEmpty())
        html += QStringLiteral("<p><b>%1</b> %2</p>").arg(tr("License:"), r->license.toHtmlEscaped());
    if (!r->programs.isEmpty())
        html += QStringLiteral("<p><b>%1</b> <code>%2</code></p>")
                    .arg(tr("Provides:"),
                        r->programs.mid(0, 12).join(QStringLiteral(", ")).toHtmlEscaped());
    m_details->setHtml(html);
}

void PackagesPage::reloadTargets()
{
    const QString remembered = QSettings().value(QStringLiteral("packages/lastTarget")).toString();
    const QString current = m_targetBox->currentText();

    m_targets.clear();
    m_targetBox->clear();
    if (!m_ctx.project || !m_ctx.project->isOpen())
        return;

    QStringList files;
    for (const HostInfo &h : m_ctx.project->hosts())
        files << h.entryFile;
    for (const ModuleInfo &m : m_ctx.project->modules())
        files << m.absPath;

    QDir root(m_ctx.project->root());
    for (const QString &absPath : std::as_const(files)) {
        NixFile *f = m_ctx.project->file(absPath);
        if (!f)
            continue;
        for (const NixList *l : f->packageLists()) {
            Target t;
            t.absPath = absPath;
            t.listPath = l->path;
            t.label = QStringLiteral("%1  ·  %2")
                          .arg(root.relativeFilePath(absPath), l->path);
            m_targets.push_back(t);
            m_targetBox->addItem(t.label);
        }
    }

    const QString wanted = current.isEmpty() ? remembered : current;
    const int index = m_targetBox->findText(wanted);
    if (index >= 0)
        m_targetBox->setCurrentIndex(index);

    m_addButton->setEnabled(selectedResult() != nullptr && m_targetBox->count() > 0);
}

void PackagesPage::addSelectedPackage()
{
    const PackageResult *r = selectedResult();
    if (!r)
        return;
    const int index = m_targetBox->currentIndex();
    if (index < 0 || index >= m_targets.size()) {
        QMessageBox::information(this, tr("Add package"),
            tr("There is no package list to add to. Create a module with an "
               "environment.systemPackages list first."));
        return;
    }

    const Target target = m_targets.at(index);
    NixFile *file = m_ctx.project->file(target.absPath);
    if (!file)
        return;

    const NixList *list = file->findList(target.listPath);
    if (!list)
        return;

    // `with pkgs;` lists take the bare attribute; a `with pkgs.kdePackages;`
    // list needs the attribute relative to that set, and a plain list needs the
    // full `pkgs.` prefix.
    QString expr = r->attr;
    if (list->withExpr.isEmpty()) {
        expr = QStringLiteral("pkgs.") + r->attr;
    } else if (list->withExpr != QLatin1String("pkgs")) {
        const QString prefix = list->withExpr.startsWith(QLatin1String("pkgs."))
            ? list->withExpr.mid(5) + QLatin1Char('.')
            : QString();
        if (!prefix.isEmpty() && r->attr.startsWith(prefix))
            expr = r->attr.mid(prefix.size());
        else
            expr = r->attr;
    }

    if (!file->addPackage(target.listPath, expr)) {
        QMessageBox::warning(this, tr("Add package"),
            tr("Could not add %1 to %2.").arg(expr, target.listPath));
        return;
    }

    QSettings().setValue(QStringLiteral("packages/lastTarget"), target.label);
    emit statusMessage(tr("Added %1 to %2").arg(expr, target.label));
    emit configModified();
    reloadInstalled();
}

void PackagesPage::reloadInstalled()
{
    m_installed->clear();
    if (!m_ctx.project || !m_ctx.project->isOpen())
        return;

    QStringList files;
    for (const HostInfo &h : m_ctx.project->hosts())
        files << h.entryFile;
    for (const ModuleInfo &m : m_ctx.project->modules())
        files << m.absPath;

    QDir root(m_ctx.project->root());
    int total = 0;
    for (const QString &absPath : std::as_const(files)) {
        NixFile *f = m_ctx.project->file(absPath);
        if (!f)
            continue;
        const auto lists = f->packageLists();
        if (lists.isEmpty())
            continue;

        auto *fileItem = new QTreeWidgetItem(m_installed, { root.relativeFilePath(absPath) });
        QFont bold = fileItem->font(0);
        bold.setBold(true);
        fileItem->setFont(0, bold);
        fileItem->setData(0, kRoleAbsPath, absPath);
        fileItem->setExpanded(true);

        for (const NixList *l : lists) {
            QTreeWidgetItem *listItem = fileItem;
            if (lists.size() > 1) {
                listItem = new QTreeWidgetItem(fileItem, { l->path });
                listItem->setForeground(0, palette().placeholderText());
                listItem->setExpanded(true);
            }
            for (const PackageEntry &e : l->entries) {
                auto *item = new QTreeWidgetItem(listItem);
                item->setText(0, e.expr);
                item->setText(1, e.comment);
                item->setData(0, kRoleAbsPath, absPath);
                item->setData(0, kRoleListPath, l->path);
                item->setData(0, kRoleExpr, e.expr);
                item->setData(0, kRoleEnabled, e.enabled);
                if (!e.enabled) {
                    QFont f2 = item->font(0);
                    f2.setStrikeOut(true);
                    item->setFont(0, f2);
                    item->setForeground(0, palette().placeholderText());
                }
                ++total;
            }
        }
    }

    applyInstalledFilter(m_installedFilter->text());
    m_installed->setHeaderLabels({ tr("Package (%1)").arg(total), tr("Note") });
}

void PackagesPage::applyInstalledFilter(const QString &text)
{
    const QString needle = text.trimmed();

    std::function<int(QTreeWidgetItem *)> filterItem = [&](QTreeWidgetItem *item) -> int {
        int visibleChildren = 0;
        for (int i = 0; i < item->childCount(); ++i)
            visibleChildren += filterItem(item->child(i));

        const bool isLeaf = item->childCount() == 0;
        bool matches = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
        if (!isLeaf)
            matches = visibleChildren > 0 || (needle.isEmpty());
        item->setHidden(!matches);
        return matches ? 1 : 0;
    };

    for (int i = 0; i < m_installed->topLevelItemCount(); ++i)
        filterItem(m_installed->topLevelItem(i));
}

void PackagesPage::removeSelectedInstalled()
{
    auto *item = m_installed->currentItem();
    if (!item)
        return;
    const QString absPath = item->data(0, kRoleAbsPath).toString();
    const QString listPath = item->data(0, kRoleListPath).toString();
    const QString expr = item->data(0, kRoleExpr).toString();
    if (expr.isEmpty())
        return;

    NixFile *file = m_ctx.project->file(absPath);
    if (!file || !file->removePackage(listPath, expr))
        return;

    emit statusMessage(tr("Removed %1").arg(expr));
    emit configModified();
    reloadInstalled();
}

void PackagesPage::toggleSelectedInstalled()
{
    auto *item = m_installed->currentItem();
    if (!item)
        return;
    const QString absPath = item->data(0, kRoleAbsPath).toString();
    const QString listPath = item->data(0, kRoleListPath).toString();
    const QString expr = item->data(0, kRoleExpr).toString();
    const bool enabled = item->data(0, kRoleEnabled).toBool();
    if (expr.isEmpty())
        return;

    NixFile *file = m_ctx.project->file(absPath);
    if (!file || !file->setPackageEnabled(listPath, expr, !enabled))
        return;

    emit statusMessage(enabled ? tr("Commented out %1").arg(expr) : tr("Re-enabled %1").arg(expr));
    emit configModified();
    reloadInstalled();
}

} // namespace nixm
