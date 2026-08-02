#include "ConfigProject.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>

namespace nixm {
namespace {

const QStringList kSkippedDirs = { QStringLiteral(".git"), QStringLiteral("result"),
    QStringLiteral(".direnv"), QStringLiteral("node_modules") };

/// Files that are machine-generated or per-host and should not show up as
/// reusable modules.
bool isModuleCandidate(const QString &relPath)
{
    if (relPath == QLatin1String("flake.nix"))
        return false;
    if (relPath.endsWith(QLatin1String("hardware-configuration.nix")))
        return false;
    if (relPath.startsWith(QLatin1String("hosts/")))
        return false;
    return relPath.endsWith(QLatin1String(".nix"));
}

} // namespace

ConfigProject::ConfigProject(QObject *parent)
    : QObject(parent)
{
}

ConfigProject::~ConfigProject()
{
    close();
}

void ConfigProject::close()
{
    qDeleteAll(m_files);
    m_files.clear();
    m_hosts.clear();
    m_modules.clear();
    m_inputs.clear();
    m_globalModules.clear();
    m_kind = None;
    m_root.clear();
    m_flakeFile.clear();
}

bool ConfigProject::open(const QString &rootPath, QString *err)
{
    QFileInfo info(rootPath);
    QString dir = rootPath;
    QString singleFile;

    if (info.isFile()) {
        singleFile = info.absoluteFilePath();
        dir = info.absolutePath();
    }

    QDir d(dir);
    if (!d.exists()) {
        if (err)
            *err = tr("No such directory: %1").arg(dir);
        return false;
    }

    close();
    m_root = d.absolutePath();
    m_writable = QFileInfo(m_root).isWritable();

    if (!singleFile.isEmpty() && QFileInfo(singleFile).fileName() != QLatin1String("flake.nix")) {
        m_kind = SingleFile;
    } else if (d.exists(QStringLiteral("flake.nix"))) {
        m_kind = Flake;
        m_flakeFile = d.absoluteFilePath(QStringLiteral("flake.nix"));
    } else if (d.exists(QStringLiteral("configuration.nix"))) {
        m_kind = SingleFile;
    } else {
        if (err)
            *err = tr("%1 contains neither flake.nix nor configuration.nix.").arg(m_root);
        m_kind = None;
        return false;
    }

    if (m_kind == SingleFile && singleFile.isEmpty())
        singleFile = d.absoluteFilePath(QStringLiteral("configuration.nix"));

    if (m_kind == Flake) {
        scanFlakeInputs();
        scanHosts();
    } else {
        HostInfo h;
        h.entryFile = singleFile;
        h.dir = QFileInfo(singleFile).absolutePath();
        h.relEntry = QDir(m_root).relativeFilePath(singleFile);
        NixFile *f = file(singleFile);
        if (f) {
            if (const AttrEntry *a = f->findAttr(QStringLiteral("networking.hostName")))
                h.name = a->unquoted();
        }
        if (h.name.isEmpty())
            h.name = QStringLiteral("nixos");
        m_hosts.push_back(h);
    }

    scanModules();
    emit changed();
    return true;
}

void ConfigProject::rescan()
{
    if (m_kind == None)
        return;
    m_hosts.clear();
    m_modules.clear();
    m_inputs.clear();
    m_globalModules.clear();
    if (m_kind == Flake) {
        scanFlakeInputs();
        scanHosts();
    } else if (!m_root.isEmpty()) {
        const QString entry = QDir(m_root).absoluteFilePath(QStringLiteral("configuration.nix"));
        HostInfo h;
        h.entryFile = entry;
        h.dir = m_root;
        h.relEntry = QStringLiteral("configuration.nix");
        h.name = QStringLiteral("nixos");
        if (NixFile *f = file(entry)) {
            if (const AttrEntry *a = f->findAttr(QStringLiteral("networking.hostName")))
                h.name = a->unquoted();
        }
        m_hosts.push_back(h);
    }
    scanModules();
    emit changed();
}

void ConfigProject::scanFlakeInputs()
{
    NixFile *flake = file(m_flakeFile);
    if (!flake)
        return;

    static const QRegularExpression inputUrl(QStringLiteral("^inputs\\.(.+)\\.url$"));
    for (const AttrEntry &a : flake->attrs()) {
        const auto m = inputUrl.match(a.path);
        if (!m.hasMatch())
            continue;
        FlakeInput in;
        in.name = m.captured(1);
        in.url = a.unquoted();
        m_inputs.push_back(in);
    }

    for (const FlakeInput &in : m_inputs) {
        if (in.name != QLatin1String("nixpkgs"))
            continue;
        // "github:nixos/nixpkgs/nixos-unstable" -> "nixos-unstable"
        const QString ref = in.url.section(QLatin1Char('/'), -1);
        if (ref.startsWith(QLatin1String("nixos-")))
            m_channel = ref;
        else if (ref.startsWith(QLatin1String("nixpkgs-unstable")))
            m_channel = QStringLiteral("nixos-unstable");
        break;
    }
}

void ConfigProject::scanHosts()
{
    NixFile *flake = file(m_flakeFile);
    if (!flake)
        return;

    static const QRegularExpression hostAttr(
        QStringLiteral("(?:^|\\.)nixosConfigurations\\.([^.]+)$"));

    for (const AttrEntry &a : flake->attrs()) {
        const auto m = hostAttr.match(a.path);
        if (!m.hasMatch())
            continue;

        HostInfo h;
        h.name = m.captured(1);

        // The host's entry point is the first path literal in the value, which
        // covers both `mkHost ./hosts/x` and an inline `modules = [ ./hosts/x ]`.
        const auto valueTokens = tokenize(a.rawValue);
        QString literal;
        for (const Token &t : valueTokens) {
            if (t.kind == TokKind::Path) {
                literal = t.text;
                break;
            }
        }
        if (literal.isEmpty())
            literal = QStringLiteral("./hosts/") + h.name;

        const QString resolved = resolveNixPath(m_flakeFile, literal);
        QFileInfo ri(resolved);
        if (ri.isDir()) {
            h.dir = ri.absoluteFilePath();
            h.entryFile = QDir(h.dir).absoluteFilePath(QStringLiteral("default.nix"));
        } else {
            h.entryFile = ri.absoluteFilePath();
            h.dir = ri.absolutePath();
        }
        h.relEntry = QDir(m_root).relativeFilePath(h.entryFile);
        m_hosts.push_back(h);
    }

    // Fall back to the conventional layout when the flake could not be read.
    if (m_hosts.isEmpty()) {
        QDir hostsDir(QDir(m_root).absoluteFilePath(QStringLiteral("hosts")));
        if (hostsDir.exists()) {
            const auto entries = hostsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &name : entries) {
                HostInfo h;
                h.name = name;
                h.dir = hostsDir.absoluteFilePath(name);
                h.entryFile = QDir(h.dir).absoluteFilePath(QStringLiteral("default.nix"));
                h.relEntry = QDir(m_root).relativeFilePath(h.entryFile);
                m_hosts.push_back(h);
            }
        }
    }

    std::sort(m_hosts.begin(), m_hosts.end(),
        [](const HostInfo &a, const HostInfo &b) { return a.name < b.name; });

    // A `mkHost` helper usually carries a shared `modules = [ … ]` list; those
    // paths apply to every host even though no host imports them directly.
    QStringList hostEntries;
    for (const HostInfo &h : std::as_const(m_hosts))
        hostEntries << h.entryFile << h.dir;

    for (const NixList &l : flake->lists()) {
        if (!l.path.endsWith(QLatin1String("modules"))
            && !l.path.endsWith(QLatin1String("imports")))
            continue;
        for (const PackageEntry &e : l.entries) {
            if (!e.enabled || !e.expr.contains(QLatin1Char('/')))
                continue;
            const QString abs
                = QFileInfo(resolveNixPath(m_flakeFile, e.expr)).absoluteFilePath();
            if (abs.isEmpty() || hostEntries.contains(abs) || m_globalModules.contains(abs))
                continue;
            if (QFileInfo::exists(abs))
                m_globalModules << abs;
        }
    }
}

bool ConfigProject::isGlobalModule(const QString &absPath) const
{
    return m_globalModules.contains(QFileInfo(absPath).absoluteFilePath());
}

void ConfigProject::scanModules()
{
    // A host's own entry file is not a module you can import into that host —
    // offering it would let you add `./configuration.nix` to its own imports,
    // which is an infinite recursion at evaluation time.
    QStringList entryFiles;
    for (const HostInfo &h : std::as_const(m_hosts))
        entryFiles << h.entryFile;

    QDirIterator it(m_root, QStringList() << QStringLiteral("*.nix"), QDir::Files,
        QDirIterator::Subdirectories);
    QDir rootDir(m_root);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QString rel = rootDir.relativeFilePath(abs);

        bool skipped = false;
        for (const QString &bad : kSkippedDirs) {
            if (rel.startsWith(bad + QLatin1Char('/')) || rel.contains(QLatin1Char('/') + bad + QLatin1Char('/'))) {
                skipped = true;
                break;
            }
        }
        if (skipped || !isModuleCandidate(rel) || entryFiles.contains(abs))
            continue;

        ModuleInfo m;
        m.absPath = abs;
        m.relPath = rel;
        m.name = QFileInfo(abs).completeBaseName();

        QString dirPart = QFileInfo(rel).path();   // "modules/desktop" or "."
        if (dirPart == QLatin1String("."))
            dirPart.clear();
        if (dirPart.startsWith(QLatin1String("modules/")))
            dirPart = dirPart.mid(8);
        else if (dirPart == QLatin1String("modules"))
            dirPart.clear();
        m.category = dirPart.isEmpty() ? tr("General") : dirPart;

        m_modules.push_back(m);
    }

    std::sort(m_modules.begin(), m_modules.end(), [](const ModuleInfo &a, const ModuleInfo &b) {
        if (a.category != b.category)
            return a.category < b.category;
        return a.name < b.name;
    });
}

const HostInfo *ConfigProject::host(const QString &name) const
{
    for (const HostInfo &h : m_hosts)
        if (h.name == name)
            return &h;
    return nullptr;
}

QVector<OptionDecl> ConfigProject::declaredOptions()
{
    QVector<OptionDecl> out;
    for (const ModuleInfo &m : m_modules) {
        NixFile *f = file(m.absPath);
        if (!f)
            continue;
        for (const OptionDecl &o : f->optionDecls())
            out.push_back(o);
    }
    std::sort(out.begin(), out.end(),
        [](const OptionDecl &a, const OptionDecl &b) { return a.path < b.path; });
    return out;
}

NixFile *ConfigProject::file(const QString &absPath)
{
    const QString key = QFileInfo(absPath).absoluteFilePath();
    auto it = m_files.constFind(key);
    if (it != m_files.constEnd())
        return it.value();

    if (!QFileInfo::exists(key))
        return nullptr;

    auto *f = new NixFile(key);
    if (!f->load()) {
        delete f;
        return nullptr;
    }
    m_files.insert(key, f);
    return f;
}

QVector<NixFile *> ConfigProject::openFiles() const
{
    QVector<NixFile *> out;
    for (NixFile *f : m_files)
        out.push_back(f);
    std::sort(out.begin(), out.end(),
        [](NixFile *a, NixFile *b) { return a->path() < b->path(); });
    return out;
}

QVector<NixFile *> ConfigProject::dirtyFiles() const
{
    QVector<NixFile *> out;
    for (NixFile *f : m_files)
        if (f->isDirty())
            out.push_back(f);
    return out;
}

bool ConfigProject::hasUnsavedChanges() const
{
    for (NixFile *f : m_files)
        if (f->isDirty())
            return true;
    return false;
}

bool ConfigProject::saveAll(QVector<QPair<QString, QString>> *needsPrivilege, QString *err)
{
    bool ok = true;
    for (NixFile *f : m_files) {
        if (!f->isDirty())
            continue;
        QString localErr;
        bool denied = false;
        if (f->save(&localErr, &denied))
            continue;

        if (denied && needsPrivilege) {
            QString tmpErr;
            const QString tmp = f->writeToTemp(&tmpErr);
            if (!tmp.isEmpty()) {
                needsPrivilege->push_back({ tmp, f->path() });
                continue;
            }
            localErr += QLatin1Char(' ') + tmpErr;
        }
        ok = false;
        if (err)
            *err += localErr + QLatin1Char('\n');
    }
    return ok;
}

void ConfigProject::reloadAll()
{
    for (NixFile *f : m_files)
        f->load();
    rescan();
}

QString ConfigProject::resolveNixPath(const QString &fromFile, const QString &literal)
{
    QString lit = literal.trimmed();
    if (lit.startsWith(QLatin1Char('<')))
        return QString();
    if (lit.startsWith(QLatin1Char('/')))
        return QDir::cleanPath(lit);
    if (lit.startsWith(QLatin1String("~/")))
        return QDir::cleanPath(QDir::homePath() + lit.mid(1));
    const QString base = QFileInfo(fromFile).absolutePath();
    return QDir::cleanPath(base + QLatin1Char('/') + lit);
}

QString ConfigProject::relativeNixPath(const QString &fromFile, const QString &targetFile)
{
    const QString base = QFileInfo(fromFile).absolutePath();
    QString rel = QDir(base).relativeFilePath(QFileInfo(targetFile).absoluteFilePath());
    if (rel.isEmpty())
        rel = QStringLiteral(".");
    if (!rel.startsWith(QLatin1String("./")) && !rel.startsWith(QLatin1String("../")))
        rel = QStringLiteral("./") + rel;
    return rel;
}

} // namespace nixm
