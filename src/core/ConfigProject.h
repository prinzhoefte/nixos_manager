#pragma once

#include "NixFile.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace nixm {

struct HostInfo {
    QString name;        // "main-pc"
    QString dir;         // absolute path of the host directory (may equal the file's dir)
    QString entryFile;   // absolute path of default.nix / configuration.nix
    QString relEntry;    // path relative to the project root
};

struct ModuleInfo {
    QString absPath;
    QString relPath;     // "modules/desktop/plasma6.nix"
    QString category;    // "desktop"
    QString name;        // "plasma6"
};

struct FlakeInput {
    QString name;
    QString url;
    QString lockedRev;
    QString lastModified;
};

/// How a reference is written, which decides whether it can be rewritten
/// without guessing at the surrounding expression.
enum class ReferenceKind {
    Import,      ///< an entry of the file's own top-level `imports` list
    ListEntry,   ///< an entry of another list, e.g. the flake's shared `modules`
    Binding      ///< a path inside a plain binding (`main-pc = mkHost ./hosts/main-pc`)
};

/// One place in the tree that points at a file.
struct FileReference {
    QString file;        // absolute path of the file holding the reference
    QString relFile;     // that path relative to the project root
    QString owner;       // "imports", "outputs.…modules", "nixosConfigurations.t420"
    QString literal;     // the path literal exactly as written
    ReferenceKind kind = ReferenceKind::Import;
    bool enabled = true; // false when the entry is commented out
    int line = 0;

    /// True when removing or commenting the reference out is a safe edit.
    bool rewritable() const { return kind != ReferenceKind::Binding; }
};

/// What to do with the references to a file that is being deleted.
enum class ReferenceAction {
    Leave,      ///< change nothing; the tree will not evaluate until fixed
    Disable,    ///< comment the entry out, the way unticking a module does
    Remove      ///< delete the entry
};

struct DeleteRequest {
    QStringList paths;                                  ///< absolute files or directories
    bool toTrash = true;                                ///< move to the desktop trash if possible
    ReferenceAction references = ReferenceAction::Remove;
};

struct DeleteResult {
    QStringList deleted;          ///< paths that are gone from disk
    QStringList trashed;          ///< subset of `deleted` that went to the trash
    QStringList needsPrivilege;   ///< paths a plain removal was refused for
    QStringList editedFiles;      ///< buffers whose references were rewritten
    QStringList leftBehind;       ///< references that have to be fixed by hand
    QStringList errors;
};

/// A NixOS configuration tree: either a flake with several `nixosConfigurations`
/// or a plain `configuration.nix`. Owns the `NixFile` buffers so that every page
/// of the UI edits the same in-memory state.
class ConfigProject : public QObject
{
    Q_OBJECT

public:
    enum Kind { None, Flake, SingleFile };

    explicit ConfigProject(QObject *parent = nullptr);
    ~ConfigProject() override;

    bool open(const QString &rootPath, QString *err);
    void close();

    Kind kind() const { return m_kind; }
    QString root() const { return m_root; }
    QString flakeFile() const { return m_flakeFile; }
    bool isOpen() const { return m_kind != None; }
    /// False when the tree is root-owned; saving then needs privilege escalation.
    bool isWritable() const { return m_writable; }

    const QVector<HostInfo> &hosts() const { return m_hosts; }
    const QVector<ModuleInfo> &modules() const { return m_modules; }
    const QVector<FlakeInput> &flakeInputs() const { return m_inputs; }
    const HostInfo *host(const QString &name) const;

    /// Options declared by any module in the tree (`lib.mkOption`).
    QVector<OptionDecl> declaredOptions();

    /// Modules the flake adds to every host (e.g. the shared `modules = [ … ]`
    /// inside a `mkHost` helper), as absolute paths. These are active on all
    /// hosts without appearing in any host's own `imports`.
    QStringList globalModules() const { return m_globalModules; }
    bool isGlobalModule(const QString &absPath) const;

    /// Loads (and caches) a file. Returns nullptr if it cannot be read.
    NixFile *file(const QString &absPath);
    /// Files currently held in memory.
    QVector<NixFile *> openFiles() const;
    QVector<NixFile *> dirtyFiles() const;
    bool hasUnsavedChanges() const;

    /// Saves every dirty buffer. Paths that could not be written are reported in
    /// `needsPrivilege` together with the temp file holding the new contents.
    bool saveAll(QVector<QPair<QString, QString>> *needsPrivilege, QString *err);
    void reloadAll();

    /// Path of `relTo`'s directory expressed as a Nix path literal for `from`.
    static QString relativeNixPath(const QString &fromFile, const QString &targetFile);
    /// Resolves a Nix path literal appearing in `fromFile` to an absolute path.
    static QString resolveNixPath(const QString &fromFile, const QString &literal);

    /// The nixpkgs channel this configuration tracks, e.g. "nixos-unstable".
    QString nixpkgsChannel() const { return m_channel; }

    /// Rescans the tree for hosts and modules without dropping edits.
    void rescan();

    // ── File management ──────────────────────────────────────────────────────

    /// True when `absPath` lies inside the project root. The root itself does
    /// not count, so nothing here can ever delete the tree it is managing.
    bool containsPath(const QString &absPath) const;

    /// Every path literal in the tree that resolves to `absPath`: import lists,
    /// module lists and plain bindings alike. A directory matches both a literal
    /// naming it (Nix reads its `default.nix`) and any literal naming a file
    /// inside it.
    QVector<FileReference> referencesTo(const QString &absPath);

    /// What `absPath` is used for, as a short phrase for confirmation prompts
    /// ("the entry file of host t420"). Empty when it plays no special role.
    QString describeRole(const QString &absPath) const;

    /// The `.nix` files at or below `absPath`, for previewing a deletion.
    QStringList nixFilesUnder(const QString &absPath) const;

    /// Deletes files and directories, first rewriting the imports that point at
    /// them. Everything that happened — or could not happen — is reported in
    /// `result`; paths in `result->needsPrivilege` need an elevated `rm`, which
    /// the caller runs through the CommandRunner.
    bool deleteFiles(const DeleteRequest &request, DeleteResult *result);

    /// Drops the cached buffer for `absPath` and, when it is a directory, for
    /// everything below it. Call this after removing a path behind our back.
    void forgetFile(const QString &absPath);

signals:
    void changed();

private:
    void scanHosts();
    void scanModules();
    void scanFlakeInputs();

    Kind m_kind = None;
    QString m_root;
    QString m_flakeFile;
    QString m_channel = QStringLiteral("nixos-unstable");
    bool m_writable = true;

    QVector<HostInfo> m_hosts;
    QVector<ModuleInfo> m_modules;
    QVector<FlakeInput> m_inputs;
    QStringList m_globalModules;
    QHash<QString, NixFile *> m_files;
};

} // namespace nixm
