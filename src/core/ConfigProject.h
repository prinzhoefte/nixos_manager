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
