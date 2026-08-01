#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

namespace nixm {

/// Syntax highlighting for the Nix language. Handles the two multi-line
/// constructs (`''…''` strings and `/* … */` comments) via block state.
class NixHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit NixHighlighter(QTextDocument *parent = nullptr);

    /// Rebuilds the palette for a light or dark background.
    void setDarkMode(bool dark);

protected:
    void highlightBlock(const QString &text) override;

private:
    enum BlockState { Normal = 0, InIndentString = 1, InBlockComment = 2 };

    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    void buildRules();
    void highlightInterpolation(const QString &text, int from, int to);

    QVector<Rule> m_rules;
    QTextCharFormat m_keyword;
    QTextCharFormat m_builtin;
    QTextCharFormat m_string;
    QTextCharFormat m_comment;
    QTextCharFormat m_path;
    QTextCharFormat m_number;
    QTextCharFormat m_attribute;
    QTextCharFormat m_operatorFmt;
    QTextCharFormat m_interpolation;
    bool m_dark = false;
};

} // namespace nixm
