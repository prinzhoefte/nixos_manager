#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;
class QLabel;
class QWidget;

namespace nixm {

/// The JR-IT corporate palette, resolved for a light or dark surface.
///
/// The brand rule of thumb is roughly 70% blue, 20% light/neutral and 10%
/// accent. Amber is the only warm colour and is reserved for small signals —
/// status dots, the unsaved-changes marker, the current generation — never for
/// large fills or body text.
struct ThemeColors {
    // Brand constants, identical in both modes.
    QColor ink { 0x04, 0x2C, 0x53 };        // #042C53  deep blue
    QColor primary { 0x0C, 0x44, 0x7C };    // #0C447C  primary blue
    QColor secondary { 0x18, 0x5F, 0xA5 };  // #185FA5  secondary blue
    QColor accent { 0x37, 0x8A, 0xDD };     // #378ADD  light blue accent
    QColor lightBlue { 0xE6, 0xF1, 0xFB };  // #E6F1FB  light
    QColor amber { 0xF4, 0xA9, 0x3C };      // #F4A93C  amber accent

    // Surface roles, mode dependent.
    QColor window;
    QColor surface;
    QColor surfaceAlt;
    QColor surfaceSunken;
    QColor text;
    QColor textMuted;
    QColor textOnBrand;
    QColor line;
    QColor lineStrong;
    QColor brand;           // the blue to lead with on this background
    QColor brandHover;
    QColor selectionBg;
    QColor selectionText;
    QColor success;
    QColor danger;

    bool dark = false;
};

class Theme
{
public:
    enum Mode { Light, Dark, System };

    /// Installs the palette, the widget style and the stylesheet on `app`.
    static void apply(QApplication *app, Mode mode);
    static Mode mode();
    static void setMode(QApplication *app, Mode mode);

    static const ThemeColors &colors();
    static bool isDark();

    /// Manrope when it is installed, otherwise the best available sans.
    static QString uiFontFamily();
    static QString monoFontFamily();
    static bool brandFontAvailable();

    /// The JR-IT mark, rendered from the shipped artwork rather than redrawn in
    /// code. Override the file with $NIXOS_MANAGER_LOGO, or replace
    /// share/icons/logo.svg (or .png) and rebuild.
    static QIcon logo();
    static QPixmap logoPixmap(int size, qreal devicePixelRatio = 1.0);
    /// Path the mark is loaded from — a resource path unless overridden.
    static QString logoSource();

    /// Flat line icons painted at runtime, so the app needs no icon theme and
    /// no SVG plugin.
    static QIcon icon(const QString &name, const QColor &colour = QColor());
    /// A filled dot, for status columns.
    static QIcon dot(const QColor &colour);

    /// Renders a chevron to a cached PNG and returns its path. Qt stylesheets
    /// can only point `image:` at a file, and Qt cannot read SVG without the
    /// qtsvg plugin, so combo and spin box arrows go through here.
    static QString chevronPath(Qt::ArrowType direction, const QColor &colour);

    /// Marks a button as the primary action in its group.
    static void makePrimary(QWidget *button, bool primary = true);
    /// Styles a label as an uppercase, letter-spaced eyebrow.
    static void makeEyebrow(QLabel *label);

    /// CSS injected into the QTextBrowser panes so rich text matches the app.
    static QString richTextCss();

    static QString styleSheet(const ThemeColors &c);
};

} // namespace nixm
