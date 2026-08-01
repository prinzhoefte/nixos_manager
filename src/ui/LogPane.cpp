#include "LogPane.h"

#include "core/CommandRunner.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>

namespace nixm {

LogPane::LogPane(CommandRunner *runner, QWidget *parent)
    : QWidget(parent)
    , m_runner(runner)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto *bar = new QHBoxLayout;
    m_status = new QLabel(tr("Idle"), this);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addWidget(m_status, 1);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setMaximumWidth(140);
    m_progress->setVisible(false);
    bar->addWidget(m_progress);

    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setEnabled(false);
    connect(m_cancel, &QPushButton::clicked, m_runner, &CommandRunner::cancel);
    bar->addWidget(m_cancel);

    auto *copy = new QPushButton(tr("Copy"), this);
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_output->toPlainText());
    });
    bar->addWidget(copy);

    auto *clear = new QPushButton(tr("Clear"), this);
    connect(clear, &QPushButton::clicked, this, &LogPane::clear);
    bar->addWidget(clear);

    layout->addLayout(bar);

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(20000);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_output->setPlaceholderText(
        tr("Output from nixos-rebuild, garbage collection and flake updates appears here."));
    layout->addWidget(m_output, 1);

    connect(m_runner, &CommandRunner::output, this, &LogPane::onOutput);
    connect(m_runner, &CommandRunner::stepStarted, this, &LogPane::onStepStarted);
    connect(m_runner, &CommandRunner::stepFinished, this, &LogPane::onStepFinished);
    connect(m_runner, &CommandRunner::runningChanged, this, &LogPane::onRunningChanged);
}

void LogPane::clear()
{
    m_output->clear();
}

QString LogPane::stripAnsi(const QString &in)
{
    static const QRegularExpression ansi(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]"));
    QString out = in;
    out.remove(ansi);
    // Progress lines overwrite themselves with \r; keep them as separate lines.
    out.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return out;
}

void LogPane::onOutput(const QString &chunk)
{
    const bool atBottom
        = m_output->verticalScrollBar()->value() >= m_output->verticalScrollBar()->maximum() - 4;

    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(stripAnsi(chunk));

    if (atBottom)
        m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

void LogPane::appendHtmlLine(const QString &html)
{
    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    if (!m_output->document()->isEmpty())
        cursor.insertText(QStringLiteral("\n"));
    cursor.insertHtml(html);
    cursor.insertText(QStringLiteral("\n"));
    m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
}

void LogPane::appendNote(const QString &text)
{
    appendHtmlLine(QStringLiteral("<span style='color:#3b82f6'>%1</span>").arg(text.toHtmlEscaped()));
}

void LogPane::appendError(const QString &text)
{
    appendHtmlLine(QStringLiteral("<span style='color:#ef4444'><b>%1</b></span>")
                       .arg(text.toHtmlEscaped()));
}

void LogPane::onStepStarted(const QString &label, const QString &commandLine)
{
    Q_UNUSED(label);
    appendHtmlLine(QStringLiteral("<span style='color:#3b82f6'><b>$ %1</b></span>")
                       .arg(commandLine.toHtmlEscaped()));
    m_status->setText(tr("Running: %1").arg(label));
}

void LogPane::onStepFinished(const QString &label, int exitCode)
{
    if (exitCode == 0) {
        appendHtmlLine(QStringLiteral("<span style='color:#22c55e'>✓ %1</span>")
                           .arg(tr("%1 finished").arg(label).toHtmlEscaped()));
    } else {
        appendHtmlLine(QStringLiteral("<span style='color:#ef4444'><b>✗ %1</b></span>")
                           .arg(tr("%1 failed with exit code %2").arg(label).arg(exitCode)
                                   .toHtmlEscaped()));
    }
}

void LogPane::onRunningChanged(bool running)
{
    m_cancel->setEnabled(running);
    m_progress->setVisible(running);
    if (!running)
        m_status->setText(tr("Idle"));
}

} // namespace nixm
