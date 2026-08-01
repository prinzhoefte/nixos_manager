#pragma once

#include "NixLexer.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace nixm {

/// One entry of an `imports = [ ... ];` list. Commented-out entries are kept,
/// because commenting a module out is how NixOS configs usually "disable" it.
struct ImportEntry {
    QString text;               // "../../modules/common/nix.nix"
    QString comment;            // trailing note, without the '#'
    QString section;            // nearest preceding "# ── Section ──" header
    bool enabled = true;        // false when the entry is commented out
    int start = 0;              // char range of the entry (or of the whole comment)
    int end = 0;                // end of the path itself
    int endFull = 0;            // end of the line, past any trailing comment
    int line = 0;
};

/// One entry of a package list.
struct PackageEntry {
    QString expr;               // "vim", "kdePackages.filelight", "(python3.withPackages …)"
    QString comment;            // trailing "# …" comment on the same line, without the '#'
    bool enabled = true;
    int start = 0;
    int end = 0;                // end of the expression itself
    int lineEnd = 0;            // end of the line (past any trailing comment)
    int line = 0;
};

/// A list-valued binding, e.g. `environment.systemPackages = with pkgs; [ … ];`
struct NixList {
    QString path;               // "environment.systemPackages"
    QString withExpr;           // "pkgs", "pkgs.kdePackages" or empty
    int lbracket = 0;           // char position of '['
    int rbracket = 0;           // char position of ']'
    int stmtStart = 0;          // char position where the binding starts
    int stmtEnd = 0;            // char position just past the terminating ';'
    QVector<PackageEntry> entries;
    bool isPackageList = false;

    /// Fully-qualified attribute for an entry, taking `with` into account.
    QString qualify(const QString &expr) const;
};

/// A scalar (or otherwise non-attrset) binding.
struct AttrEntry {
    QString path;               // "networking.hostName"
    QString rawValue;           // "\"main-pc\""
    int valueStart = 0;
    int valueEnd = 0;
    int stmtStart = 0;
    int stmtEnd = 0;
    int line = 0;

    /// Value with surrounding quotes removed, for string values.
    QString unquoted() const;
};

/// An option declared by a module via `lib.mkOption`.
struct OptionDecl {
    QString path;               // "nixos.pkgs.wallpaper-engine-kde-plugin.enable"
    QString type;               // best effort: "bool", "str", …
    QString defaultValue;
    QString description;
    QString file;               // absolute path of the declaring file
};

/// A single `.nix` file: text buffer, parsed views over it, and range-precise
/// mutations. Every mutation rewrites only the bytes it has to, so formatting,
/// alignment and comments elsewhere in the file are untouched.
class NixFile
{
public:
    NixFile() = default;
    explicit NixFile(const QString &path);

    bool load(QString *err = nullptr);
    /// Writes the buffer back to disk. Returns false and sets *err on failure;
    /// *permissionDenied tells the caller it may retry with elevated privileges.
    bool save(QString *err = nullptr, bool *permissionDenied = nullptr);
    /// Writes the buffer to a temporary file and returns its path, for callers
    /// that need to install it with `pkexec`.
    QString writeToTemp(QString *err = nullptr) const;

    QString path() const { return m_path; }
    void setPath(const QString &p) { m_path = p; }
    QString text() const { return m_text; }
    void setText(const QString &t);
    bool isDirty() const { return m_dirty; }
    void markClean() { m_dirty = false; }

    const QVector<Token> &tokens() const { return m_tokens; }
    const QVector<ImportEntry> &imports() const { return m_imports; }
    const QVector<NixList> &lists() const { return m_lists; }
    const QVector<AttrEntry> &attrs() const { return m_attrs; }
    const QVector<OptionDecl> &optionDecls() const { return m_options; }

    bool hasImportsList() const { return m_importsList >= 0; }
    /// Section headers found inside the imports list, in source order.
    QStringList importSections() const;

    const AttrEntry *findAttr(const QString &path) const;
    const NixList *findList(const QString &path) const;
    QVector<const NixList *> packageLists() const;

    // ── Mutations ────────────────────────────────────────────────────────────
    bool setImportEnabled(const QString &importText, bool enabled);
    bool addImport(const QString &relPath, const QString &section = QString());
    bool removeImport(const QString &importText);

    bool addPackage(const QString &listPath, const QString &expr, const QString &comment = QString());
    bool removePackage(const QString &listPath, const QString &expr);
    bool setPackageEnabled(const QString &listPath, const QString &expr, bool enabled);

    /// Updates an existing binding, or appends one to the top-level attrset.
    bool setAttribute(const QString &path, const QString &rawValue);
    bool removeAttribute(const QString &path);

    static QString quoteNixString(const QString &s);

private:
    void analyze();
    void replaceRange(int start, int end, const QString &with);
    /// Expands [start,end) to cover the whole line when nothing else is on it.
    void removeRange(int start, int end);
    QString indentAt(int offset) const;
    int lineStart(int offset) const;
    int lineEnd(int offset) const;

    QString m_path;
    QString m_text;
    bool m_dirty = false;

    QVector<Token> m_tokens;
    QVector<ImportEntry> m_imports;
    QVector<NixList> m_lists;
    QVector<AttrEntry> m_attrs;
    QVector<OptionDecl> m_options;
    QStringList m_importSections;
    QMap<QString, int> m_importSectionEnd;   // section name -> end of its header comment
    int m_importsList = -1;   // index into m_lists
    int m_bodyClose = -1;     // char position of the top-level body's '}'

    friend class Analyzer;
};

} // namespace nixm
