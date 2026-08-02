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

/// Add, edit and remove NixOS options.
///
/// The left half searches the NixOS option index so you can look up an option's
/// type, default and description before setting it. The right half lists every
/// option your configuration actually sets, grouped by file, and edits or
/// removes them in place.
class OptionsPage : public QWidget
{
    Q_OBJECT

public:
    explicit OptionsPage(const AppContext &ctx, QWidget *parent = nullptr);

public slots:
    void refresh();
    void applyTheme();

signals:
    void openFileRequested(const QString &absPath);
    void configModified();
    void statusMessage(const QString &text);

private:
    void buildUi();
    void runSearch();
    void onResults(const QString &query, const QVector<OptionResult> &results, bool fromCache);
    void onResultSelected();
    void applySelectedOption();
    void reloadTargets();
    void reloadSetOptions();
    void applyFilter(const QString &text);
    void onSetItemChanged(QTreeWidgetItem *item, int column);
    void editSelected();
    void removeSelected();
    void addManually();
    const OptionResult *selectedResult() const;
    /// Absolute path of the file the "Set in" combo points at.
    QString targetFile() const;

    AppContext m_ctx;
    QVector<OptionResult> m_results;
    QStringList m_targetFiles;
    bool m_updating = false;

    QLineEdit *m_query = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_searchStatus = nullptr;
    QTableWidget *m_resultTable = nullptr;
    QTextBrowser *m_details = nullptr;
    QComboBox *m_targetBox = nullptr;
    QLineEdit *m_valueEdit = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_manualButton = nullptr;

    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_setTree = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_removeButton = nullptr;
};

} // namespace nixm
