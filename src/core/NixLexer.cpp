#include "NixLexer.h"

namespace nixm {
namespace {

bool isIdentStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('\'')
        || c == QLatin1Char('-');
}

bool isPathChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')
        || c == QLatin1Char('-') || c == QLatin1Char('+');
}

/// Returns the end offset of a Nix path literal starting at `i`, or -1.
/// A path needs at least one `/` that is followed by a path character, which is
/// what distinguishes `./a/b` from the division and update (`//`) operators.
int matchPath(const QString &s, int i)
{
    const int n = s.size();
    int j = i;
    if (s[j] == QLatin1Char('~')) {
        if (j + 1 < n && s[j + 1] == QLatin1Char('/'))
            ++j;
        else
            return -1;
    }
    while (j < n && isPathChar(s[j]))
        ++j;
    if (j >= n || s[j] != QLatin1Char('/'))
        return -1;

    bool consumedSegment = false;
    while (j < n && s[j] == QLatin1Char('/')) {
        int k = j + 1;
        const int segStart = k;
        while (k < n && isPathChar(s[k]))
            ++k;
        if (k == segStart)
            break;
        j = k;
        consumedSegment = true;
    }
    return consumedSegment ? j : -1;
}

/// `<nixpkgs>` style search paths.
int matchSearchPath(const QString &s, int i)
{
    const int n = s.size();
    if (s[i] != QLatin1Char('<'))
        return -1;
    int j = i + 1;
    while (j < n && (isPathChar(s[j]) || s[j] == QLatin1Char('/')))
        ++j;
    if (j == i + 1 || j >= n || s[j] != QLatin1Char('>'))
        return -1;
    return j + 1;
}

int matchUri(const QString &s, int i)
{
    const int n = s.size();
    int j = i;
    if (!s[j].isLetter())
        return -1;
    while (j < n
        && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('+') || s[j] == QLatin1Char('-')
            || s[j] == QLatin1Char('.')))
        ++j;
    if (j >= n || s[j] != QLatin1Char(':'))
        return -1;
    static const QString extra = QStringLiteral("%/?:@&=+$,-_.!~*'");
    int k = j + 1;
    const int bodyStart = k;
    while (k < n && (s[k].isLetterOrNumber() || extra.contains(s[k])))
        ++k;
    // Require the scheme to be followed by something that cannot be mistaken for
    // a lambda (`x: y`) or an attribute binding.
    if (k == bodyStart)
        return -1;
    return k;
}

void countNewlines(const QString &s, int from, int to, int *line)
{
    for (int i = from; i < to; ++i)
        if (s[i] == QLatin1Char('\n'))
            ++(*line);
}

void skipInterpolation(const QString &s, int *i, int *line); // fwd

/// `*i` points at the opening quote. Leaves `*i` one past the closing quote.
void skipDoubleString(const QString &s, int *i, int *line)
{
    const int n = s.size();
    int j = *i + 1;
    while (j < n) {
        const QChar c = s[j];
        if (c == QLatin1Char('\\')) {
            if (j + 1 < n && s[j + 1] == QLatin1Char('\n'))
                ++(*line);
            j += 2;
            continue;
        }
        if (c == QLatin1Char('"')) {
            ++j;
            break;
        }
        if (c == QLatin1Char('$') && j + 1 < n && s[j + 1] == QLatin1Char('{')) {
            j += 2;
            skipInterpolation(s, &j, line);
            continue;
        }
        if (c == QLatin1Char('\n'))
            ++(*line);
        ++j;
    }
    *i = j;
}

/// `*i` points at the first quote of `''`. Leaves `*i` one past the closing `''`.
void skipIndentString(const QString &s, int *i, int *line)
{
    const int n = s.size();
    int j = *i + 2;
    while (j < n) {
        const QChar c = s[j];
        if (c == QLatin1Char('\'') && j + 1 < n && s[j + 1] == QLatin1Char('\'')) {
            const QChar next = (j + 2 < n) ? s[j + 2] : QChar();
            if (next == QLatin1Char('\'') || next == QLatin1Char('$')) {
                j += 3; // '''  or  ''$   -> escaped, keep going
                continue;
            }
            if (next == QLatin1Char('\\')) {
                j += 4; // ''\<char>
                continue;
            }
            j += 2;
            break;
        }
        if (c == QLatin1Char('$') && j + 1 < n && s[j + 1] == QLatin1Char('{')) {
            j += 2;
            skipInterpolation(s, &j, line);
            continue;
        }
        if (c == QLatin1Char('\n'))
            ++(*line);
        ++j;
    }
    *i = j;
}

/// `*i` points just past `${`. Leaves `*i` one past the matching `}`.
void skipInterpolation(const QString &s, int *i, int *line)
{
    const int n = s.size();
    int depth = 1;
    int j = *i;
    while (j < n && depth > 0) {
        const QChar c = s[j];
        if (c == QLatin1Char('{')) {
            ++depth;
            ++j;
        } else if (c == QLatin1Char('}')) {
            --depth;
            ++j;
        } else if (c == QLatin1Char('"')) {
            skipDoubleString(s, &j, line);
        } else if (c == QLatin1Char('\'') && j + 1 < n && s[j + 1] == QLatin1Char('\'')) {
            skipIndentString(s, &j, line);
        } else if (c == QLatin1Char('#')) {
            while (j < n && s[j] != QLatin1Char('\n'))
                ++j;
        } else if (c == QLatin1Char('/') && j + 1 < n && s[j + 1] == QLatin1Char('*')) {
            const int close = s.indexOf(QStringLiteral("*/"), j + 2);
            const int stop = (close < 0) ? n : close + 2;
            countNewlines(s, j, stop, line);
            j = stop;
        } else {
            if (c == QLatin1Char('\n'))
                ++(*line);
            ++j;
        }
    }
    *i = j;
}

const char *const kMultiCharPunct[] = { "...", "//", "++", "==", "!=", "<=", ">=", "->", "&&",
    "||", "|>", "<|", nullptr };

} // namespace

QVector<Token> tokenize(const QString &src)
{
    QVector<Token> out;
    const int n = src.size();
    int i = 0;
    int line = 0;

    auto push = [&](TokKind kind, int start, int end, int tokLine) {
        Token t;
        t.kind = kind;
        t.start = start;
        t.end = end;
        t.line = tokLine;
        t.text = src.mid(start, end - start);
        out.push_back(t);
    };

    while (i < n) {
        const QChar c = src[i];

        if (c == QLatin1Char('\n')) {
            ++line;
            ++i;
            continue;
        }
        if (c.isSpace()) {
            ++i;
            continue;
        }

        const int startLine = line;

        // ── Comments ─────────────────────────────────────────────────────────
        if (c == QLatin1Char('#')) {
            int j = i;
            while (j < n && src[j] != QLatin1Char('\n'))
                ++j;
            push(TokKind::Comment, i, j, startLine);
            i = j;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n && src[i + 1] == QLatin1Char('*')) {
            const int close = src.indexOf(QStringLiteral("*/"), i + 2);
            const int stop = (close < 0) ? n : close + 2;
            countNewlines(src, i, stop, &line);
            push(TokKind::Comment, i, stop, startLine);
            i = stop;
            continue;
        }

        // ── Strings ──────────────────────────────────────────────────────────
        if (c == QLatin1Char('"')) {
            int j = i;
            skipDoubleString(src, &j, &line);
            push(TokKind::String, i, j, startLine);
            i = j;
            continue;
        }
        if (c == QLatin1Char('\'') && i + 1 < n && src[i + 1] == QLatin1Char('\'')) {
            int j = i;
            skipIndentString(src, &j, &line);
            push(TokKind::IndentStr, i, j, startLine);
            i = j;
            continue;
        }

        // ── Paths (before punctuation, so `./x` does not become `.` `/` `x`) ─
        if (const int pe = matchSearchPath(src, i); pe > 0) {
            push(TokKind::Path, i, pe, startLine);
            i = pe;
            continue;
        }
        if (c == QLatin1Char('.') || c == QLatin1Char('/') || c == QLatin1Char('~')
            || isIdentStart(c) || c.isDigit()) {
            const int pe = matchPath(src, i);
            if (pe > 0) {
                push(TokKind::Path, i, pe, startLine);
                i = pe;
                continue;
            }
        }

        // ── URIs (before identifiers, `https:` would otherwise split) ────────
        if (c.isLetter()) {
            const int ue = matchUri(src, i);
            if (ue > 0) {
                push(TokKind::Uri, i, ue, startLine);
                i = ue;
                continue;
            }
        }

        // ── Identifiers ──────────────────────────────────────────────────────
        if (isIdentStart(c)) {
            int j = i;
            while (j < n && isIdentChar(src[j]))
                ++j;
            push(TokKind::Ident, i, j, startLine);
            i = j;
            continue;
        }

        // ── Numbers ──────────────────────────────────────────────────────────
        if (c.isDigit()) {
            int j = i;
            while (j < n && src[j].isDigit())
                ++j;
            if (j < n && src[j] == QLatin1Char('.') && j + 1 < n && src[j + 1].isDigit()) {
                ++j;
                while (j < n && src[j].isDigit())
                    ++j;
            }
            push(TokKind::Number, i, j, startLine);
            i = j;
            continue;
        }

        // ── Punctuation ──────────────────────────────────────────────────────
        bool matched = false;
        for (int k = 0; kMultiCharPunct[k]; ++k) {
            const QLatin1String op(kMultiCharPunct[k]);
            if (src.mid(i, op.size()) == op) {
                push(TokKind::Punct, i, i + op.size(), startLine);
                i += op.size();
                matched = true;
                break;
            }
        }
        if (matched)
            continue;

        push(TokKind::Punct, i, i + 1, startLine);
        ++i;
    }

    Token end;
    end.kind = TokKind::End;
    end.start = end.end = n;
    end.line = line;
    out.push_back(end);
    return out;
}

QString commentBody(const QString &commentText)
{
    QString s = commentText;
    if (s.startsWith(QLatin1String("/*"))) {
        s = s.mid(2);
        if (s.endsWith(QLatin1String("*/")))
            s.chop(2);
    } else {
        while (s.startsWith(QLatin1Char('#')))
            s = s.mid(1);
    }
    return s.trimmed();
}

} // namespace nixm
