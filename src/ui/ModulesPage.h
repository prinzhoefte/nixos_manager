#pragma once

#include "AppContext.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QMenu;
class QPoint;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace nixm {

/// Browses the reusable modules in the tree: what each one installs, what it
/// configures and which hosts use it. Also creates modules from templates and
/// deletes them again, imports included.
class ModulesPage : public QWidget
{
    Q_OBJECT

public:
    explicit ModulesPage(const AppContext &ctx, QWidget *parent = nullptr);

public slots:
    void refresh();
    void applyTheme();

signals:
    void openFileRequested(const QString &absPath);
    void configModified();
    /// Files appeared or disappeared, so every page has to re-read the tree.
    void projectStructureChanged();
    /// Rewritten buffers should be written to disk now.
    void saveRequested();
    void statusMessage(const QString &text);

private:
    void buildUi();
    void onSelectionChanged();
    void createModule();
    void deleteSelected();
    void showContextMenu(const QPoint &pos);
    void applyFilter(const QString &text);
    /// Absolute paths of the selected modules; category rows contribute their
    /// children, so deleting a whole category is one click.
    QStringList selectedModules() const;
    QStringList hostsUsing(const QString &absModulePath) const;

    AppContext m_ctx;
    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_filter = nullptr;
    QLabel *m_title = nullptr;
    QTextBrowser *m_details = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_delete = nullptr;
};

} // namespace nixm
