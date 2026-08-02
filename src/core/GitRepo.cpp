#include "GitRepo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

namespace nixm {
namespace {

CommandRunner::Step gitStep(const QString &dir, const QString &label, const QStringList &args)
{
    CommandRunner::Step s;
    s.label = label;
    s.program = QStringLiteral("git");
    s.args = args;
    s.workDir = dir;
    s.privilege = CommandRunner::AsUser;
    return s;
}

QString runGit(const QString &dir, const QStringList &args, int *exitCode = nullptr,
    int timeoutMs = 20000)
{
    return CommandRunner::captureOutput(QStringLiteral("git"), args, dir, timeoutMs, exitCode);
}

/// porcelain=v2 quotes paths containing unusual characters, C-style.
QString unquotePath(const QString &raw)
{
    if (!raw.startsWith(QLatin1Char('"')) || !raw.endsWith(QLatin1Char('"')) || raw.size() < 2)
        return raw;
    QString out;
    const QString inner = raw.mid(1, raw.size() - 2);
    for (int i = 0; i < inner.size(); ++i) {
        if (inner[i] == QLatin1Char('\\') && i + 1 < inner.size()) {
            const QChar next = inner[++i];
            if (next == QLatin1Char('n'))
                out += QLatin1Char('\n');
            else if (next == QLatin1Char('t'))
                out += QLatin1Char('\t');
            else
                out += next;
        } else {
            out += inner[i];
        }
    }
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

bool GitFileChange::isStaged() const
{
    return !untracked && indexStatus != QLatin1Char('.') && indexStatus != QLatin1Char(' ');
}

bool GitFileChange::isUnstaged() const
{
    return untracked
        || (worktreeStatus != QLatin1Char('.') && worktreeStatus != QLatin1Char(' '));
}

QString GitFileChange::describe() const
{
    if (conflicted)
        return QObject::tr("conflicted");
    if (untracked)
        return QObject::tr("untracked");

    auto word = [](QChar c) -> QString {
        switch (c.toLatin1()) {
        case 'M':
            return QObject::tr("modified");
        case 'A':
            return QObject::tr("added");
        case 'D':
            return QObject::tr("deleted");
        case 'R':
            return QObject::tr("renamed");
        case 'C':
            return QObject::tr("copied");
        case 'T':
            return QObject::tr("type changed");
        default:
            return QString();
        }
    };

    const QString staged = word(indexStatus);
    const QString unstaged = word(worktreeStatus);
    if (!staged.isEmpty() && !unstaged.isEmpty() && staged != unstaged)
        return QObject::tr("%1, %2 unstaged").arg(staged, unstaged);
    if (!staged.isEmpty())
        return staged;
    if (!unstaged.isEmpty())
        return unstaged;
    return QObject::tr("changed");
}

int GitStatus::stagedCount() const
{
    int n = 0;
    for (const GitFileChange &c : changes)
        if (c.isStaged())
            ++n;
    return n;
}

int GitStatus::unstagedCount() const
{
    int n = 0;
    for (const GitFileChange &c : changes)
        if (c.isUnstaged())
            ++n;
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────

namespace GitRepo {

bool available()
{
    static const bool found
        = !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
    return found;
}

GitStatus status(const QString &dir)
{
    GitStatus out;
    if (!available()) {
        out.error = QObject::tr("git is not installed.");
        return out;
    }
    if (dir.isEmpty() || !QFileInfo::exists(dir))
        return out;

    int exitCode = 0;
    const QString topLevel
        = runGit(dir, { QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel") },
            &exitCode)
              .trimmed();

    if (exitCode != 0) {
        // "dubious ownership" is the common one on a root-owned /etc/nixos.
        if (topLevel.contains(QLatin1String("dubious ownership"))
            || topLevel.contains(QLatin1String("safe.directory")))
            out.error = topLevel.trimmed();
        return out;   // not a repository, which is a normal state
    }

    out.isRepository = true;
    out.root = topLevel;

    const QString name
        = runGit(dir, { QStringLiteral("config"), QStringLiteral("user.name") }).trimmed();
    const QString email
        = runGit(dir, { QStringLiteral("config"), QStringLiteral("user.email") }).trimmed();
    out.hasIdentity = !name.isEmpty() && !email.isEmpty();

    int remoteExit = 0;
    const QString remote = runGit(dir,
        { QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin") },
        &remoteExit)
                               .trimmed();
    if (remoteExit == 0)
        out.remoteUrl = remote;

    int headExit = 0;
    runGit(dir, { QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("HEAD") },
        &headExit);
    out.hasCommits = headExit == 0;

    const QString raw = runGit(dir,
        { QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("--branch"),
            QStringLiteral("--untracked-files=all") },
        &exitCode, 30000);
    if (exitCode != 0) {
        out.error = raw.trimmed();
        return out;
    }

    const auto lines = raw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("# branch.head "))) {
            out.branch = line.mid(14).trimmed();
            out.detached = out.branch == QLatin1String("(detached)");
        } else if (line.startsWith(QLatin1String("# branch.upstream "))) {
            out.upstream = line.mid(18).trimmed();
        } else if (line.startsWith(QLatin1String("# branch.ab "))) {
            // "+3 -1"
            const auto parts = line.mid(12).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                if (part.startsWith(QLatin1Char('+')))
                    out.ahead = part.mid(1).toInt();
                else if (part.startsWith(QLatin1Char('-')))
                    out.behind = part.mid(1).toInt();
            }
        } else if (line.startsWith(QLatin1String("1 "))
            || line.startsWith(QLatin1String("2 "))) {
            // 1 XY sub mH mI mW hH hI path
            // 2 XY sub mH mI mW hH hI X<score> path<TAB>origPath
            const bool rename = line.startsWith(QLatin1String("2 "));
            const int fields = rename ? 9 : 8;
            const auto parts = line.split(QLatin1Char(' '));
            if (parts.size() < fields + 1)
                continue;
            GitFileChange c;
            const QString xy = parts.at(1);
            c.indexStatus = xy.at(0);
            c.worktreeStatus = xy.size() > 1 ? xy.at(1) : QLatin1Char('.');

            QString rest = parts.mid(fields).join(QLatin1Char(' '));
            if (rename) {
                const int tab = rest.indexOf(QLatin1Char('\t'));
                if (tab >= 0) {
                    c.originalPath = unquotePath(rest.mid(tab + 1));
                    rest = rest.left(tab);
                }
            }
            c.path = unquotePath(rest);
            out.changes.push_back(c);
        } else if (line.startsWith(QLatin1String("u "))) {
            const auto parts = line.split(QLatin1Char(' '));
            if (parts.size() < 11)
                continue;
            GitFileChange c;
            c.conflicted = true;
            c.path = unquotePath(parts.mid(10).join(QLatin1Char(' ')));
            out.changes.push_back(c);
        } else if (line.startsWith(QLatin1String("? "))) {
            GitFileChange c;
            c.untracked = true;
            c.path = unquotePath(line.mid(2));
            out.changes.push_back(c);
        }
    }

    std::sort(out.changes.begin(), out.changes.end(),
        [](const GitFileChange &a, const GitFileChange &b) { return a.path < b.path; });
    return out;
}

QString diff(const QString &dir, const QString &path, bool staged)
{
    if (!available())
        return QString();

    QStringList args { QStringLiteral("--no-pager"), QStringLiteral("diff"),
        QStringLiteral("--no-color") };
    if (staged)
        args << QStringLiteral("--cached");
    if (!path.isEmpty())
        args << QStringLiteral("--") << path;

    int exitCode = 0;
    QString out = runGit(dir, args, &exitCode, 30000);

    // An untracked file has no diff; show its contents instead so the pane is
    // never mysteriously empty.
    if (out.trimmed().isEmpty() && !staged && !path.isEmpty()) {
        const QString abs = QDir(dir).absoluteFilePath(path);
        QFile f(abs);
        if (QFileInfo(abs).isFile() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            const QStringList body = in.readAll().split(QLatin1Char('\n'));
            out = QStringLiteral("+++ %1\n").arg(path);
            for (const QString &l : body)
                out += QLatin1Char('+') + l + QLatin1Char('\n');
        }
    }
    return out;
}

QVector<GitCommit> log(const QString &dir, int limit)
{
    QVector<GitCommit> out;
    if (!available())
        return out;

    // Unit separator between fields, record separator between commits.
    int exitCode = 0;
    const QString raw = runGit(dir,
        { QStringLiteral("--no-pager"), QStringLiteral("log"),
            QStringLiteral("--max-count=%1").arg(limit),
            QStringLiteral("--date=format:%Y-%m-%d %H:%M"),
            QStringLiteral("--pretty=format:%H%x1f%h%x1f%an%x1f%ad%x1f%s") },
        &exitCode, 30000);
    if (exitCode != 0)
        return out;

    const auto lines = raw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const auto fields = line.split(QChar(0x1F));
        if (fields.size() < 5)
            continue;
        GitCommit c;
        c.hash = fields.at(0);
        c.shortHash = fields.at(1);
        c.author = fields.at(2);
        c.date = fields.at(3);
        c.subject = fields.at(4);
        out.push_back(c);
    }
    return out;
}

QString readGitignore(const QString &dir)
{
    QFile f(QDir(dir).absoluteFilePath(QStringLiteral(".gitignore")));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// ── Mutating operations ──────────────────────────────────────────────────────

CommandRunner::Step init(const QString &dir)
{
    return gitStep(dir, QObject::tr("initialise a git repository"),
        { QStringLiteral("init"), QStringLiteral("--initial-branch=main") });
}

CommandRunner::Step stage(const QString &dir, const QStringList &paths)
{
    QStringList args { QStringLiteral("add"), QStringLiteral("--") };
    args << paths;
    return gitStep(dir, QObject::tr("stage %n path(s)", nullptr, int(paths.size())), args);
}

CommandRunner::Step unstage(const QString &dir, const QStringList &paths)
{
    QStringList args { QStringLiteral("restore"), QStringLiteral("--staged"),
        QStringLiteral("--") };
    args << paths;
    return gitStep(dir, QObject::tr("unstage %n path(s)", nullptr, int(paths.size())), args);
}

CommandRunner::Step commit(const QString &dir, const QString &message, bool amend)
{
    QStringList args { QStringLiteral("commit"), QStringLiteral("--message"), message };
    if (amend)
        args << QStringLiteral("--amend");
    return gitStep(dir, amend ? QObject::tr("amend the last commit") : QObject::tr("commit"),
        args);
}

CommandRunner::Step push(const QString &dir, const QString &branch, bool setUpstream)
{
    QStringList args { QStringLiteral("push") };
    if (setUpstream)
        args << QStringLiteral("--set-upstream") << QStringLiteral("origin") << branch;
    return gitStep(dir, QObject::tr("push"), args);
}

CommandRunner::Step pull(const QString &dir)
{
    return gitStep(dir, QObject::tr("pull"),
        { QStringLiteral("pull"), QStringLiteral("--ff-only") });
}

CommandRunner::Step fetch(const QString &dir)
{
    return gitStep(dir, QObject::tr("fetch"),
        { QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune") });
}

CommandRunner::Step discard(const QString &dir, const QStringList &paths)
{
    QStringList args { QStringLiteral("checkout"), QStringLiteral("HEAD"), QStringLiteral("--") };
    args << paths;
    return gitStep(dir, QObject::tr("discard changes in %n path(s)", nullptr, int(paths.size())),
        args);
}

QVector<CommandRunner::Step> setIdentity(const QString &dir, const QString &name,
    const QString &email)
{
    // Written to the repository, not globally, so the app never changes the
    // user's identity for anything outside this configuration tree.
    return {
        gitStep(dir, QObject::tr("set user.name"),
            { QStringLiteral("config"), QStringLiteral("user.name"), name }),
        gitStep(dir, QObject::tr("set user.email"),
            { QStringLiteral("config"), QStringLiteral("user.email"), email }),
    };
}

CommandRunner::Step markSafeDirectory(const QString &dir)
{
    return gitStep(dir, QObject::tr("mark %1 as a safe directory").arg(dir),
        { QStringLiteral("config"), QStringLiteral("--global"), QStringLiteral("--add"),
            QStringLiteral("safe.directory"), dir });
}

} // namespace GitRepo
} // namespace nixm
