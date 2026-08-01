#pragma once

#include "AppContext.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace nixm {

/// Per-host view: identity, which modules the host imports, and the feature
/// options its modules declare.
class HostsPage : public QWidget
{
    Q_OBJECT

public:
    explicit HostsPage(const AppContext &ctx, QWidget *parent = nullptr);

    QString currentHost() const;
    void selectHost(const QString &name);

public slots:
    void refresh();
    void applyTheme();

signals:
    void openFileRequested(const QString &absPath);
    void rebuildRequested(const QString &host);
    void configModified();
    void statusMessage(const QString &text);

private:
    void buildUi();
    void onHostSelected();
    void reloadHostDetails();
    void reloadModuleTree();
    void reloadOptions();
    void reloadUsers();
    void onModuleItemChanged(QTreeWidgetItem *item, int column);
    void onOptionItemChanged(QTreeWidgetItem *item, int column);
    void commitIdentity();
    void applyModuleFilter(const QString &text);

    /// Section header a module should be filed under, derived from its category.
    QString sectionForCategory(const QString &category) const;

    AppContext m_ctx;
    bool m_updating = false;

    QListWidget *m_hostList = nullptr;
    QLabel *m_entryLabel = nullptr;
    QLineEdit *m_hostName = nullptr;
    QLineEdit *m_stateVersion = nullptr;
    QLineEdit *m_moduleFilter = nullptr;
    QTreeWidget *m_moduleTree = nullptr;
    QTreeWidget *m_optionTree = nullptr;
    QTreeWidget *m_userTree = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_openHostFile = nullptr;
    QPushButton *m_rebuild = nullptr;
};

} // namespace nixm
