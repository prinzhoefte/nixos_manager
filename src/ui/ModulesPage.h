#pragma once

#include "AppContext.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace nixm {

/// Browses the reusable modules in the tree: what each one installs, what it
/// configures and which hosts use it. Also creates new modules from templates.
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
    void statusMessage(const QString &text);

private:
    void buildUi();
    void onSelectionChanged();
    void createModule();
    void applyFilter(const QString &text);
    QStringList hostsUsing(const QString &absModulePath) const;

    AppContext m_ctx;
    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_filter = nullptr;
    QLabel *m_title = nullptr;
    QTextBrowser *m_details = nullptr;
    QPushButton *m_open = nullptr;
};

} // namespace nixm
