#include "NixFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTextStream>

namespace nixm {
namespace {

const QStringList kReserved = { QStringLiteral("inherit"), QStringLiteral("with"),
    QStringLiteral("let"), QStringLiteral("rec"), QStringLiteral("assert"), QStringLiteral("if"),
    QStringLiteral("then"), QStringLiteral("else"), QStringLiteral("in") };

const QStringList kPackageListNames = { QStringLiteral("systemPackages"),
    QStringLiteral("packages"), QStringLiteral("extraPackages"),
    QStringLiteral("extraCompatPackages"), QStringLiteral("excludePackages"),
    QStringLiteral("buildInputs"), QStringLiteral("nativeBuildInputs"),
    QStringLiteral("propagatedBuildInputs"), QStringLiteral("path") };

QString stripQuotes(const QString &s)
{
    if (s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
        return s.mid(1, s.size() - 2);
    return s;
}

/// "── Kernel ─────────────" -> "Kernel"; returns empty for non-headers.
QString cleanSection(const QString &body)
{
    static const QString junk = QStringLiteral("-=*_.~ \t─━┄┅┈┉"
                                               "╌╍═");
    int a = 0;
    int b = body.size();
    while (a < b && junk.contains(body[a]))
        ++a;
    while (b > a && junk.contains(body[b - 1]))
        --b;
    QString s = body.mid(a, b - a).trimmed();
    // Section headers are short labels, not prose or code.
    if (s.isEmpty() || s.size() > 40 || s.contains(QLatin1Char('/'))
        || s.contains(QLatin1Char(';')) || s.contains(QLatin1Char('=')))
        return QString();
    return s;
}

bool looksLikePath(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._~+/-]+$"));
    return s.contains(QLatin1Char('/')) && re.match(s).hasMatch();
}

bool looksLikePackageExpr(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_][A-Za-z0-9_'.-]*$"));
    return re.match(s).hasMatch();
}

/// Splits a commented-out entry into the expression and any note after it.
void splitCommentBody(const QString &body, QString *expr, QString *note)
{
    const int hash = body.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        *expr = body.left(hash).trimmed();
        *note = body.mid(hash + 1).trimmed();
    } else {
        *expr = body.trimmed();
        note->clear();
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Small value helpers
// ─────────────────────────────────────────────────────────────────────────────

QString NixList::qualify(const QString &expr) const
{
    if (withExpr.isEmpty() || withExpr == QLatin1String("pkgs"))
        return expr;
    if (withExpr.startsWith(QLatin1String("pkgs.")))
        return withExpr.mid(5) + QLatin1Char('.') + expr;
    return expr;
}

QString AttrEntry::unquoted() const
{
    QString v = rawValue.trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) {
        v = v.mid(1, v.size() - 2);
        v.replace(QLatin1String("\\\""), QLatin1String("\""));
        v.replace(QLatin1String("\\n"), QLatin1String("\n"));
        v.replace(QLatin1String("\\\\"), QLatin1String("\\"));
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Analyzer: walks the token stream and records every binding it understands.
// ─────────────────────────────────────────────────────────────────────────────

class Analyzer
{
public:
    explicit Analyzer(NixFile &f)
        : F(f)
        , src(f.m_text)
        , t(f.m_tokens)
    {
    }

    void run()
    {
        int begin = 0;
        int end = 0;
        if (!findTopLevelBody(&begin, &end))
            return;
        parseBody(begin, end, QString());
    }

private:
    NixFile &F;
    const QString &src;
    const QVector<Token> &t;

    int count() const { return t.size(); }

    int match(int i) const
    {
        if (i < 0 || i >= count() || t[i].kind != TokKind::Punct)
            return -1;
        const QString open = t[i].text;
        QString close;
        if (open == QLatin1String("{"))
            close = QStringLiteral("}");
        else if (open == QLatin1String("["))
            close = QStringLiteral("]");
        else if (open == QLatin1String("("))
            close = QStringLiteral(")");
        else
            return -1;

        int depth = 0;
        for (int j = i; j < count(); ++j) {
            if (t[j].kind != TokKind::Punct)
                continue;
            if (t[j].text == open) {
                ++depth;
            } else if (t[j].text == close) {
                if (--depth == 0)
                    return j;
            }
        }
        return -1;
    }

    /// Index of the `;` that terminates a binding, or -1.
    int findSemi(int from, int end, bool skipWith = true) const
    {
        int depth = 0;
        for (int i = from; i < end && i < count(); ++i) {
            const Token &x = t[i];
            if (x.kind == TokKind::Comment)
                continue;
            if (x.kind == TokKind::Punct) {
                const QString &p = x.text;
                if (p == QLatin1String("{") || p == QLatin1String("[") || p == QLatin1String("(")) {
                    ++depth;
                } else if (p == QLatin1String("}") || p == QLatin1String("]")
                    || p == QLatin1String(")")) {
                    if (depth == 0)
                        return -1;
                    --depth;
                } else if (depth == 0 && p == QLatin1String(";")) {
                    return i;
                }
                continue;
            }
            // `with pkgs;` and `assert x;` are prefixes of a value, not terminators.
            if (skipWith && depth == 0
                && (x.isIdent("with") || x.isIdent("assert") || x.isIdent("inherit"))) {
                const int semi = findSemi(i + 1, end, false);
                if (semi < 0)
                    return -1;
                i = semi;
                continue;
            }

            // A `let … in` prelude carries its own `;` per binding; none of them
            // terminates the binding we are measuring.
            if (skipWith && depth == 0 && x.isIdent("let")) {
                int nested = 0;
                int braceDepth = 0;
                int j = i + 1;
                for (; j < end && j < count(); ++j) {
                    const Token &y = t[j];
                    if (y.kind == TokKind::Punct) {
                        const QString &q = y.text;
                        if (q == QLatin1String("{") || q == QLatin1String("[")
                            || q == QLatin1String("(")) {
                            ++braceDepth;
                        } else if (q == QLatin1String("}") || q == QLatin1String("]")
                            || q == QLatin1String(")")) {
                            if (braceDepth == 0)
                                break;
                            --braceDepth;
                        }
                    } else if (braceDepth == 0 && y.isIdent("let")) {
                        ++nested;
                    } else if (braceDepth == 0 && y.isIdent("in")) {
                        if (nested == 0)
                            break;
                        --nested;
                    }
                }
                i = j;
                continue;
            }
        }
        return -1;
    }

    bool isFormals(int i) const
    {
        const int close = match(i);
        if (close < 0)
            return false;
        int j = close + 1;
        if (j < count() && t[j].isPunct("@"))
            j += 2;
        return j < count() && t[j].isPunct(":");
    }

    bool readAttrPath(int &i, int end, QString &out) const
    {
        QStringList parts;
        int j = i;
        auto readComponent = [&](QString *c) -> bool {
            if (j >= end || j >= count())
                return false;
            const Token &x = t[j];
            if (x.kind == TokKind::Ident && !kReserved.contains(x.text)) {
                *c = x.text;
                ++j;
                return true;
            }
            if (x.kind == TokKind::String) {
                *c = stripQuotes(x.text);
                ++j;
                return true;
            }
            return false;
        };

        QString comp;
        if (!readComponent(&comp))
            return false;
        parts << comp;
        while (j < end && j < count() && t[j].isPunct(".")) {
            const int save = j;
            ++j;
            QString next;
            if (!readComponent(&next)) {
                j = save;
                break;
            }
            parts << next;
        }
        i = j;
        out = parts.join(QLatin1Char('.'));
        return true;
    }

    /// Locates the attribute set that forms the module body, skipping the
    /// `{ pkgs, ... }:` formals and any `let … in` prelude.
    bool findTopLevelBody(int *begin, int *end)
    {
        int i = 0;
        int guard = 0;
        while (i < count() && ++guard < 10000) {
            const Token &x = t[i];
            if (x.kind == TokKind::Comment || x.kind == TokKind::End) {
                if (x.kind == TokKind::End)
                    return false;
                ++i;
                continue;
            }
            if (x.isIdent("let")) {
                int depth = 0;
                int j = i + 1;
                for (; j < count(); ++j) {
                    if (t[j].kind == TokKind::Punct) {
                        const QString &p = t[j].text;
                        if (p == QLatin1String("{") || p == QLatin1String("[")
                            || p == QLatin1String("("))
                            ++depth;
                        else if (p == QLatin1String("}") || p == QLatin1String("]")
                            || p == QLatin1String(")"))
                            --depth;
                    } else if (depth == 0 && t[j].isIdent("in")) {
                        break;
                    }
                }
                if (j >= count())
                    return false;
                i = j + 1;
                continue;
            }
            if (x.kind == TokKind::Ident && i + 1 < count() && t[i + 1].isPunct("@")) {
                i += 2;
                continue;
            }
            if (x.isPunct("{")) {
                const int close = match(i);
                if (close < 0)
                    return false;
                if (isFormals(i)) {
                    int j = close + 1;
                    if (j < count() && t[j].isPunct("@"))
                        j += 2;
                    if (j < count() && t[j].isPunct(":"))
                        ++j;
                    i = j;
                    continue;
                }
                *begin = i + 1;
                *end = close;
                F.m_bodyClose = t[close].start;
                return true;
            }
            ++i;
        }
        return false;
    }

    static QString normalize(QString s)
    {
        if (s.startsWith(QLatin1String("options.")))
            s = s.mid(8);
        if (s.startsWith(QLatin1String("config.")))
            s = s.mid(7);
        return s;
    }

    void parseBody(int begin, int end, const QString &prefix)
    {
        int i = begin;
        int guard = 0;
        while (i < end && i < count() && ++guard < 100000) {
            const Token &x = t[i];
            if (x.kind == TokKind::Comment) {
                ++i;
                continue;
            }
            if (x.isIdent("inherit")) {
                const int semi = findSemi(i + 1, end, false);
                i = (semi < 0) ? end : semi + 1;
                continue;
            }

            const int stmtTok = i;
            QString path;
            int cursor = i;
            if (!readAttrPath(cursor, end, path) || cursor >= end || !t[cursor].isPunct("=")) {
                const int semi = findSemi(stmtTok, end);
                i = (semi < 0) ? end : semi + 1;
                continue;
            }

            const int eq = cursor;
            const int semi = findSemi(eq + 1, end);
            int vLast = (semi < 0) ? end - 1 : semi - 1;
            while (vLast > eq && t[vLast].kind == TokKind::Comment)
                --vLast;

            if (vLast > eq) {
                const int stmtEndChar = (semi < 0) ? t[vLast].end : t[semi].end;
                handleBinding(prefix, path, stmtTok, eq + 1, vLast, stmtEndChar);
            }
            i = (semi < 0) ? end : semi + 1;
        }
    }

    void handleBinding(const QString &prefix, const QString &path, int stmtTok, int vBegin,
        int vLast, int stmtEndChar)
    {
        const QString full = prefix.isEmpty() ? path : prefix + QLatin1Char('.') + path;
        const QString norm = normalize(full);
        const int stmtStart = t[stmtTok].start;

        // A `with …;` prefix belongs to the value, not to the binding.
        int p = vBegin;
        QString withExpr;
        while (p <= vLast) {
            if (t[p].kind == TokKind::Comment) {
                ++p;
                continue;
            }
            if (t[p].isIdent("with")) {
                const int semi = findSemi(p + 1, vLast + 1, false);
                if (semi < 0)
                    break;
                withExpr = src.mid(t[p + 1].start, t[semi].start - t[p + 1].start).trimmed();
                p = semi + 1;
                continue;
            }
            break;
        }
        if (p > vLast)
            return;

        if (t[p].isPunct("[")) {
            const int close = match(p);
            if (close >= 0 && close <= vLast) {
                parseList(norm, withExpr, p, close, stmtStart, stmtEndChar);
                return;
            }
        }
        if (t[p].isPunct("{") && !isFormals(p)) {
            const int close = match(p);
            if (close >= 0 && close <= vLast) {
                NixAttrSet set;
                set.path = norm;
                set.lbrace = t[p].start;
                set.rbrace = t[close].start;
                set.stmtStart = stmtStart;
                set.stmtEnd = stmtEndChar;
                F.m_sets.push_back(set);

                parseBody(p + 1, close, full);
                return;
            }
        }

        AttrEntry a;
        a.path = norm;
        a.valueStart = t[p].start;
        a.valueEnd = t[vLast].end;
        a.rawValue = src.mid(a.valueStart, a.valueEnd - a.valueStart);
        a.stmtStart = stmtStart;
        a.stmtEnd = stmtEndChar;
        a.line = t[stmtTok].line;
        F.m_attrs.push_back(a);

        // Complex values still deserve a look: option declarations and attrsets
        // hidden behind `lib.mkIf`, `lib.mkMerge` and friends.
        bool declaresOption = false;
        for (int q = p; q <= vLast; ++q) {
            const Token &y = t[q];
            if (y.kind == TokKind::Ident && y.text == QLatin1String("mkOption"))
                declaresOption = true;
            if (y.isPunct("{") && !isFormals(q)) {
                const int close = match(q);
                if (close > q && close <= vLast) {
                    parseBody(q + 1, close, full);
                    q = close;
                }
            }
        }
        if (declaresOption) {
            OptionDecl o;
            o.path = norm;
            o.file = F.m_path;
            F.m_options.push_back(o);
        }
    }

    void parseList(const QString &path, const QString &withExpr, int lb, int rb, int stmtStart,
        int stmtEnd)
    {
        NixList list;
        list.path = path;
        list.withExpr = withExpr;
        list.lbracket = t[lb].start;
        list.rbracket = t[rb].start;
        list.stmtStart = stmtStart;
        list.stmtEnd = stmtEnd;

        const bool isImports
            = (path == QLatin1String("imports") || path.endsWith(QLatin1String(".imports")));
        const QString lastComponent = path.section(QLatin1Char('.'), -1);
        list.isPackageList = !isImports
            && (withExpr.contains(QLatin1String("pkgs")) || kPackageListNames.contains(lastComponent));

        QString section;
        int i = lb + 1;
        int guard = 0;
        while (i < rb && ++guard < 100000) {
            const Token &x = t[i];

            if (x.kind == TokKind::Comment) {
                QString expr;
                QString note;
                splitCommentBody(commentBody(x.text), &expr, &note);
                if (isImports && looksLikePath(expr)) {
                    ImportEntry e;
                    e.text = expr;
                    e.comment = note;
                    e.enabled = false;
                    e.start = x.start;
                    e.end = x.end;
                    e.endFull = x.end;
                    e.line = x.line;
                    e.section = section;
                    F.m_imports.push_back(e);
                } else if (!isImports && looksLikePackageExpr(expr)) {
                    PackageEntry pe;
                    pe.expr = expr;
                    pe.comment = note;
                    pe.enabled = false;
                    pe.start = x.start;
                    pe.end = x.end;
                    pe.lineEnd = x.end;
                    pe.line = x.line;
                    list.entries.push_back(pe);
                } else {
                    const QString sec = cleanSection(commentBody(x.text));
                    if (!sec.isEmpty()) {
                        section = sec;
                        if (isImports && !F.m_importSections.contains(sec)) {
                            F.m_importSections << sec;
                            F.m_importSectionEnd.insert(sec, x.end);
                        }
                    }
                }
                ++i;
                continue;
            }

            // Determine the extent of one list element.
            const int first = i;
            int last = i;
            if (x.isPunct("(") || x.isPunct("[") || x.isPunct("{")) {
                const int close = match(i);
                last = (close > 0 && close < rb) ? close : i;
            } else if (x.kind == TokKind::Ident) {
                while (last + 2 < rb && t[last + 1].isPunct(".")
                    && (t[last + 2].kind == TokKind::Ident || t[last + 2].kind == TokKind::String))
                    last += 2;
            }

            int j = last + 1;
            QString note;
            int endOfEntry = t[last].end;
            if (j < rb && t[j].kind == TokKind::Comment && t[j].line == t[last].line) {
                note = commentBody(t[j].text);
                endOfEntry = t[j].end;
                ++j;
            }

            if (isImports) {
                ImportEntry e;
                e.text = src.mid(t[first].start, t[last].end - t[first].start);
                e.comment = note;
                e.enabled = true;
                e.start = t[first].start;
                e.end = t[last].end;
                e.endFull = endOfEntry;
                e.line = t[first].line;
                e.section = section;
                F.m_imports.push_back(e);
            } else {
                PackageEntry pe;
                pe.expr = src.mid(t[first].start, t[last].end - t[first].start);
                pe.comment = note;
                pe.enabled = true;
                pe.start = t[first].start;
                pe.end = t[last].end;
                pe.lineEnd = endOfEntry;
                pe.line = t[first].line;
                list.entries.push_back(pe);
            }
            i = j;
        }

        F.m_lists.push_back(list);
        if (isImports && F.m_importsList < 0)
            F.m_importsList = F.m_lists.size() - 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  NixFile
// ─────────────────────────────────────────────────────────────────────────────

NixFile::NixFile(const QString &path)
    : m_path(path)
{
}

bool NixFile::load(QString *err)
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err)
            *err = QStringLiteral("%1: %2").arg(m_path, f.errorString());
        return false;
    }
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    m_text = in.readAll();
    m_dirty = false;
    analyze();
    return true;
}

bool NixFile::save(QString *err, bool *permissionDenied)
{
    if (permissionDenied)
        *permissionDenied = false;
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (err)
            *err = QStringLiteral("%1: %2").arg(m_path, f.errorString());
        if (permissionDenied && !QFileInfo(QFileInfo(m_path).absolutePath()).isWritable())
            *permissionDenied = true;
        return false;
    }
    {
        QTextStream out(&f);
        out.setEncoding(QStringConverter::Utf8);
        out << m_text;
    }
    if (!f.commit()) {
        if (err)
            *err = QStringLiteral("%1: %2").arg(m_path, f.errorString());
        if (permissionDenied)
            *permissionDenied = true;
        return false;
    }
    m_dirty = false;
    return true;
}

QString NixFile::writeToTemp(QString *err) const
{
    QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/nixos-manager-XXXXXX.nix"));
    tmp.setAutoRemove(false);
    if (!tmp.open()) {
        if (err)
            *err = tmp.errorString();
        return QString();
    }
    {
        QTextStream out(&tmp);
        out.setEncoding(QStringConverter::Utf8);
        out << m_text;
    }
    tmp.close();
    return tmp.fileName();
}

void NixFile::setText(const QString &t)
{
    if (m_text == t)
        return;
    m_text = t;
    m_dirty = true;
    analyze();
}

void NixFile::analyze()
{
    m_tokens = tokenize(m_text);
    m_imports.clear();
    m_lists.clear();
    m_attrs.clear();
    m_sets.clear();
    m_options.clear();
    m_importSections.clear();
    m_importSectionEnd.clear();
    m_importsList = -1;
    m_bodyClose = -1;

    Analyzer a(*this);
    a.run();

    // Fill in option metadata from the scalars inside each `mkOption { … }`.
    for (OptionDecl &o : m_options) {
        if (const AttrEntry *ty = findAttr(o.path + QStringLiteral(".type")))
            o.type = ty->rawValue;
        if (const AttrEntry *dv = findAttr(o.path + QStringLiteral(".default")))
            o.defaultValue = dv->rawValue;
        if (const AttrEntry *de = findAttr(o.path + QStringLiteral(".description")))
            o.description = de->unquoted();
    }
}

QStringList NixFile::importSections() const
{
    return m_importSections;
}

const AttrEntry *NixFile::findAttr(const QString &path) const
{
    for (const AttrEntry &a : m_attrs)
        if (a.path == path)
            return &a;
    return nullptr;
}

const NixList *NixFile::findList(const QString &path) const
{
    for (const NixList &l : m_lists)
        if (l.path == path)
            return &l;
    return nullptr;
}

const NixAttrSet *NixFile::findSet(const QString &path) const
{
    for (const NixAttrSet &s : m_sets)
        if (s.path == path)
            return &s;
    return nullptr;
}

QVector<const NixList *> NixFile::packageLists() const
{
    QVector<const NixList *> out;
    for (const NixList &l : m_lists)
        if (l.isPackageList)
            out.push_back(&l);
    return out;
}

// ── Text primitives ──────────────────────────────────────────────────────────

int NixFile::lineStart(int offset) const
{
    if (offset <= 0)
        return 0;
    const int idx = m_text.lastIndexOf(QLatin1Char('\n'), offset - 1);
    return idx < 0 ? 0 : idx + 1;
}

int NixFile::lineEnd(int offset) const
{
    const int idx = m_text.indexOf(QLatin1Char('\n'), qMax(0, offset));
    return idx < 0 ? m_text.size() : idx;
}

QString NixFile::indentAt(int offset) const
{
    const int ls = lineStart(offset);
    int i = ls;
    while (i < m_text.size() && i < offset
        && (m_text[i] == QLatin1Char(' ') || m_text[i] == QLatin1Char('\t')))
        ++i;
    return m_text.mid(ls, i - ls);
}

void NixFile::replaceRange(int start, int end, const QString &with)
{
    if (start < 0 || end > m_text.size() || start > end)
        return;
    m_text.replace(start, end - start, with);
    m_dirty = true;
    analyze();
}

void NixFile::removeRange(int start, int end)
{
    if (start < 0 || end > m_text.size() || start > end)
        return;
    const int ls = lineStart(start);
    const int le = lineEnd(end);
    const bool aloneBefore = m_text.mid(ls, start - ls).trimmed().isEmpty();
    const bool aloneAfter = m_text.mid(end, le - end).trimmed().isEmpty();
    if (aloneBefore && aloneAfter) {
        const int stop = qMin(le + 1, m_text.size());
        m_text.remove(ls, stop - ls);
    } else {
        m_text.remove(start, end - start);
    }
    m_dirty = true;
    analyze();
}

QString NixFile::quoteNixString(const QString &s)
{
    QString out = s;
    out.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    out.replace(QLatin1String("\""), QLatin1String("\\\""));
    out.replace(QLatin1String("\n"), QLatin1String("\\n"));
    out.replace(QLatin1String("${"), QLatin1String("\\${"));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

// ── Import mutations ─────────────────────────────────────────────────────────

bool NixFile::setImportEnabled(const QString &importText, bool enabled)
{
    for (const ImportEntry &e : m_imports) {
        if (e.text != importText)
            continue;
        if (e.enabled == enabled)
            return true;
        QString body = e.text;
        if (!e.comment.isEmpty())
            body += QStringLiteral("  # ") + e.comment;
        replaceRange(e.start, e.endFull, enabled ? body : QLatin1Char('#') + body);
        return true;
    }
    return false;
}

bool NixFile::addImport(const QString &relPath, const QString &section)
{
    for (const ImportEntry &e : m_imports) {
        if (e.text == relPath)
            return e.enabled ? true : setImportEnabled(relPath, true);
    }

    if (m_importsList < 0) {
        if (m_bodyClose < 0)
            return false;
        const QString indent = indentAt(m_bodyClose) + QStringLiteral("    ");
        const QString block = indent + QStringLiteral("imports = [\n") + indent
            + QStringLiteral("    ") + relPath + QLatin1Char('\n') + indent
            + QStringLiteral("];\n\n");
        const int at = lineStart(m_bodyClose);
        m_text.insert(at, block);
        m_dirty = true;
        analyze();
        return true;
    }

    const NixList &list = m_lists[m_importsList];
    int insertAt = -1;
    QString indent;

    if (!section.isEmpty()) {
        for (const ImportEntry &e : m_imports)
            if (e.section == section)
                insertAt = qMax(insertAt, lineEnd(e.endFull));
        if (insertAt < 0 && m_importSectionEnd.contains(section))
            insertAt = lineEnd(m_importSectionEnd.value(section));
    }
    if (insertAt < 0 && !m_imports.isEmpty())
        insertAt = lineEnd(m_imports.last().endFull);
    if (insertAt < 0)
        insertAt = list.lbracket + 1;

    indent = m_imports.isEmpty() ? indentAt(list.lbracket) + QStringLiteral("    ")
                                 : indentAt(m_imports.first().start);

    m_text.insert(insertAt, QLatin1Char('\n') + indent + relPath);
    m_dirty = true;
    analyze();
    return true;
}

bool NixFile::removeImport(const QString &importText)
{
    for (const ImportEntry &e : m_imports) {
        if (e.text != importText)
            continue;
        removeRange(e.start, e.endFull);
        return true;
    }
    return false;
}

// ── Package-list mutations ───────────────────────────────────────────────────

bool NixFile::addPackage(const QString &listPath, const QString &expr, const QString &comment)
{
    const NixList *list = findList(listPath);
    if (!list)
        return false;
    for (const PackageEntry &e : list->entries) {
        if (e.expr == expr)
            return e.enabled ? true : setPackageEnabled(listPath, expr, true);
    }

    int insertAt;
    QString indent;
    if (!list->entries.isEmpty()) {
        insertAt = lineEnd(list->entries.last().lineEnd);
        indent = indentAt(list->entries.first().start);
    } else {
        insertAt = list->lbracket + 1;
        indent = indentAt(list->lbracket) + QStringLiteral("    ");
    }

    QString line = QLatin1Char('\n') + indent + expr;
    if (!comment.isEmpty())
        line += QStringLiteral("  # ") + comment;
    m_text.insert(insertAt, line);
    m_dirty = true;
    analyze();
    return true;
}

bool NixFile::removePackage(const QString &listPath, const QString &expr)
{
    const NixList *list = findList(listPath);
    if (!list)
        return false;
    for (const PackageEntry &e : list->entries) {
        if (e.expr != expr)
            continue;
        const int start = e.start;
        const int end = e.lineEnd;
        removeRange(start, end);
        return true;
    }
    return false;
}

bool NixFile::setPackageEnabled(const QString &listPath, const QString &expr, bool enabled)
{
    const NixList *list = findList(listPath);
    if (!list)
        return false;
    for (const PackageEntry &e : list->entries) {
        if (e.expr != expr)
            continue;
        if (e.enabled == enabled)
            return true;
        QString body = e.expr;
        if (!e.comment.isEmpty())
            body += QStringLiteral("  # ") + e.comment;
        const int start = e.start;
        const int end = e.lineEnd;
        replaceRange(start, end, enabled ? body : QLatin1Char('#') + body);
        return true;
    }
    return false;
}

// ── Attribute mutations ──────────────────────────────────────────────────────

bool NixFile::setAttribute(const QString &path, const QString &rawValue)
{
    if (const AttrEntry *a = findAttr(path)) {
        replaceRange(a->valueStart, a->valueEnd, rawValue);
        return true;
    }
    if (m_bodyClose < 0)
        return false;
    const QString indent = indentAt(m_bodyClose) + QStringLiteral("    ");
    const int at = lineStart(m_bodyClose);
    m_text.insert(at, indent + path + QStringLiteral(" = ") + rawValue + QStringLiteral(";\n"));
    m_dirty = true;
    analyze();
    return true;
}

bool NixFile::addToAttrSet(const QString &setPath, const QString &name, const QString &rawValue)
{
    const NixAttrSet *set = findSet(setPath);
    if (!set)
        return false;

    const QString memberPath = setPath + QLatin1Char('.') + name;
    if (findAttr(memberPath) || findSet(memberPath))
        return setAttribute(memberPath, rawValue);

    // Collect the direct members so we can match their layout.
    QVector<const AttrEntry *> siblings;
    for (const AttrEntry &a : m_attrs) {
        if (!a.path.startsWith(setPath + QLatin1Char('.')))
            continue;
        if (a.path.mid(setPath.size() + 1).contains(QLatin1Char('.')))
            continue;
        if (a.stmtStart > set->lbrace && a.stmtEnd <= set->rbrace)
            siblings.push_back(&a);
    }

    QString indent;
    int insertAt;
    int equalsColumn = -1;

    if (siblings.isEmpty()) {
        indent = indentAt(set->lbrace) + QStringLiteral("    ");
        insertAt = set->lbrace + 1;
    } else {
        const AttrEntry *last = siblings.first();
        for (const AttrEntry *a : std::as_const(siblings))
            if (a->stmtEnd > last->stmtEnd)
                last = a;
        indent = indentAt(siblings.first()->stmtStart);
        insertAt = lineEnd(last->stmtEnd);

        // Their `=` signs are often aligned in a column; keep that up.
        for (const AttrEntry *a : std::as_const(siblings)) {
            const int eq = m_text.lastIndexOf(QLatin1Char('='), a->valueStart);
            if (eq < 0)
                continue;
            const int column = eq - lineStart(eq);
            equalsColumn = qMax(equalsColumn, column);
        }
    }

    QString member = indent + name;
    if (equalsColumn > 0) {
        // The " = " we append supplies one of the spaces, hence the -1.
        const int target = equalsColumn - int(indent.size()) - 1;
        if (target > name.size())
            member += QString(target - name.size(), QLatin1Char(' '));
    }
    member += QStringLiteral(" = ") + rawValue + QLatin1Char(';');

    m_text.insert(insertAt, QLatin1Char('\n') + member);
    m_dirty = true;
    analyze();
    return true;
}

bool NixFile::removeAttribute(const QString &path)
{
    if (const AttrEntry *a = findAttr(path)) {
        const int start = a->stmtStart;
        const int end = a->stmtEnd;
        removeRange(start, end);
        return true;
    }
    return false;
}

} // namespace nixm
