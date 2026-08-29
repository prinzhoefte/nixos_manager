#include "ConfigProject.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
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

/// True for the literals that name a file on disk. `<nixpkgs/…>` and
/// `inputs.foo.nixosModules.bar` name something else entirely.
bool isPathLiteral(const QString &literal)
{
    const QString t = literal.trimmed();
    return t.startsWith(QLatin1String("./")) || t.startsWith(QLatin1String("../"))
        || t.startsWith(QLatin1Char('/')) || t.startsWith(QLatin1String("~/"));
}

/// True when the path literal `literal`, written in `fromFile`, ends up at
/// `target` — either directly, through the `default.nix` a directory literal
/// stands for, or because `target` is a directory the literal points inside.
bool literalHits(const QString &fromFile, const QString &literal, const QString &target)
{
    if (!isPathLiteral(literal))
        return false;
    const QString resolved = ConfigProject::resolveNixPath(fromFile, literal);
    if (resolved.isEmpty())
        return false;
    const QString abs = QFileInfo(resolved).absoluteFilePath();
    if (abs == target)
        return true;
    if (QDir(abs).absoluteFilePath(QStringLiteral("default.nix")) == target)
        return true;
    return abs.startsWith(target + QLatin1Char('/'));
}

/// Removes `path` from disk. `*permissionDenied` tells the caller the removal
/// is worth retrying as root rather than reporting as a plain failure.
bool erasePath(const QString &path, bool toTrash, bool *movedToTrash, bool *permissionDenied,
    QString *err)
{
    QFileInfo info(path);
    if (movedToTrash)
        *movedToTrash = false;
    if (permissionDenied)
        *permissionDenied = false;

    if (toTrash && QFile::moveToTrash(path)) {
        if (movedToTrash)
            *movedToTrash = true;
        return true;
    }
    // No trash on this filesystem, or the tree is not ours: fall through to a
    // real removal and let the caller decide about privileges.

    if (info.isDir()) {
        if (QDir(path).removeRecursively())
            return true;
        if (err)
            *err = QCoreApplication::translate("nixm::ConfigProject",
                "%1 could not be removed completely.").arg(path);
    } else {
        QFile f(path);
        if (f.remove())
            return true;
        if (err)
            *err = QStringLiteral("%1: %2").arg(path, f.errorString());
    }

    if (permissionDenied)
        *permissionDenied = !QFileInfo(info.absolutePath()).isWritable();
    return false;
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

bool ConfigProject::containsPath(const QString &absPath) const
{
    if (m_root.isEmpty() || absPath.isEmpty())
        return false;
    const QString abs = QDir::cleanPath(QFileInfo(absPath).absoluteFilePath());
    const QString root = QDir::cleanPath(m_root);
    return abs.startsWith(root + QLatin1Char('/'));
}

QVector<FileReference> ConfigProject::referencesTo(const QString &absPath)
{
    QVector<FileReference> out;
    if (m_kind == None || absPath.isEmpty())
        return out;

    const QString target = QDir::cleanPath(QFileInfo(absPath).absoluteFilePath());

    // Everything that can carry an import: the flake, the host entry points and
    // the modules themselves, since a module may pull in a sibling.
    QStringList candidates;
    if (!m_flakeFile.isEmpty())
        candidates << m_flakeFile;
    for (const HostInfo &h : std::as_const(m_hosts))
        candidates << h.entryFile;
    for (const ModuleInfo &m : std::as_const(m_modules))
        candidates << m.absPath;
    candidates.removeDuplicates();

    const QDir rootDir(m_root);
    for (const QString &candidate : std::as_const(candidates)) {
        const QString from = QDir::cleanPath(QFileInfo(candidate).absoluteFilePath());
        // A file's own imports go away with the file; and a file inside the
        // directory being deleted is not worth editing either.
        if (from == target || from.startsWith(target + QLatin1Char('/')))
            continue;

        NixFile *f = file(from);
        if (!f)
            continue;

        const QString rel = rootDir.relativeFilePath(from);

        // Char ranges already accounted for, so a literal inside a list is not
        // reported a second time as part of the binding that encloses the list.
        QVector<QPair<int, int>> covered;

        for (const ImportEntry &e : f->imports()) {
            if (!literalHits(from, e.text, target))
                continue;
            FileReference r;
            r.file = from;
            r.relFile = rel;
            r.owner = QStringLiteral("imports");
            r.literal = e.text;
            r.kind = ReferenceKind::Import;
            r.enabled = e.enabled;
            r.line = e.line;
            out.push_back(r);
            covered.push_back({ e.start, qMax(e.endFull, e.end) });
        }

        for (const NixList &l : f->lists()) {
            // The top-level `imports` list is the one above.
            if (l.path == QLatin1String("imports"))
                continue;
            if (!l.path.endsWith(QLatin1String("modules"))
                && !l.path.endsWith(QLatin1String("imports")))
                continue;
            for (const PackageEntry &e : l.entries) {
                if (!literalHits(from, e.expr, target))
                    continue;
                FileReference r;
                r.file = from;
                r.relFile = rel;
                r.owner = l.path;
                r.literal = e.expr;
                r.kind = ReferenceKind::ListEntry;
                r.enabled = e.enabled;
                r.line = e.line;
                out.push_back(r);
                covered.push_back({ e.start, qMax(e.lineEnd, e.end) });
            }
        }

        // Paths written into a plain binding — `main-pc = mkHost ./hosts/main-pc;`
        // is how a flake names its hosts. Nothing here can rewrite the
        // surrounding expression, but the user has to hear about it.
        //
        // Bindings nest (`outputs` contains every host binding), so each literal
        // is credited to the innermost binding that holds it, keyed by where it
        // sits in the file.
        struct BindingHit {
            int span = 0;
            QString owner;
            QString literal;
            int line = 0;
        };
        QMap<int, BindingHit> bindings;

        for (const AttrEntry &a : f->attrs()) {
            const auto valueTokens = tokenize(a.rawValue);
            for (const Token &t : valueTokens) {
                if (t.kind != TokKind::Path || !literalHits(from, t.text, target))
                    continue;
                const int offset = a.valueStart + t.start;
                bool alreadyCovered = false;
                for (const auto &range : std::as_const(covered)) {
                    if (offset >= range.first && offset < range.second) {
                        alreadyCovered = true;
                        break;
                    }
                }
                if (alreadyCovered)
                    continue;

                const int span = a.valueEnd - a.valueStart;
                const auto existing = bindings.constFind(offset);
                if (existing != bindings.constEnd() && existing->span <= span)
                    continue;
                bindings.insert(offset, { span, a.path, t.text, a.line });
            }
        }

        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            FileReference r;
            r.file = from;
            r.relFile = rel;
            r.owner = it->owner;
            r.literal = it->literal;
            r.kind = ReferenceKind::Binding;
            r.line = it->line;
            out.push_back(r);
        }
    }
    return out;
}

QString ConfigProject::describeRole(const QString &absPath) const
{
    const QString abs = QDir::cleanPath(QFileInfo(absPath).absoluteFilePath());
    if (abs.isEmpty())
        return QString();
    if (!m_flakeFile.isEmpty() && abs == QDir::cleanPath(m_flakeFile))
        return tr("the flake entry point — the whole configuration is defined through it");

    for (const HostInfo &h : m_hosts) {
        if (abs == QDir::cleanPath(h.entryFile))
            return tr("the entry file of host %1").arg(h.name);
        if (abs == QDir::cleanPath(h.dir))
            return tr("the directory of host %1").arg(h.name);
        if (abs.startsWith(QDir::cleanPath(h.dir) + QLatin1Char('/')))
            return tr("part of host %1").arg(h.name);
    }
    if (isGlobalModule(abs))
        return tr("a module every host gets through flake.nix");
    return QString();
}

QStringList ConfigProject::nixFilesUnder(const QString &absPath) const
{
    QStringList out;
    const QFileInfo info(absPath);
    if (!info.exists())
        return out;
    if (info.isFile()) {
        out << info.absoluteFilePath();
        return out;
    }
    QDirIterator it(info.absoluteFilePath(), QStringList() << QStringLiteral("*.nix"), QDir::Files,
        QDirIterator::Subdirectories);
    while (it.hasNext())
        out << it.next();
    out.sort();
    return out;
}

void ConfigProject::forgetFile(const QString &absPath)
{
    const QString abs = QDir::cleanPath(QFileInfo(absPath).absoluteFilePath());
    if (abs.isEmpty())
        return;
    const QString prefix = abs + QLatin1Char('/');
    for (auto it = m_files.begin(); it != m_files.end();) {
        const QString key = QDir::cleanPath(it.key());
        if (key == abs || key.startsWith(prefix)) {
            delete it.value();
            it = m_files.erase(it);
        } else {
            ++it;
        }
    }
}

bool ConfigProject::deleteFiles(const DeleteRequest &request, DeleteResult *result)
{
    DeleteResult discarded;
    DeleteResult &res = result ? *result : discarded;

    QStringList targets;
    for (const QString &raw : request.paths) {
        const QString abs = QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
        if (abs.isEmpty())
            continue;
        if (!containsPath(abs)) {
            res.errors << tr("%1 is outside %2 and was left alone.").arg(abs, m_root);
            continue;
        }
        if (!QFileInfo::exists(abs)) {
            res.errors << tr("%1 no longer exists.").arg(abs);
            continue;
        }
        if (!targets.contains(abs))
            targets << abs;
    }
    if (targets.isEmpty())
        return false;

    // Rewrite the references first: once a file is gone the parser can no
    // longer tell us where it was mentioned.
    if (request.references != ReferenceAction::Leave) {
        for (const QString &target : std::as_const(targets)) {
            const QVector<FileReference> refs = referencesTo(target);
            for (const FileReference &r : refs) {
                if (!r.rewritable()) {
                    // `x = mkHost ./hosts/x;` and friends: only the caller knows
                    // what the binding as a whole should become.
                    const QString note = QStringLiteral("%1: %2").arg(r.relFile, r.owner);
                    if (!res.leftBehind.contains(note))
                        res.leftBehind << note;
                    continue;
                }
                NixFile *f = file(r.file);
                if (!f)
                    continue;
                const bool isImport = r.kind == ReferenceKind::Import;
                bool ok = false;
                if (request.references == ReferenceAction::Remove) {
                    ok = isImport ? f->removeImport(r.literal)
                                  : f->removePackage(r.owner, r.literal);
                } else if (!r.enabled) {
                    ok = true;   // already commented out
                } else {
                    ok = isImport ? f->setImportEnabled(r.literal, false)
                                  : f->setPackageEnabled(r.owner, r.literal, false);
                }
                if (!ok) {
                    res.errors << tr("Could not update the reference to %1 in %2.")
                                      .arg(r.literal, r.relFile);
                } else if (!res.editedFiles.contains(r.file)) {
                    res.editedFiles << r.file;
                }
            }
        }
    }

    for (const QString &target : std::as_const(targets)) {
        bool trashed = false;
        bool denied = false;
        QString err;
        if (erasePath(target, request.toTrash, &trashed, &denied, &err)) {
            res.deleted << target;
            if (trashed)
                res.trashed << target;
            forgetFile(target);
        } else if (denied) {
            res.needsPrivilege << target;
        } else {
            res.errors << err;
        }
    }

    rescan();
    return !res.deleted.isEmpty() || !res.editedFiles.isEmpty();
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
