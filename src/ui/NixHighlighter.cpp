#include "NixHighlighter.h"

namespace nixm {

NixHighlighter::NixHighlighter(QTextDocument *parent)
    : QSyntaxHighlighter(parent)
{
    buildRules();
}

void NixHighlighter::setDarkMode(bool dark)
{
    if (m_dark == dark && !m_rules.isEmpty())
        return;
    m_dark = dark;
    buildRules();
    rehighlight();
}

void NixHighlighter::buildRules()
{
    m_rules.clear();

    const QColor keywordColor = m_dark ? QColor(0xC5, 0x92, 0xE8) : QColor(0x7C, 0x3A, 0xED);
    const QColor builtinColor = m_dark ? QColor(0x7A, 0xB8, 0xFF) : QColor(0x1D, 0x4E, 0xD8);
    const QColor stringColor = m_dark ? QColor(0x9E, 0xD0, 0x7C) : QColor(0x16, 0x7F, 0x3D);
    const QColor commentColor = m_dark ? QColor(0x7F, 0x8C, 0x8D) : QColor(0x6B, 0x72, 0x80);
    const QColor pathColor = m_dark ? QColor(0xE5, 0xB5, 0x67) : QColor(0xB4, 0x53, 0x09);
    const QColor numberColor = m_dark ? QColor(0xE0, 0x8C, 0x6D) : QColor(0xB9, 0x2B, 0x27);
    const QColor attrColor = m_dark ? QColor(0x8B, 0xD5, 0xCA) : QColor(0x0F, 0x76, 0x6E);

    m_keyword.setForeground(keywordColor);
    m_keyword.setFontWeight(QFont::DemiBold);
    m_builtin.setForeground(builtinColor);
    m_string.setForeground(stringColor);
    m_comment.setForeground(commentColor);
    m_comment.setFontItalic(true);
    m_path.setForeground(pathColor);
    m_number.setForeground(numberColor);
    m_attribute.setForeground(attrColor);
    m_operatorFmt.setForeground(m_dark ? QColor(0xB0, 0xB8, 0xC0) : QColor(0x4B, 0x55, 0x63));
    m_interpolation.setForeground(m_dark ? QColor(0xF2, 0xC0, 0x7B) : QColor(0x92, 0x40, 0x0E));

    // Attribute paths on the left of `=` — the most useful visual anchor in a
    // NixOS module.
    m_rules.push_back({ QRegularExpression(QStringLiteral(
                            R"((^|[{;]|\bimports\b)\s*([A-Za-z_][A-Za-z0-9_'.-]*)\s*(?==[^=]))")),
        m_attribute });

    static const QStringList keywords = { QStringLiteral("let"), QStringLiteral("in"),
        QStringLiteral("rec"), QStringLiteral("inherit"), QStringLiteral("with"),
        QStringLiteral("assert"), QStringLiteral("if"), QStringLiteral("then"),
        QStringLiteral("else"), QStringLiteral("or") };
    for (const QString &kw : keywords)
        m_rules.push_back(
            { QRegularExpression(QStringLiteral("\\b%1\\b").arg(kw)), m_keyword });

    static const QStringList builtins = { QStringLiteral("true"), QStringLiteral("false"),
        QStringLiteral("null"), QStringLiteral("import"), QStringLiteral("builtins"),
        QStringLiteral("derivation"), QStringLiteral("map"), QStringLiteral("toString"),
        QStringLiteral("fetchTarball"), QStringLiteral("fetchGit"), QStringLiteral("mkIf"),
        QStringLiteral("mkOption"), QStringLiteral("mkEnableOption"), QStringLiteral("mkMerge"),
        QStringLiteral("mkDefault"), QStringLiteral("mkForce"), QStringLiteral("mkOverride") };
    for (const QString &b : builtins)
        m_rules.push_back({ QRegularExpression(QStringLiteral("\\b%1\\b").arg(b)), m_builtin });

    m_rules.push_back(
        { QRegularExpression(QStringLiteral(R"(\b\d+(\.\d+)?\b)")), m_number });

    // Paths need at least one slash, which keeps `a.b` and `4 / 2` out.
    m_rules.push_back({ QRegularExpression(QStringLiteral(
                            R"((?<![\w"'])(~?[A-Za-z0-9._+-]*(?:/[A-Za-z0-9._+-]+)+/?))")),
        m_path });
    m_rules.push_back({ QRegularExpression(QStringLiteral(R"(<[A-Za-z0-9._/+-]+>)")), m_path });

    m_rules.push_back(
        { QRegularExpression(QStringLiteral(R"((\+\+|//|->|&&|\|\||==|!=|<=|>=|[=;:?@]))")),
            m_operatorFmt });

    // Double-quoted strings last, so they win over anything matched inside them.
    m_rules.push_back({ QRegularExpression(QStringLiteral(R"("(?:[^"\\]|\\.)*")")), m_string });
    m_rules.push_back({ QRegularExpression(QStringLiteral("#[^\n]*")), m_comment });
}

void NixHighlighter::highlightInterpolation(const QString &text, int from, int to)
{
    static const QRegularExpression interp(QStringLiteral(R"(\$\{[^}]*\})"));
    auto it = interp.globalMatch(text, from);
    while (it.hasNext()) {
        const auto m = it.next();
        if (m.capturedStart() >= to)
            break;
        setFormat(m.capturedStart(), qMin(m.capturedLength(), to - m.capturedStart()),
            m_interpolation);
    }
}

void NixHighlighter::highlightBlock(const QString &text)
{
    // ── Continuation of a multi-line construct ───────────────────────────────
    const int previous = previousBlockState();
    int offset = 0;

    if (previous == InIndentString) {
        const int close = text.indexOf(QStringLiteral("''"));
        if (close < 0) {
            setFormat(0, text.length(), m_string);
            highlightInterpolation(text, 0, text.length());
            setCurrentBlockState(InIndentString);
            return;
        }
        setFormat(0, close + 2, m_string);
        highlightInterpolation(text, 0, close);
        offset = close + 2;
    } else if (previous == InBlockComment) {
        const int close = text.indexOf(QStringLiteral("*/"));
        if (close < 0) {
            setFormat(0, text.length(), m_comment);
            setCurrentBlockState(InBlockComment);
            return;
        }
        setFormat(0, close + 2, m_comment);
        offset = close + 2;
    }

    setCurrentBlockState(Normal);

    // ── Single-line rules ────────────────────────────────────────────────────
    for (const Rule &rule : std::as_const(m_rules)) {
        auto it = rule.pattern.globalMatch(text, offset);
        while (it.hasNext()) {
            const auto m = it.next();
            // Rules with a capture group highlight that group only.
            const int group = (m.lastCapturedIndex() >= 2 && rule.format == m_attribute) ? 2 : 0;
            if (m.capturedStart(group) < 0)
                continue;
            setFormat(m.capturedStart(group), m.capturedLength(group), rule.format);
        }
    }

    // ── Openers of multi-line constructs ─────────────────────────────────────
    int i = offset;
    while (i < text.length()) {
        const QChar c = text[i];
        if (c == QLatin1Char('#')) {
            setFormat(i, text.length() - i, m_comment);
            return;
        }
        if (c == QLatin1Char('"')) {
            // Skip over a complete single-line string.
            int j = i + 1;
            while (j < text.length()) {
                if (text[j] == QLatin1Char('\\')) {
                    j += 2;
                    continue;
                }
                if (text[j] == QLatin1Char('"'))
                    break;
                ++j;
            }
            setFormat(i, qMin(j + 1, text.length()) - i, m_string);
            highlightInterpolation(text, i, qMin(j + 1, text.length()));
            i = j + 1;
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < text.length() && text[i + 1] == QLatin1Char('*')) {
            const int close = text.indexOf(QStringLiteral("*/"), i + 2);
            if (close < 0) {
                setFormat(i, text.length() - i, m_comment);
                setCurrentBlockState(InBlockComment);
                return;
            }
            setFormat(i, close + 2 - i, m_comment);
            i = close + 2;
            continue;
        }
        if (c == QLatin1Char('\'') && i + 1 < text.length() && text[i + 1] == QLatin1Char('\'')) {
            const int close = text.indexOf(QStringLiteral("''"), i + 2);
            if (close < 0) {
                setFormat(i, text.length() - i, m_string);
                highlightInterpolation(text, i, text.length());
                setCurrentBlockState(InIndentString);
                return;
            }
            setFormat(i, close + 2 - i, m_string);
            highlightInterpolation(text, i, close);
            i = close + 2;
            continue;
        }
        ++i;
    }
}

} // namespace nixm
