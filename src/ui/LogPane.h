#pragma once

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace nixm {

class CommandRunner;

/// Shows the live output of whatever CommandRunner is doing, plus a cancel
/// button. Every privileged action in the app reports through here so the user
/// always sees the exact command that ran.
class LogPane : public QWidget
{
    Q_OBJECT

public:
    explicit LogPane(CommandRunner *runner, QWidget *parent = nullptr);

    void applyTheme();
    void appendNote(const QString &text);
    void appendError(const QString &text);
    void clear();

private:
    void onOutput(const QString &chunk);
    void onStepStarted(const QString &label, const QString &commandLine);
    void onStepFinished(const QString &label, int exitCode);
    void onRunningChanged(bool running);
    void appendHtmlLine(const QString &html);

    static QString stripAnsi(const QString &in);

    CommandRunner *m_runner = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_cancel = nullptr;
};

} // namespace nixm
