#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStandardPaths>
#include <QStyleOption>
#include <QWidget>

namespace nixm {
namespace {

ThemeColors g_colors;
Theme::Mode g_mode = Theme::System;

ThemeColors lightColors()
{
    ThemeColors c;
    c.dark = false;
    c.window = QColor(0xF4, 0xF6, 0xF9);         // soft neutral page
    c.surface = QColor(0xFF, 0xFF, 0xFF);
    c.surfaceAlt = QColor(0xF7, 0xFA, 0xFD);
    c.surfaceSunken = QColor(0xEC, 0xF2, 0xF9);
    c.text = c.ink;
    c.textMuted = QColor(0x6B, 0x7A, 0x88);
    c.textOnBrand = QColor(0xFF, 0xFF, 0xFF);
    c.line = QColor(0xE4, 0xE7, 0xEC);
    c.lineStrong = QColor(0xCF, 0xDA, 0xE6);
    c.brand = c.primary;
    c.brandHover = c.secondary;
    c.selectionBg = c.lightBlue;
    c.selectionText = c.primary;
    c.success = QColor(0x1B, 0x8A, 0x53);
    c.danger = QColor(0xC0, 0x35, 0x3A);
    return c;
}

ThemeColors darkColors()
{
    ThemeColors c;
    c.dark = true;
    c.window = QColor(0x03, 0x1E, 0x39);         // one step below ink
    c.surface = QColor(0x06, 0x2A, 0x4C);
    c.surfaceAlt = QColor(0x09, 0x33, 0x5C);
    c.surfaceSunken = QColor(0x02, 0x18, 0x2E);
    c.text = QColor(0xE6, 0xF1, 0xFB);
    c.textMuted = QColor(0x8F, 0xA9, 0xC4);
    c.textOnBrand = QColor(0xFF, 0xFF, 0xFF);
    c.line = QColor(0x11, 0x3E, 0x68);
    c.lineStrong = QColor(0x1B, 0x53, 0x86);
    c.brand = c.accent;
    c.brandHover = QColor(0x5C, 0xA2, 0xE8);
    c.selectionBg = QColor(0x12, 0x4A, 0x81);
    c.selectionText = QColor(0xFF, 0xFF, 0xFF);
    c.success = QColor(0x4A, 0xD1, 0x8C);
    c.danger = QColor(0xF0, 0x6B, 0x70);
    return c;
}

QString rgba(const QColor &c, qreal alpha = 1.0)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(QString::number(alpha, 'f', 3));
}

/// Draws the check marks, radio buttons and tree arrows so they follow the
/// brand palette. Doing this in a style rather than in the stylesheet keeps
/// Qt's native sizing and avoids needing the SVG image plugin.
class BrandStyle : public QProxyStyle
{
public:
    BrandStyle()
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
    {
    }

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
        QStyleHintReturn *returnData) const override
    {
        switch (hint) {
        case SH_EtchDisabledText:
        case SH_DitherDisabledText:
            return 0;
        case SH_Menu_Scrollable:
            return 1;
        default:
            break;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option,
        const QWidget *widget) const override
    {
        switch (metric) {
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
        case PM_ExclusiveIndicatorWidth:
        case PM_ExclusiveIndicatorHeight:
            return 17;
        case PM_CheckBoxLabelSpacing:
        case PM_RadioButtonLabelSpacing:
            return 7;
        case PM_SmallIconSize:
            return 16;
        case PM_ToolBarIconSize:
            return 18;
        default:
            break;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
        const QWidget *widget) const override
    {
        const ThemeColors &c = Theme::colors();

        // Qt's dotted focus rectangle fights with our focus rings.
        if (element == PE_FrameFocusRect)
            return;

        if (element == PE_IndicatorCheckBox || element == PE_IndicatorRadioButton) {
            const bool on = option->state & State_On;
            const bool tri = option->state & State_NoChange;
            const bool enabled = option->state & State_Enabled;
            const bool hover = option->state & State_MouseOver;
            const bool radio = element == PE_IndicatorRadioButton;

            const int side = qMin(option->rect.width(), option->rect.height());
            QRectF box(0, 0, side, side);
            box.moveCenter(QRectF(option->rect).center());
            box.adjust(1.0, 1.0, -1.0, -1.0);

            QColor fill = (on || tri) ? c.brand : c.surface;
            QColor border = (on || tri) ? c.brand : (hover ? c.brand : c.lineStrong);
            if (!enabled) {
                fill.setAlphaF(0.35f);
                border.setAlphaF(0.35f);
            }

            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(QPen(border, 1.4));
            painter->setBrush(fill);
            if (radio)
                painter->drawEllipse(box);
            else
                painter->drawRoundedRect(box, 4.5, 4.5);

            if (radio && on) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(c.textOnBrand);
                painter->drawEllipse(box.center(), side * 0.17, side * 0.17);
            } else if (tri) {
                painter->setPen(QPen(c.textOnBrand, 1.8, Qt::SolidLine, Qt::RoundCap));
                painter->drawLine(QPointF(box.left() + side * 0.24, box.center().y()),
                    QPointF(box.right() - side * 0.24, box.center().y()));
            } else if (on) {
                QPainterPath tick;
                tick.moveTo(box.left() + side * 0.24, box.top() + side * 0.50);
                tick.lineTo(box.left() + side * 0.42, box.top() + side * 0.67);
                tick.lineTo(box.right() - side * 0.21, box.top() + side * 0.30);
                painter->setBrush(Qt::NoBrush);
                painter->setPen(QPen(c.textOnBrand, 1.9, Qt::SolidLine, Qt::RoundCap,
                    Qt::RoundJoin));
                painter->drawPath(tick);
            }
            painter->restore();
            return;
        }

        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

/// Paints one of the flat line icons. Everything is drawn in a 24×24 box and
/// scaled, so a single description works at any size.
void paintIcon(QPainter *p, const QString &name, const QColor &colour, int size)
{
    p->setRenderHint(QPainter::Antialiasing, true);
    p->scale(size / 24.0, size / 24.0);

    QPen pen(colour, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);

    if (name == QLatin1String("open")) {
        QPainterPath path;
        path.moveTo(3, 19);
        path.lineTo(3, 6);
        path.lineTo(9.5, 6);
        path.lineTo(11.5, 8.5);
        path.lineTo(21, 8.5);
        path.lineTo(21, 19);
        path.closeSubpath();
        p->drawPath(path);
    } else if (name == QLatin1String("save")) {
        p->drawRoundedRect(QRectF(3.5, 3.5, 17, 17), 2.5, 2.5);
        p->drawLine(QPointF(8, 3.5), QPointF(8, 9.5));
        p->drawLine(QPointF(8, 9.5), QPointF(16, 9.5));
        p->drawLine(QPointF(16, 9.5), QPointF(16, 3.5));
        p->drawRect(QRectF(8, 14, 8, 6.5));
    } else if (name == QLatin1String("reload")) {
        p->drawArc(QRectF(4, 4, 16, 16), 40 * 16, 280 * 16);
        QPainterPath head;
        head.moveTo(20, 4.5);
        head.lineTo(20, 10);
        head.lineTo(14.5, 10);
        p->drawPath(head);
    } else if (name == QLatin1String("search")) {
        p->drawEllipse(QPointF(10.5, 10.5), 6.5, 6.5);
        p->drawLine(QPointF(15.3, 15.3), QPointF(20.5, 20.5));
    } else if (name == QLatin1String("add")) {
        p->drawLine(QPointF(12, 5), QPointF(12, 19));
        p->drawLine(QPointF(5, 12), QPointF(19, 12));
    } else if (name == QLatin1String("remove")) {
        p->drawLine(QPointF(5, 12), QPointF(19, 12));
    } else if (name == QLatin1String("trash")) {
        p->drawLine(QPointF(4, 6.5), QPointF(20, 6.5));
        p->drawPath([] {
            QPainterPath path;
            path.moveTo(6.5, 6.5);
            path.lineTo(7.5, 20);
            path.lineTo(16.5, 20);
            path.lineTo(17.5, 6.5);
            return path;
        }());
        p->drawLine(QPointF(9.5, 6.5), QPointF(10, 3.8));
        p->drawLine(QPointF(14.5, 6.5), QPointF(14, 3.8));
        p->drawLine(QPointF(10, 3.8), QPointF(14, 3.8));
    } else if (name == QLatin1String("run")) {
        QPainterPath tri;
        tri.moveTo(7.5, 4.8);
        tri.lineTo(19.5, 12);
        tri.lineTo(7.5, 19.2);
        tri.closeSubpath();
        p->setBrush(colour);
        p->drawPath(tri);
    } else if (name == QLatin1String("cancel")) {
        p->drawLine(QPointF(6, 6), QPointF(18, 18));
        p->drawLine(QPointF(18, 6), QPointF(6, 18));
    } else if (name == QLatin1String("host")) {
        p->drawRoundedRect(QRectF(3, 4.5, 18, 12), 2, 2);
        p->drawLine(QPointF(8.5, 20), QPointF(15.5, 20));
        p->drawLine(QPointF(12, 16.5), QPointF(12, 20));
    } else if (name == QLatin1String("module")) {
        p->drawRoundedRect(QRectF(3.5, 3.5, 7.5, 7.5), 1.6, 1.6);
        p->drawRoundedRect(QRectF(13, 3.5, 7.5, 7.5), 1.6, 1.6);
        p->drawRoundedRect(QRectF(3.5, 13, 7.5, 7.5), 1.6, 1.6);
        p->drawRoundedRect(QRectF(13, 13, 7.5, 7.5), 1.6, 1.6);
    } else if (name == QLatin1String("package")) {
        QPainterPath box;
        box.moveTo(12, 3);
        box.lineTo(20.5, 7.5);
        box.lineTo(20.5, 16.5);
        box.lineTo(12, 21);
        box.lineTo(3.5, 16.5);
        box.lineTo(3.5, 7.5);
        box.closeSubpath();
        p->drawPath(box);
        p->drawLine(QPointF(3.5, 7.5), QPointF(12, 12));
        p->drawLine(QPointF(20.5, 7.5), QPointF(12, 12));
        p->drawLine(QPointF(12, 12), QPointF(12, 21));
    } else if (name == QLatin1String("editor")) {
        p->drawLine(QPointF(4, 20), QPointF(8, 19));
        QPainterPath nib;
        nib.moveTo(7.5, 17.5);
        nib.lineTo(17.5, 4.8);
        nib.lineTo(20.2, 6.9);
        nib.lineTo(10.2, 19.6);
        nib.closeSubpath();
        p->drawPath(nib);
    } else if (name == QLatin1String("system")) {
        p->drawLine(QPointF(4, 7.5), QPointF(20, 7.5));
        p->drawLine(QPointF(4, 16.5), QPointF(20, 16.5));
        p->setBrush(colour);
        p->drawEllipse(QPointF(9, 7.5), 2.6, 2.6);
        p->drawEllipse(QPointF(15.5, 16.5), 2.6, 2.6);
    } else if (name == QLatin1String("options")) {
        // A key: options are the knobs that unlock behaviour.
        p->drawEllipse(QPointF(8, 9), 4.6, 4.6);
        p->drawLine(QPointF(11.3, 12.3), QPointF(20, 21));
        p->drawLine(QPointF(17.5, 18.5), QPointF(15.5, 20.5));
        p->drawLine(QPointF(20, 21), QPointF(18, 23));
    } else if (name == QLatin1String("broom")) {
        p->drawLine(QPointF(16.5, 4), QPointF(10.5, 10));
        QPainterPath head;
        head.moveTo(8, 11.5);
        head.lineTo(13, 16.5);
        head.lineTo(9.5, 20.5);
        head.lineTo(4, 15);
        head.closeSubpath();
        p->drawPath(head);
    } else if (name == QLatin1String("theme")) {
        p->drawEllipse(QPointF(12, 12), 7.5, 7.5);
        QPainterPath half;
        half.moveTo(12, 4.5);
        half.arcTo(QRectF(4.5, 4.5, 15, 15), 90, -180);
        half.closeSubpath();
        p->setBrush(colour);
        p->setPen(Qt::NoPen);
        p->drawPath(half);
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

const ThemeColors &Theme::colors()
{
    return g_colors;
}

bool Theme::isDark()
{
    return g_colors.dark;
}

Theme::Mode Theme::mode()
{
    return g_mode;
}

bool Theme::brandFontAvailable()
{
    static const bool available
        = QFontDatabase::families().contains(QStringLiteral("Manrope"), Qt::CaseInsensitive);
    return available;
}

QString Theme::uiFontFamily()
{
    if (brandFontAvailable())
        return QStringLiteral("Manrope");
    // Closest widely-installed geometric sans faces, then whatever exists.
    const QStringList families = QFontDatabase::families();
    for (const QString &candidate : { QStringLiteral("Inter"), QStringLiteral("Cantarell"),
             QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans") }) {
        if (families.contains(candidate, Qt::CaseInsensitive))
            return candidate;
    }
    return QApplication::font().family();
}

QString Theme::monoFontFamily()
{
    const QStringList families = QFontDatabase::families();
    for (const QString &candidate : { QStringLiteral("JetBrains Mono"),
             QStringLiteral("Fira Code"), QStringLiteral("Fira Mono"),
             QStringLiteral("Source Code Pro"), QStringLiteral("Noto Sans Mono"),
             QStringLiteral("DejaVu Sans Mono") }) {
        if (families.contains(candidate, Qt::CaseInsensitive))
            return candidate;
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

void Theme::apply(QApplication *app, Mode requested)
{
    g_mode = requested;

    bool dark = false;
    if (requested == Dark) {
        dark = true;
    } else if (requested == System) {
        // Qt 6.4 has no colour-scheme API; infer from the platform palette.
        const QColor base = app->palette().color(QPalette::Window);
        dark = base.lightness() < 128;
    }

    g_colors = dark ? darkColors() : lightColors();
    const ThemeColors &c = g_colors;

    static bool styleInstalled = false;
    if (!styleInstalled) {
        app->setStyle(new BrandStyle);
        styleInstalled = true;
    }

    QFont uiFont(uiFontFamily());
    uiFont.setPointSizeF(app->font().pointSizeF() > 0 ? app->font().pointSizeF() : 10.0);
    app->setFont(uiFont);

    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.surface);
    p.setColor(QPalette::AlternateBase, c.surfaceAlt);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::PlaceholderText, c.textMuted);
    p.setColor(QPalette::Button, c.surface);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, c.amber);
    p.setColor(QPalette::Highlight, c.selectionBg);
    p.setColor(QPalette::HighlightedText, c.selectionText);
    p.setColor(QPalette::Link, c.brand);
    p.setColor(QPalette::LinkVisited, c.secondary);
    p.setColor(QPalette::ToolTipBase, c.ink);
    p.setColor(QPalette::ToolTipText, c.lightBlue);
    p.setColor(QPalette::Mid, c.line);
    p.setColor(QPalette::Dark, c.lineStrong);
    p.setColor(QPalette::Shadow, c.ink);
    p.setColor(QPalette::Disabled, QPalette::Text, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textMuted);
    app->setPalette(p);

    app->setStyleSheet(styleSheet(c));
}

void Theme::setMode(QApplication *app, Mode requested)
{
    apply(app, requested);
}

// ─────────────────────────────────────────────────────────────────────────────

QString Theme::styleSheet(const ThemeColors &c)
{
    const QString mono = monoFontFamily();

    QString qss = QStringLiteral(R"QSS(
/* ── Base ─────────────────────────────────────────────────────────────── */
QWidget {
    color: %TEXT%;
}
QMainWindow, QDialog, QScrollArea, QScrollArea > QWidget > QWidget {
    background: %WINDOW%;
}
QToolTip {
    background: %INK%;
    color: %LIGHTBLUE%;
    border: 1px solid %LINESTRONG%;
    border-radius: 6px;
    padding: 5px 8px;
}

/* ── Menu bar ─────────────────────────────────────────────────────────── */
QMenuBar {
    background: %SURFACE%;
    border-bottom: 1px solid %LINE%;
    padding: 3px 6px;
}
QMenuBar::item {
    padding: 5px 11px;
    border-radius: 6px;
    background: transparent;
}
QMenuBar::item:selected  { background: %SELBG%; color: %SELTEXT%; }
QMenu {
    background: %SURFACE%;
    border: 1px solid %LINE%;
    border-radius: 9px;
    padding: 6px;
}
QMenu::item { padding: 7px 22px 7px 14px; border-radius: 6px; }
QMenu::item:selected  { background: %SELBG%; color: %SELTEXT%; }
QMenu::item:disabled  { color: %MUTED%; }
QMenu::separator { height: 1px; background: %LINE%; margin: 5px 8px; }

/* ── Tabs ─────────────────────────────────────────────────────────────── */
QTabWidget::pane {
    border: none;
    background: %WINDOW%;
    top: 0px;
}
QTabBar { qproperty-drawBase: 0; background: transparent; }
QTabBar::tab {
    background: transparent;
    color: %MUTED%;
    border: none;
    border-bottom: 2px solid transparent;
    padding: 9px 16px;
    margin-right: 2px;
    font-weight: 600;
}
QTabBar::tab:hover    { color: %TEXT%; }
QTabBar::tab:selected {
    color: %BRAND%;
    border-bottom: 2px solid %BRAND%;
}

/* ── Cards (group boxes) ──────────────────────────────────────────────── */
QGroupBox {
    background: %SURFACE%;
    border: 1px solid %LINE%;
    border-radius: 10px;
    margin-top: 15px;
    padding: 16px 14px 13px 14px;
    font-weight: 700;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 13px;
    top: 2px;
    padding: 1px 7px;
    color: %ACCENT%;
    background: %SURFACE%;
    font-size: 10px;
    font-weight: 800;
    text-transform: uppercase;
}

/* ── Buttons ──────────────────────────────────────────────────────────── */
QPushButton {
    background: %SURFACE%;
    color: %TEXT%;
    border: 1px solid %LINESTRONG%;
    border-radius: 7px;
    padding: 6px 14px;
    font-weight: 600;
    min-height: 17px;
}
QPushButton:hover    { border-color: %BRAND%; color: %BRAND%; }
QPushButton:pressed  { background: %SUNKEN%; }
QPushButton:disabled { color: %MUTED%; border-color: %LINE%; background: transparent; }
QPushButton:focus    { border-color: %BRAND%; }

QPushButton[primary="true"] {
    background: %BRAND%;
    color: %ONBRAND%;
    border: 1px solid %BRAND%;
}
QPushButton[primary="true"]:hover    { background: %BRANDHOVER%; border-color: %BRANDHOVER%; }
QPushButton[primary="true"]:pressed  { background: %PRIMARY%; }
QPushButton[primary="true"]:disabled { background: %LINE%; border-color: %LINE%; color: %MUTED%; }

QPushButton[danger="true"]:hover { border-color: %DANGER%; color: %DANGER%; }

/* ── Text inputs ──────────────────────────────────────────────────────── */
QLineEdit, QSpinBox, QComboBox, QPlainTextEdit, QTextBrowser, QTextEdit {
    background: %SURFACE%;
    border: 1px solid %LINESTRONG%;
    border-radius: 7px;
    padding: 5px 9px;
    selection-background-color: %SELBG%;
    selection-color: %SELTEXT%;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus {
    border-color: %BRAND%;
}
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
    background: %SUNKEN%; color: %MUTED%;
}
QComboBox::drop-down { border: none; width: 24px; background: transparent; }
QComboBox::down-arrow { image: url("%CHEVRONDOWN%"); width: 20px; height: 20px; }
QComboBox QAbstractItemView {
    background: %SURFACE%;
    border: 1px solid %LINE%;
    border-radius: 8px;
    padding: 4px;
    selection-background-color: %SELBG%;
    selection-color: %SELTEXT%;
    outline: none;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 18px; border: none; background: transparent; subcontrol-origin: border;
}
QSpinBox::up-button   { subcontrol-position: top right; }
QSpinBox::down-button { subcontrol-position: bottom right; }
QSpinBox::up-arrow    { image: url("%CHEVRONUP%");   width: 16px; height: 16px; }
QSpinBox::down-arrow  { image: url("%CHEVRONDOWN%"); width: 16px; height: 16px; }

/* ── Item views ───────────────────────────────────────────────────────── */
QTreeWidget, QTreeView, QTableWidget, QTableView, QListWidget, QListView {
    background: %SURFACE%;
    alternate-background-color: %SURFACEALT%;
    border: 1px solid %LINE%;
    border-radius: 9px;
    outline: none;
    selection-background-color: %SELBG%;
    selection-color: %SELTEXT%;
}
QTreeView::item, QTableView::item, QListView::item {
    padding: 4px 2px;
    border: none;
}
QTreeView::item:hover, QTableView::item:hover, QListView::item:hover {
    background: %SURFACEALT%;
}
QTreeView::item:selected, QTableView::item:selected, QListView::item:selected {
    background: %SELBG%;
    color: %SELTEXT%;
}
QHeaderView { background: transparent; }
QHeaderView::section {
    background: %SUNKEN%;
    color: %MUTED%;
    border: none;
    border-bottom: 1px solid %LINE%;
    border-right: 1px solid %LINE%;
    padding: 6px 8px;
    font-size: 10px;
    font-weight: 800;
    text-transform: uppercase;
}
QHeaderView::section:last { border-right: none; }
QTableCornerButton::section { background: %SUNKEN%; border: none; }

/* ── Scroll bars ──────────────────────────────────────────────────────── */
QScrollBar:vertical   { background: transparent; width: 11px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: %SCROLL%;
    border-radius: 4px;
    min-height: 28px;
    min-width: 28px;
}
QScrollBar::handle:hover { background: %SCROLLHOVER%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ── Splitters, frames, misc ──────────────────────────────────────────── */
QSplitter::handle { background: transparent; }
QSplitter::handle:horizontal { width: 7px; }
QSplitter::handle:vertical   { height: 7px; }

QProgressBar {
    background: %SUNKEN%;
    border: none;
    border-radius: 4px;
    height: 7px;
    text-align: center;
    color: transparent;
}
QProgressBar::chunk { background: %ACCENT%; border-radius: 4px; }

QStatusBar {
    background: %SURFACE%;
    border-top: 1px solid %LINE%;
    color: %MUTED%;
}
QStatusBar::item { border: none; }

QDockWidget {
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background: %SUNKEN%;
    color: %MUTED%;
    padding: 7px 12px;
    border-top: 1px solid %LINE%;
    font-size: 10px;
    font-weight: 800;
    text-transform: uppercase;
}

QCheckBox, QRadioButton { spacing: 7px; background: transparent; }
QCheckBox:disabled, QRadioButton:disabled { color: %MUTED%; }

QLabel { background: transparent; }
QLabel[eyebrow="true"] {
    color: %ACCENT%;
    font-size: 10px;
    font-weight: 800;
    text-transform: uppercase;
}

/* Monospace surfaces: the log and the Nix editor. */
QPlainTextEdit[mono="true"] {
    font-family: "%MONO%";
    background: %CODEBG%;
    border: 1px solid %LINE%;
    border-radius: 9px;
    padding: 8px;
}
)QSS");

    const QHash<QString, QString> tokens {
        { QStringLiteral("%TEXT%"), c.text.name() },
        { QStringLiteral("%MUTED%"), c.textMuted.name() },
        { QStringLiteral("%WINDOW%"), c.window.name() },
        { QStringLiteral("%SURFACE%"), c.surface.name() },
        { QStringLiteral("%SURFACEALT%"), c.surfaceAlt.name() },
        { QStringLiteral("%SUNKEN%"), c.surfaceSunken.name() },
        { QStringLiteral("%LINE%"), c.line.name() },
        { QStringLiteral("%LINESTRONG%"), c.lineStrong.name() },
        { QStringLiteral("%BRAND%"), c.brand.name() },
        { QStringLiteral("%BRANDHOVER%"), c.brandHover.name() },
        { QStringLiteral("%PRIMARY%"), c.primary.name() },
        { QStringLiteral("%ACCENT%"), c.accent.name() },
        { QStringLiteral("%AMBER%"), c.amber.name() },
        { QStringLiteral("%INK%"), c.ink.name() },
        { QStringLiteral("%LIGHTBLUE%"), c.lightBlue.name() },
        { QStringLiteral("%ONBRAND%"), c.textOnBrand.name() },
        { QStringLiteral("%SELBG%"), c.selectionBg.name() },
        { QStringLiteral("%SELTEXT%"), c.selectionText.name() },
        { QStringLiteral("%DANGER%"), c.danger.name() },
        { QStringLiteral("%SCROLL%"), rgba(c.textMuted, c.dark ? 0.45 : 0.35) },
        { QStringLiteral("%SCROLLHOVER%"), rgba(c.brand, 0.65) },
        { QStringLiteral("%CODEBG%"), c.dark ? c.surfaceSunken.name() : c.surface.name() },
        { QStringLiteral("%MONO%"), mono },
        { QStringLiteral("%CHEVRONDOWN%"), chevronPath(Qt::DownArrow, c.textMuted) },
        { QStringLiteral("%CHEVRONUP%"), chevronPath(Qt::UpArrow, c.textMuted) },
    };

    for (auto it = tokens.constBegin(); it != tokens.constEnd(); ++it)
        qss.replace(it.key(), it.value());
    return qss;
}

QString Theme::richTextCss()
{
    const ThemeColors &c = g_colors;
    return QStringLiteral(R"CSS(
body   { color: %1; font-size: 10pt; }
h3     { color: %2; font-size: 13pt; margin: 0 0 6px 0; }
b      { color: %2; }
code   { font-family: "%3"; color: %4; }
a      { color: %5; text-decoration: none; }
small  { color: %6; }
i      { color: %6; }
ul     { margin-left: 6px; }
)CSS")
        .arg(c.text.name(), c.dark ? c.lightBlue.name() : c.primary.name(), monoFontFamily(),
            c.dark ? c.accent.name() : c.secondary.name(), c.brand.name(), c.textMuted.name());
}

// ─────────────────────────────────────────────────────────────────────────────

QPixmap Theme::logoPixmap(int size, qreal devicePixelRatio)
{
    QPixmap pm(int(size * devicePixelRatio), int(size * devicePixelRatio));
    pm.setDevicePixelRatio(devicePixelRatio);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.scale(size / 100.0, size / 100.0);

    const ThemeColors &c = g_colors;

    // Rounded tile with the brand gradient.
    QLinearGradient tile(0, 0, 100, 100);
    tile.setColorAt(0.0, c.primary);
    tile.setColorAt(1.0, c.ink);
    p.setPen(Qt::NoPen);
    p.setBrush(tile);
    p.drawRoundedRect(QRectF(0, 0, 100, 100), 23, 23);

    // "JR" monogram. Manrope when it is installed, otherwise the fallback sans.
    QFont monogram(uiFontFamily());
    monogram.setPixelSize(38);
    monogram.setWeight(QFont::Bold);
    monogram.setLetterSpacing(QFont::AbsoluteSpacing, -1.5);
    p.setFont(monogram);
    p.setPen(c.lightBlue);
    p.drawText(QRectF(0, 12, 100, 56), Qt::AlignCenter, QStringLiteral("JR"));

    // Circuit traces leading to the nodes.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c.accent, 2.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath left;
    left.moveTo(19, 78);
    left.lineTo(38, 78);
    left.lineTo(38, 70);
    p.drawPath(left);
    p.drawLine(QPointF(50, 78), QPointF(50, 73));
    QPainterPath right;
    right.moveTo(81, 78);
    right.lineTo(62, 78);
    right.lineTo(62, 70);
    p.drawPath(right);

    // Amber nodes — the single warm accent in the whole mark.
    p.setPen(Qt::NoPen);
    p.setBrush(c.amber);
    p.drawEllipse(QPointF(38, 66.6), 3.7, 3.7);
    p.drawEllipse(QPointF(62, 66.6), 3.7, 3.7);
    p.setBrush(c.accent);
    p.drawEllipse(QPointF(50, 69.8), 3.7, 3.7);

    return pm;
}

QIcon Theme::logo()
{
    QIcon out;
    for (int size : { 16, 24, 32, 48, 64, 128 })
        out.addPixmap(logoPixmap(size));
    return out;
}

QIcon Theme::icon(const QString &name, const QColor &colour)
{
    const QColor stroke = colour.isValid() ? colour : g_colors.text;

    QIcon out;
    for (int size : { 16, 20, 24, 32 }) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paintIcon(&p, name, stroke, size);
        out.addPixmap(pm, QIcon::Normal);

        QPixmap disabled(size, size);
        disabled.fill(Qt::transparent);
        QPainter dp(&disabled);
        paintIcon(&dp, name, g_colors.textMuted, size);
        out.addPixmap(disabled, QIcon::Disabled);
    }
    return out;
}

QString Theme::chevronPath(Qt::ArrowType direction, const QColor &colour)
{
    const QString dir
        = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
              .absoluteFilePath(QStringLiteral("ui"));
    QDir().mkpath(dir);

    const QString name = QStringLiteral("chevron-%1-%2.png")
                             .arg(int(direction))
                             .arg(colour.name(QColor::HexRgb).mid(1));
    const QString path = QDir(dir).absoluteFilePath(name);
    if (QFileInfo::exists(path))
        return path;

    constexpr int kSize = 20;
    QPixmap pm(kSize, kSize);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(colour, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.translate(kSize / 2.0, kSize / 2.0);
        switch (direction) {
        case Qt::UpArrow:
            p.rotate(180);
            break;
        case Qt::LeftArrow:
            p.rotate(90);
            break;
        case Qt::RightArrow:
            p.rotate(-90);
            break;
        default:
            break;
        }
        QPainterPath chevron;
        chevron.moveTo(-3.6, -1.8);
        chevron.lineTo(0, 1.8);
        chevron.lineTo(3.6, -1.8);
        p.drawPath(chevron);
    }
    pm.save(path, "PNG");
    return path;
}

QIcon Theme::dot(const QColor &colour)
{
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawEllipse(QPointF(6, 6), 3.6, 3.6);
    return QIcon(pm);
}

void Theme::makePrimary(QWidget *button, bool primary)
{
    if (!button)
        return;
    button->setProperty("primary", primary);
    button->style()->unpolish(button);
    button->style()->polish(button);
}

void Theme::makeEyebrow(QLabel *label)
{
    if (!label)
        return;
    label->setProperty("eyebrow", true);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

} // namespace nixm
