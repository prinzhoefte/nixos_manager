#include "EditorPage.h"

#include "NixHighlighter.h"
#include "Theme.h"
#include "core/ConfigProject.h"
#include "core/NixFile.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

namespace nixm {

EditorPage::EditorPage(const AppContext &ctx, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    buildUi();
}

void EditorPage::buildUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter);

    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setNameFilters({ QStringLiteral("*.nix"), QStringLiteral("*.lock"),
        QStringLiteral("*.md"), QStringLiteral("*.sh") });
    m_fsModel->setNameFilterDisables(false);
    m_fsModel->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);

    m_tree = new QTreeView(splitter);
    m_tree->setModel(m_fsModel);
    for (int col = 1; col < m_fsModel->columnCount(); ++col)
        m_tree->hideColumn(col);
    m_tree->setHeaderHidden(true);
    m_tree->setAnimated(false);
    connect(m_tree, &QTreeView::activated, this, &EditorPage::onTreeActivated);
    connect(m_tree, &QTreeView::clicked, this, &EditorPage::onTreeActivated);
    splitter->addWidget(m_tree);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(6, 6, 6, 6);

    auto *headerRow = new QHBoxLayout;
    m_header = new QLabel(tr("No file open"), right);
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerRow->addWidget(m_header, 1);

    m_reload = new QPushButton(tr("Discard changes"), right);
    m_reload->setIcon(Theme::icon(QStringLiteral("reload")));
    m_reload->setProperty("danger", true);
    m_reload->setToolTip(tr("Re-read this file from disk, throwing away unsaved edits."));
    m_reload->setEnabled(false);
    connect(m_reload, &QPushButton::clicked, this, &EditorPage::reloadFromDisk);
    headerRow->addWidget(m_reload);
    rightLayout->addLayout(headerRow);

    m_editor = new QPlainTextEdit(right);
    QFont code(Theme::monoFontFamily());
    code.setPointSizeF(QApplication::font().pointSizeF() + 0.5);
    m_editor->setFont(code);
    m_editor->setProperty("mono", true);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(4 * m_editor->fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    m_editor->setPlaceholderText(tr("Pick a file on the left to edit it."));
    m_editor->setEnabled(false);
    rightLayout->addWidget(m_editor, 1);

    m_highlighter = new NixHighlighter(m_editor->document());
    m_highlighter->setDarkMode(Theme::isDark());

    // Editing pushes into the shared buffer, but only after the user pauses, so
    // we do not re-parse on every keystroke.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(400);
    connect(m_debounce, &QTimer::timeout, this, &EditorPage::commitToBuffer);
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_loading)
            m_debounce->start();
    });

    splitter->addWidget(right);
    splitter->setSizes({ 280, 760 });
}

void EditorPage::refresh()
{
    if (!m_ctx.project || !m_ctx.project->isOpen()) {
        m_editor->clear();
        m_editor->setEnabled(false);
        m_currentPath.clear();
        m_header->setText(tr("No configuration open"));
        return;
    }

    const QString root = m_ctx.project->root();
    m_fsModel->setRootPath(root);
    m_tree->setRootIndex(m_fsModel->index(root));

    if (!m_currentPath.isEmpty())
        syncFromBuffer();
}

void EditorPage::onTreeActivated(const QModelIndex &index)
{
    const QString path = m_fsModel->filePath(index);
    if (QFileInfo(path).isFile())
        openFile(path);
}

void EditorPage::openFile(const QString &absPath)
{
    if (absPath.isEmpty())
        return;

    // Flush anything pending for the file we are leaving.
    if (m_debounce->isActive()) {
        m_debounce->stop();
        commitToBuffer();
    }

    m_currentPath = QFileInfo(absPath).absoluteFilePath();
    NixFile *file = m_ctx.project ? m_ctx.project->file(m_currentPath) : nullptr;

    m_loading = true;
    if (file) {
        m_editor->setPlainText(file->text());
        m_editor->setEnabled(true);
        m_reload->setEnabled(true);
    } else {
        m_editor->setPlainText(tr("Could not read %1").arg(m_currentPath));
        m_editor->setEnabled(false);
        m_reload->setEnabled(false);
    }
    m_loading = false;

    if (m_ctx.project) {
        const QString rel = QDir(m_ctx.project->root()).relativeFilePath(m_currentPath);
        m_header->setText(QStringLiteral("<b>%1</b>%2")
                              .arg(rel.toHtmlEscaped(),
                                  (file && file->isDirty()) ? tr(" — unsaved") : QString()));
    }

    const QModelIndex index = m_fsModel->index(m_currentPath);
    if (index.isValid()) {
        m_tree->setCurrentIndex(index);
        m_tree->scrollTo(index);
    }
}

void EditorPage::syncFromBuffer()
{
    if (m_currentPath.isEmpty() || !m_ctx.project)
        return;
    NixFile *file = m_ctx.project->file(m_currentPath);
    if (!file)
        return;
    if (file->text() == m_editor->toPlainText()) {
        const QString rel = QDir(m_ctx.project->root()).relativeFilePath(m_currentPath);
        m_header->setText(QStringLiteral("<b>%1</b>%2")
                              .arg(rel.toHtmlEscaped(),
                                  file->isDirty() ? tr(" — unsaved") : QString()));
        return;
    }

    // Another page rewrote the buffer; keep the caret roughly where it was.
    const int scroll = m_editor->verticalScrollBar()->value();
    const int position = m_editor->textCursor().position();
    m_loading = true;
    m_editor->setPlainText(file->text());
    m_loading = false;

    QTextCursor cursor = m_editor->textCursor();
    cursor.setPosition(qMin(position, m_editor->document()->characterCount() - 1));
    m_editor->setTextCursor(cursor);
    m_editor->verticalScrollBar()->setValue(scroll);

    const QString rel = QDir(m_ctx.project->root()).relativeFilePath(m_currentPath);
    m_header->setText(QStringLiteral("<b>%1</b>%2")
                          .arg(rel.toHtmlEscaped(),
                              file->isDirty() ? tr(" — unsaved") : QString()));
}

void EditorPage::commitToBuffer()
{
    if (m_currentPath.isEmpty() || m_loading || !m_ctx.project)
        return;
    NixFile *file = m_ctx.project->file(m_currentPath);
    if (!file)
        return;
    const QString text = m_editor->toPlainText();
    if (file->text() == text)
        return;

    file->setText(text);
    const QString rel = QDir(m_ctx.project->root()).relativeFilePath(m_currentPath);
    m_header->setText(QStringLiteral("<b>%1</b>%2").arg(rel.toHtmlEscaped(), tr(" — unsaved")));
    emit configModified();
}

void EditorPage::reloadFromDisk()
{
    if (m_currentPath.isEmpty() || !m_ctx.project)
        return;
    NixFile *file = m_ctx.project->file(m_currentPath);
    if (!file)
        return;
    QString err;
    if (!file->load(&err)) {
        emit statusMessage(err);
        return;
    }
    m_loading = true;
    m_editor->setPlainText(file->text());
    m_loading = false;
    emit statusMessage(tr("Reloaded %1").arg(QFileInfo(m_currentPath).fileName()));
    emit configModified();
}

void EditorPage::applyTheme()
{
    m_highlighter->setDarkMode(Theme::isDark());
    QFont code(Theme::monoFontFamily());
    code.setPointSizeF(QApplication::font().pointSizeF() + 0.5);
    m_editor->setFont(code);
    m_reload->setIcon(Theme::icon(QStringLiteral("reload")));
}

} // namespace nixm
