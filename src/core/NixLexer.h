#pragma once

#include <QString>
#include <QVector>

namespace nixm {

enum class TokKind {
    Ident,      // foo, bar, true, with, let, ...
    Number,     // 42, 3.5
    Path,       // ./foo, ../bar/baz.nix, /etc/nixos, ~/x, <nixpkgs>
    Uri,        // https://example.com/x
    String,     // "..." including any ${...} interpolation
    IndentStr,  // ''...'' including any ${...} interpolation
    Comment,    // # ... or /* ... */
    Punct,      // = ; { } [ ] ( ) . , : @ ? ++ // -> && || == != <= >= ...
    End
};

struct Token {
    TokKind kind = TokKind::End;
    int start = 0;   // index of first character in the source
    int end = 0;     // index one past the last character
    int line = 0;    // 0-based line of `start`
    QString text;    // verbatim source slice [start, end)

    bool isPunct(const char *p) const { return kind == TokKind::Punct && text == QLatin1String(p); }
    bool isIdent(const char *p) const { return kind == TokKind::Ident && text == QLatin1String(p); }
    bool isComment() const { return kind == TokKind::Comment; }
};

/// Tokenizes Nix source. The lexer is deliberately forgiving: it never fails,
/// it just produces the best token stream it can. Strings (both flavours) are
/// emitted as a single token even when they contain `${...}` interpolation, so
/// callers can safely scan for brackets without tripping over string contents.
QVector<Token> tokenize(const QString &src);

/// Strips the comment markers from a comment token's text and trims it.
QString commentBody(const QString &commentText);

} // namespace nixm
