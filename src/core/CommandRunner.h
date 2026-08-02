#pragma once

#include <QObject>
#include <QProcess>
#include <QQueue>

#include <functional>
#include <QString>
#include <QStringList>

namespace nixm {

/// Runs a sequence of external commands, streaming their merged output. Steps
/// run one after another and the queue stops at the first non-zero exit code.
class CommandRunner : public QObject
{
    Q_OBJECT

public:
    enum Privilege {
        AsUser,       ///< run as the current user
        AsRoot        ///< run through the configured escalation helper
    };
    Q_ENUM(Privilege)

    struct Step {
        QString label;
        QString program;
        QStringList args;
        QString workDir;
        Privilege privilege = AsUser;
    };

    explicit CommandRunner(QObject *parent = nullptr);
    ~CommandRunner() override;

    /// "sudo", "pkexec" or "none". Defaults to sudo when it is on PATH.
    void setEscalationHelper(const QString &helper);

    /// Supplies the sudo password when one is needed. The runner asks at most
    /// once per batch, hands the answer straight to `sudo -v` and drops it; the
    /// password is never placed on a command line or written to the log.
    void setPasswordPrompt(std::function<QString(const QString &reason)> prompt);
    QString escalationHelper() const { return m_helper; }
    static QStringList availableEscalationHelpers();

    bool isRunning() const;
    /// Queues and starts a batch. Returns false if a batch is already running.
    bool run(const QVector<Step> &steps);
    bool run(const Step &step) { return run(QVector<Step>{ step }); }
    void cancel();

    /// Renders a step the way it will be executed, for logging.
    QString commandLine(const Step &step) const;

    /// Runs a short command synchronously and returns its stdout. Used for cheap
    /// read-only queries (nix --version, listing generations, …).
    static QString captureOutput(const QString &program, const QStringList &args,
        const QString &workDir = QString(), int timeoutMs = 20000, int *exitCode = nullptr);

signals:
    void output(const QString &text);
    void stepStarted(const QString &label, const QString &commandLine);
    void stepFinished(const QString &label, int exitCode);
    void batchFinished(bool ok);
    void runningChanged(bool running);

private:
    void startNext();
    /// Returns false when sudo could not be authenticated and the batch must stop.
    bool ensureSudoCredentials();
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void finishBatch(bool ok);
    QProcess::ProcessChannelMode channelMode() const;

    QProcess *m_proc = nullptr;
    QQueue<Step> m_queue;
    Step m_current;
    QString m_helper;
    std::function<QString(const QString &)> m_passwordPrompt;
    bool m_sudoReady = false;   // credentials cached for this batch
    bool m_cancelled = false;
    bool m_running = false;
};

} // namespace nixm
