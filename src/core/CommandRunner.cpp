#include "CommandRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace nixm {

CommandRunner::CommandRunner(QObject *parent)
    : QObject(parent)
{
    const auto helpers = availableEscalationHelpers();
    m_helper = helpers.isEmpty() ? QStringLiteral("none") : helpers.first();

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &CommandRunner::onReadyRead);
    connect(m_proc, &QProcess::finished, this, &CommandRunner::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit output(tr("!! failed to start: %1\n").arg(m_current.program));
            finishBatch(false);
        }
    });
}

CommandRunner::~CommandRunner()
{
    if (m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
}

QStringList CommandRunner::availableEscalationHelpers()
{
    // sudo first: it is what most people already have configured, and unlike
    // pkexec it does not need a running polkit authentication agent.
    QStringList out;
    if (!QStandardPaths::findExecutable(QStringLiteral("sudo")).isEmpty())
        out << QStringLiteral("sudo");
    if (!QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty())
        out << QStringLiteral("pkexec");
    out << QStringLiteral("none");
    return out;
}

void CommandRunner::setPasswordPrompt(std::function<QString(const QString &)> prompt)
{
    m_passwordPrompt = std::move(prompt);
}

bool CommandRunner::ensureSudoCredentials()
{
    if (m_sudoReady)
        return true;

    const QString sudo = QStandardPaths::findExecutable(QStringLiteral("sudo"));
    if (sudo.isEmpty()) {
        emit output(tr("!! sudo is not installed\n"));
        return false;
    }

    // Already authenticated, either by a cached timestamp or by NOPASSWD.
    int exitCode = 0;
    captureOutput(sudo, { QStringLiteral("-n"), QStringLiteral("true") }, QString(), 5000,
        &exitCode);
    if (exitCode == 0) {
        m_sudoReady = true;
        return true;
    }

    if (!m_passwordPrompt) {
        emit output(tr("!! sudo needs a password and no prompt is available\n"));
        return false;
    }

    QString password = m_passwordPrompt(tr("Administrator password required to continue."));
    if (password.isEmpty()) {
        emit output(tr("^ cancelled: no password given\n"));
        return false;
    }

    // `sudo -v` only refreshes the timestamp, so the password reaches sudo and
    // nothing else. Every later step can then use `sudo -n`.
    QProcess validate;
    validate.setProcessChannelMode(QProcess::MergedChannels);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    validate.setProcessEnvironment(env);
    validate.start(sudo,
        { QStringLiteral("-S"), QStringLiteral("-p"), QString(), QStringLiteral("-v") });

    bool ok = false;
    if (validate.waitForStarted(5000)) {
        validate.write(password.toUtf8() + '\n');
        validate.closeWriteChannel();
        ok = validate.waitForFinished(30000) && validate.exitCode() == 0;
        if (!ok) {
            const QString reason = QString::fromUtf8(validate.readAll()).trimmed();
            emit output(tr("!! sudo authentication failed%1\n")
                            .arg(reason.isEmpty() ? QString()
                                                  : QStringLiteral(": ") + reason));
        }
    }

    // Do not keep the password around any longer than sudo needed it.
    password.fill(QLatin1Char('\0'));
    password.clear();

    m_sudoReady = ok;
    return ok;
}

void CommandRunner::setEscalationHelper(const QString &helper)
{
    m_helper = helper;
}

bool CommandRunner::isRunning() const
{
    return m_running;
}

QString CommandRunner::commandLine(const Step &step) const
{
    QStringList parts;
    if (step.privilege == AsRoot && m_helper != QLatin1String("none"))
        parts << m_helper;
    parts << step.program;
    parts << step.args;

    QStringList quoted;
    for (const QString &p : std::as_const(parts))
        quoted << (p.contains(QLatin1Char(' ')) ? QLatin1Char('"') + p + QLatin1Char('"') : p);
    return quoted.join(QLatin1Char(' '));
}

bool CommandRunner::run(const QVector<Step> &steps)
{
    if (m_running || steps.isEmpty())
        return false;
    m_cancelled = false;
    m_sudoReady = false;
    m_queue.clear();
    for (const Step &s : steps)
        m_queue.enqueue(s);

    m_running = true;
    emit runningChanged(true);
    startNext();
    return true;
}

void CommandRunner::startNext()
{
    if (m_cancelled) {
        finishBatch(false);
        return;
    }
    if (m_queue.isEmpty()) {
        finishBatch(true);
        return;
    }

    m_current = m_queue.dequeue();

    // Authenticate once, before the first command that needs it.
    if (m_current.privilege == AsRoot && m_helper == QLatin1String("sudo")
        && !ensureSudoCredentials()) {
        m_queue.clear();
        finishBatch(false);
        return;
    }

    QString program = m_current.program;
    QStringList args = m_current.args;

    // pkexec needs an absolute path and does not inherit the caller's PATH.
    const QString resolved = QStandardPaths::findExecutable(program);
    if (!resolved.isEmpty())
        program = resolved;

    if (m_current.privilege == AsRoot && m_helper != QLatin1String("none")) {
        args.prepend(program);
        if (m_helper == QLatin1String("sudo")) {
            // Credentials were established above, so this never blocks on a
            // prompt the user cannot see.
            args.prepend(QStringLiteral("-n"));
        }
        program = QStandardPaths::findExecutable(m_helper);
        if (program.isEmpty())
            program = m_helper;
    }

    if (!m_current.workDir.isEmpty())
        m_proc->setWorkingDirectory(m_current.workDir);
    else
        m_proc->setWorkingDirectory(QDir::homePath());

    auto env = QProcessEnvironment::systemEnvironment();
    // Stable, parseable output regardless of the user's locale.
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("NIX_PAGER"), QStringLiteral("cat"));
    env.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
    m_proc->setProcessEnvironment(env);

    emit stepStarted(m_current.label, commandLine(m_current));
    m_proc->start(program, args);
}

void CommandRunner::onReadyRead()
{
    const QByteArray chunk = m_proc->readAllStandardOutput();
    if (!chunk.isEmpty())
        emit output(QString::fromUtf8(chunk));
}

void CommandRunner::onFinished(int exitCode, QProcess::ExitStatus status)
{
    onReadyRead();
    const int code = (status == QProcess::CrashExit) ? -1 : exitCode;
    emit stepFinished(m_current.label, code);

    if (code != 0 || m_cancelled) {
        m_queue.clear();
        finishBatch(code == 0 && !m_cancelled);
        return;
    }
    startNext();
}

void CommandRunner::finishBatch(bool ok)
{
    if (!m_running)
        return;
    m_running = false;
    m_queue.clear();
    emit runningChanged(false);
    emit batchFinished(ok);
}

void CommandRunner::cancel()
{
    if (!m_running)
        return;
    m_cancelled = true;
    m_queue.clear();
    if (m_proc->state() != QProcess::NotRunning) {
        emit output(tr("\n^ cancelled by user\n"));
        m_proc->terminate();
        if (!m_proc->waitForFinished(3000))
            m_proc->kill();
    } else {
        finishBatch(false);
    }
}

QString CommandRunner::captureOutput(const QString &program, const QStringList &args,
    const QString &workDir, int timeoutMs, int *exitCode)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    if (!workDir.isEmpty())
        p.setWorkingDirectory(workDir);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    p.setProcessEnvironment(env);

    const QString resolved = QStandardPaths::findExecutable(program);
    p.start(resolved.isEmpty() ? program : resolved, args);
    if (!p.waitForStarted(5000)) {
        if (exitCode)
            *exitCode = -1;
        return QString();
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        if (exitCode)
            *exitCode = -1;
        return QString::fromUtf8(p.readAll());
    }
    if (exitCode)
        *exitCode = p.exitCode();
    return QString::fromUtf8(p.readAll());
}

} // namespace nixm
