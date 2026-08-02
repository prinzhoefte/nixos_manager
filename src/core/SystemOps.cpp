#include "SystemOps.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>

#include <sys/stat.h>

namespace nixm {
namespace SystemOps {

QString rebuildActionName(RebuildAction a)
{
    switch (a) {
    case RebuildAction::Switch:
        return QStringLiteral("switch");
    case RebuildAction::Boot:
        return QStringLiteral("boot");
    case RebuildAction::Test:
        return QStringLiteral("test");
    case RebuildAction::DryBuild:
        return QStringLiteral("dry-build");
    case RebuildAction::DryActivate:
        return QStringLiteral("dry-activate");
    case RebuildAction::Build:
        return QStringLiteral("build");
    }
    return QStringLiteral("dry-build");
}

QVector<RebuildAction> allRebuildActions()
{
    return { RebuildAction::Switch, RebuildAction::Boot, RebuildAction::Test,
        RebuildAction::DryActivate, RebuildAction::DryBuild, RebuildAction::Build };
}

bool rebuildNeedsRoot(RebuildAction a)
{
    switch (a) {
    case RebuildAction::Switch:
    case RebuildAction::Boot:
    case RebuildAction::Test:
    case RebuildAction::DryActivate:
        return true;
    case RebuildAction::DryBuild:
    case RebuildAction::Build:
        return false;
    }
    return true;
}

CommandRunner::Step rebuild(RebuildAction action, const QString &root, const QString &host,
    bool isFlake, const QStringList &extraArgs)
{
    CommandRunner::Step s;
    s.label = QStringLiteral("nixos-rebuild %1").arg(rebuildActionName(action));
    s.program = QStringLiteral("nixos-rebuild");
    s.args << rebuildActionName(action);
    if (isFlake) {
        QString target = root;
        if (!host.isEmpty())
            target += QLatin1Char('#') + host;
        s.args << QStringLiteral("--flake") << target;
    }
    s.args << extraArgs;
    // `build` writes ./result into the working directory; keep that inside the
    // configuration tree rather than wherever the app happens to have started.
    s.workDir = root;
    s.privilege = rebuildNeedsRoot(action) ? CommandRunner::AsRoot : CommandRunner::AsUser;
    return s;
}

namespace {

/// Modification time of the link itself, not of what it points at. Store paths
/// all carry the epoch, so following the symlink would give a useless date.
QDateTime symlinkModified(const QString &path)
{
    struct stat info;
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0)
        return QDateTime();
    return QDateTime::fromSecsSinceEpoch(info.st_mtime);
}

void fillGenerationDetails(Generation *g)
{
    const QString link
        = QStringLiteral("%1-%2-link").arg(QLatin1String(kSystemProfile)).arg(g->number);

    QFile versionFile(link + QStringLiteral("/nixos-version"));
    if (versionFile.open(QIODevice::ReadOnly | QIODevice::Text))
        g->nixosVersion = QString::fromUtf8(versionFile.readAll()).trimmed();

    const QFileInfo kernel(link + QStringLiteral("/kernel"));
    if (kernel.isSymLink()) {
        // /nix/store/<hash>-linux-6.12.1/bzImage -> "linux-6.12.1"
        static const QRegularExpression kre(QStringLiteral("-(linux[^/]*?)(?:/|$)"));
        const auto km = kre.match(kernel.symLinkTarget());
        if (km.hasMatch())
            g->kernel = km.captured(1);
    }
}

/// Fallback for layouts the directory scan does not understand.
QVector<Generation> listGenerationsViaNixEnv(QString *diagnostic)
{
    QVector<Generation> out;
    if (QStandardPaths::findExecutable(QStringLiteral("nix-env")).isEmpty()) {
        if (diagnostic)
            *diagnostic = QObject::tr("nix-env is not on PATH.");
        return out;
    }

    int exitCode = 0;
    const QString text = CommandRunner::captureOutput(QStringLiteral("nix-env"),
        { QStringLiteral("--list-generations"), QStringLiteral("--profile"),
            QLatin1String(kSystemProfile) },
        QString(), 30000, &exitCode);
    if (exitCode != 0) {
        if (diagnostic)
            *diagnostic = text.trimmed();
        return out;
    }

    static const QRegularExpression re(
        QStringLiteral(R"(^\s*(\d+)\s+(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})\s*(\(current\))?)"));
    const auto lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const auto m = re.match(line);
        if (!m.hasMatch())
            continue;
        Generation g;
        g.number = m.captured(1).toInt();
        g.date = m.captured(2);
        g.current = !m.captured(3).isEmpty();
        fillGenerationDetails(&g);
        out.push_back(g);
    }
    return out;
}

} // namespace

QVector<Generation> listGenerations(QString *diagnostic)
{
    QVector<Generation> out;
    if (diagnostic)
        diagnostic->clear();

    const QFileInfo profileInfo{ QLatin1String(kSystemProfile) };
    const QDir profileDir = profileInfo.absoluteDir();
    if (!profileDir.exists()) {
        if (diagnostic)
            *diagnostic = QObject::tr("%1 does not exist — this machine is not running NixOS.")
                              .arg(profileDir.absolutePath());
        return out;
    }

    // `system` points at the live generation, e.g. "system-621-link".
    int currentNumber = -1;
    static const QRegularExpression linkRe(QStringLiteral("^system-(\\d+)-link$"));
    const QString currentTarget = QFileInfo(QLatin1String(kSystemProfile)).symLinkTarget();
    if (!currentTarget.isEmpty()) {
        const auto m = linkRe.match(QFileInfo(currentTarget).fileName());
        if (m.hasMatch())
            currentNumber = m.captured(1).toInt();
    }

    const auto entries = profileDir.entryInfoList({ QStringLiteral("system-*-link") },
        QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const auto m = linkRe.match(entry.fileName());
        if (!m.hasMatch())
            continue;
        Generation g;
        g.number = m.captured(1).toInt();
        const QDateTime built = symlinkModified(entry.absoluteFilePath());
        if (built.isValid())
            g.date = built.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        g.current = g.number == currentNumber;
        fillGenerationDetails(&g);
        out.push_back(g);
    }

    if (out.isEmpty()) {
        QString fallbackDiagnostic;
        out = listGenerationsViaNixEnv(&fallbackDiagnostic);
        if (out.isEmpty() && diagnostic) {
            *diagnostic = fallbackDiagnostic.isEmpty()
                ? QObject::tr("No system-*-link entries under %1.").arg(profileDir.absolutePath())
                : fallbackDiagnostic;
        }
        return out;
    }

    std::sort(out.begin(), out.end(),
        [](const Generation &a, const Generation &b) { return a.number > b.number; });
    return out;
}

QVector<CommandRunner::Step> rollbackTo(int generation)
{
    QVector<CommandRunner::Step> steps;

    CommandRunner::Step sw;
    sw.label = QStringLiteral("switch profile to generation %1").arg(generation);
    sw.program = QStringLiteral("nix-env");
    sw.args << QStringLiteral("--profile") << QLatin1String(kSystemProfile)
            << QStringLiteral("--switch-generation") << QString::number(generation);
    sw.privilege = CommandRunner::AsRoot;
    steps << sw;

    CommandRunner::Step activate;
    activate.label = QStringLiteral("activate generation %1").arg(generation);
    activate.program
        = QStringLiteral("%1/bin/switch-to-configuration").arg(QLatin1String(kSystemProfile));
    activate.args << QStringLiteral("switch");
    activate.privilege = CommandRunner::AsRoot;
    steps << activate;

    return steps;
}

CommandRunner::Step deleteGenerations(const QVector<int> &generations)
{
    CommandRunner::Step s;
    s.label = QStringLiteral("delete %1 generation(s)").arg(generations.size());
    s.program = QStringLiteral("nix-env");
    s.args << QStringLiteral("--profile") << QLatin1String(kSystemProfile)
           << QStringLiteral("--delete-generations");
    for (int g : generations)
        s.args << QString::number(g);
    s.privilege = CommandRunner::AsRoot;
    return s;
}

bool nixSupportsFlakeUpdateInput()
{
    static int cached = -1;
    if (cached >= 0)
        return cached == 1;

    int exitCode = 0;
    const QString out = CommandRunner::captureOutput(QStringLiteral("nix"),
        { QStringLiteral("--version") }, QString(), 10000, &exitCode);
    static const QRegularExpression re(QStringLiteral(R"((\d+)\.(\d+)(?:\.(\d+))?)"));
    const auto m = re.match(out);
    if (exitCode != 0 || !m.hasMatch()) {
        cached = 1;   // assume a recent Nix
        return true;
    }
    const QVersionNumber v(m.captured(1).toInt(), m.captured(2).toInt());
    cached = (v >= QVersionNumber(2, 19)) ? 1 : 0;
    return cached == 1;
}

CommandRunner::Step flakeUpdateAll(const QString &root, bool asRoot)
{
    CommandRunner::Step s;
    s.label = QStringLiteral("nix flake update");
    s.program = QStringLiteral("nix");
    s.args << QStringLiteral("flake") << QStringLiteral("update");
    s.workDir = root;
    s.privilege = asRoot ? CommandRunner::AsRoot : CommandRunner::AsUser;
    return s;
}

CommandRunner::Step flakeUpdateInput(const QString &root, const QString &input, bool asRoot)
{
    CommandRunner::Step s;
    s.label = QStringLiteral("update flake input '%1'").arg(input);
    s.program = QStringLiteral("nix");
    if (nixSupportsFlakeUpdateInput())
        s.args << QStringLiteral("flake") << QStringLiteral("update") << input;
    else
        s.args << QStringLiteral("flake") << QStringLiteral("lock")
               << QStringLiteral("--update-input") << input;
    s.workDir = root;
    s.privilege = asRoot ? CommandRunner::AsRoot : CommandRunner::AsUser;
    return s;
}

QHash<QString, QPair<QString, QString>> lockedInputs(const QString &root)
{
    QHash<QString, QPair<QString, QString>> out;

    QFile lock(QDir(root).absoluteFilePath(QStringLiteral("flake.lock")));
    QByteArray json;
    if (lock.open(QIODevice::ReadOnly)) {
        json = lock.readAll();
    } else {
        int exitCode = 0;
        const QString text = CommandRunner::captureOutput(QStringLiteral("nix"),
            { QStringLiteral("flake"), QStringLiteral("metadata"), QStringLiteral("--json"), root },
            root, 60000, &exitCode);
        if (exitCode != 0)
            return out;
        const auto doc = QJsonDocument::fromJson(text.toUtf8());
        json = QJsonDocument(doc.object().value(QStringLiteral("locks")).toObject()).toJson();
    }

    const auto doc = QJsonDocument::fromJson(json);
    const QJsonObject nodes = doc.object().value(QStringLiteral("nodes")).toObject();
    const QJsonObject rootNode
        = nodes.value(doc.object().value(QStringLiteral("root")).toString(QStringLiteral("root")))
              .toObject();
    const QJsonObject rootInputs = rootNode.value(QStringLiteral("inputs")).toObject();

    for (auto it = rootInputs.constBegin(); it != rootInputs.constEnd(); ++it) {
        const QString nodeName = it.value().isString()
            ? it.value().toString()
            : it.value().toArray().isEmpty() ? QString() : it.value().toArray().last().toString();
        if (nodeName.isEmpty())
            continue;
        const QJsonObject locked
            = nodes.value(nodeName).toObject().value(QStringLiteral("locked")).toObject();
        const QString rev = locked.value(QStringLiteral("rev")).toString();
        const qint64 modified = locked.value(QStringLiteral("lastModified")).toVariant().toLongLong();
        const QString when = modified > 0
            ? QDateTime::fromSecsSinceEpoch(modified).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
            : QString();
        out.insert(it.key(), { rev.left(12), when });
    }
    return out;
}

QVector<CommandRunner::Step> cleanup(const CleanupOptions &opts)
{
    QVector<CommandRunner::Step> steps;

    if (opts.deleteSystemGenerations) {
        CommandRunner::Step s;
        s.program = QStringLiteral("nix-env");
        s.args << QStringLiteral("--profile") << QLatin1String(kSystemProfile)
               << QStringLiteral("--delete-generations");
        if (opts.olderThanDays > 0) {
            s.args << QStringLiteral("%1d").arg(opts.olderThanDays);
            s.label = QStringLiteral("delete system generations older than %1 days")
                          .arg(opts.olderThanDays);
        } else {
            s.args << QStringLiteral("old");
            s.label = QStringLiteral("delete all but the current system generation");
        }
        if (opts.dryRun)
            s.args << QStringLiteral("--dry-run");
        s.privilege = CommandRunner::AsRoot;
        steps << s;
    }

    if (opts.userProfiles) {
        CommandRunner::Step s;
        s.label = QStringLiteral("delete old user profile generations");
        s.program = QStringLiteral("nix-env");
        s.args << QStringLiteral("--delete-generations");
        if (opts.olderThanDays > 0)
            s.args << QStringLiteral("%1d").arg(opts.olderThanDays);
        else
            s.args << QStringLiteral("old");
        if (opts.dryRun)
            s.args << QStringLiteral("--dry-run");
        s.privilege = CommandRunner::AsUser;
        steps << s;
    }

    if (opts.collectGarbage) {
        CommandRunner::Step s;
        s.label = QStringLiteral("collect garbage");
        s.program = QStringLiteral("nix-collect-garbage");
        if (opts.olderThanDays > 0)
            s.args << QStringLiteral("--delete-older-than")
                   << QStringLiteral("%1d").arg(opts.olderThanDays);
        else
            s.args << QStringLiteral("-d");
        if (opts.dryRun)
            s.args << QStringLiteral("--dry-run");
        s.privilege = CommandRunner::AsRoot;
        steps << s;
    }

    if (opts.optimiseStore && !opts.dryRun) {
        CommandRunner::Step s;
        s.label = QStringLiteral("optimise the store (hard-link duplicates)");
        s.program = QStringLiteral("nix");
        s.args << QStringLiteral("store") << QStringLiteral("optimise");
        s.privilege = CommandRunner::AsRoot;
        steps << s;
    }

    return steps;
}

QString storeSize()
{
    int exitCode = 0;
    const QString out = CommandRunner::captureOutput(QStringLiteral("du"),
        { QStringLiteral("-sh"), QStringLiteral("/nix/store") }, QString(), 120000, &exitCode);
    if (exitCode != 0)
        return QString();
    return out.section(QLatin1Char('\t'), 0, 0).trimmed();
}

CommandRunner::Step installFileAsRoot(const QString &tempPath, const QString &destination)
{
    CommandRunner::Step s;
    s.label = QStringLiteral("write %1").arg(destination);
    s.program = QStringLiteral("install");
    s.args << QStringLiteral("-m") << QStringLiteral("0644") << QStringLiteral("-o")
           << QStringLiteral("root") << QStringLiteral("-g") << QStringLiteral("root") << tempPath
           << destination;
    s.privilege = CommandRunner::AsRoot;
    return s;
}

} // namespace SystemOps
} // namespace nixm
