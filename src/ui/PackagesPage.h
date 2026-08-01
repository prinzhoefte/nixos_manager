#pragma once

#include "AppContext.h"
#include "core/PackageSearch.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace nixm {

/// Package manager: search nixpkgs, add a package to any list in the tree, and
/// review or remove what is already installed.
class PackagesPage : public QWidget
{
    Q_OBJECT

public:
    explicit PackagesPage(const AppContext &ctx, QWidget *parent = nullptr);

public slots:
    void refresh();
    void applyTheme();

signals:
    void openFileRequested(const QString &absPath);
    void configModified();
    void statusMessage(const QString &text);

private:
    /// A place a package can be added to: one list in one file.
    struct Target {
        QString absPath;
        QString listPath;
        QString label;
    };

    void buildUi();
    void runSearch();
    void onResults(const QString &query, const QVector<PackageResult> &results, bool fromCache);
    void onSearchFailed(const QString &message);
    void onResultSelected();
    void addSelectedPackage();
    void reloadTargets();
    void reloadInstalled();
    void removeSelectedInstalled();
    void toggleSelectedInstalled();
    void applyInstalledFilter(const QString &text);
    const PackageResult *selectedResult() const;

    AppContext m_ctx;
    QVector<PackageResult> m_results;
    QVector<Target> m_targets;

    QLineEdit *m_query = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_searchStatus = nullptr;
    QTableWidget *m_resultTable = nullptr;
    QTextBrowser *m_details = nullptr;
    QComboBox *m_targetBox = nullptr;
    QPushButton *m_addButton = nullptr;

    QLineEdit *m_installedFilter = nullptr;
    QTreeWidget *m_installed = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_toggleButton = nullptr;
};

} // namespace nixm
