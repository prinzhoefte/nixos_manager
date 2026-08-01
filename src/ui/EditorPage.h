#pragma once

#include "AppContext.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTimer;
class QTreeView;
class QFileSystemModel;

namespace nixm {

class NixHighlighter;

/// The escape hatch: a plain Nix editor over the very same buffers the
/// structured pages edit, so anything the parser does not model can still be
/// changed by hand.
class EditorPage : public QWidget
{
    Q_OBJECT

public:
    explicit EditorPage(const AppContext &ctx, QWidget *parent = nullptr);

    void openFile(const QString &absPath);
    QString currentFile() const { return m_currentPath; }

public slots:
    void refresh();
    /// Re-reads the buffer when another page changed it.
    void syncFromBuffer();

signals:
    void configModified();
    void statusMessage(const QString &text);

private:
    void buildUi();
    void onTreeActivated(const QModelIndex &index);
    void commitToBuffer();
    void reloadFromDisk();

    AppContext m_ctx;
    QString m_currentPath;
    bool m_loading = false;

    QFileSystemModel *m_fsModel = nullptr;
    QTreeView *m_tree = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    NixHighlighter *m_highlighter = nullptr;
    QLabel *m_header = nullptr;
    QPushButton *m_reload = nullptr;
    QTimer *m_debounce = nullptr;
};

} // namespace nixm
