#include "BrandHeader.h"

#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace nixm {
namespace {

QToolButton *makeAction(QWidget *parent, const QString &iconName, const QString &text,
    const QString &tip)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tip);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("iconName", iconName);
    return button;
}

} // namespace

BrandHeader::BrandHeader(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 10, 14, 10);
    layout->setSpacing(12);

    // ── Lockup: mark + wordmark ──────────────────────────────────────────────
    m_mark = new QLabel(this);
    m_mark->setFixedSize(34, 34);
    layout->addWidget(m_mark);

    auto *words = new QVBoxLayout;
    words->setSpacing(1);
    words->setContentsMargins(0, 0, 0, 0);
    m_wordmark = new QLabel(this);
    m_wordmark->setTextFormat(Qt::RichText);
    words->addWidget(m_wordmark);
    m_meta = new QLabel(this);
    words->addWidget(m_meta);
    layout->addLayout(words);

    layout->addSpacing(10);

    // ── Current configuration ────────────────────────────────────────────────
    m_project = new QLabel(this);
    m_project->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_project->setTextFormat(Qt::RichText);
    layout->addWidget(m_project);

    m_dirty = new QLabel(this);
    m_dirty->setTextFormat(Qt::RichText);
    m_dirty->setVisible(false);
    layout->addWidget(m_dirty);

    layout->addStretch(1);

    // ── Global actions ───────────────────────────────────────────────────────
    m_open = makeAction(this, QStringLiteral("open"), tr("Open"),
        tr("Open another NixOS configuration"));
    connect(m_open, &QToolButton::clicked, this, &BrandHeader::openRequested);
    layout->addWidget(m_open);

    m_save = makeAction(this, QStringLiteral("save"), tr("Save"),
        tr("Write every modified file back to disk"));
    connect(m_save, &QToolButton::clicked, this, &BrandHeader::saveRequested);
    layout->addWidget(m_save);

    m_reload = makeAction(this, QStringLiteral("reload"), tr("Reload"),
        tr("Re-read the configuration from disk"));
    connect(m_reload, &QToolButton::clicked, this, &BrandHeader::reloadRequested);
    layout->addWidget(m_reload);

    m_theme = makeAction(this, QStringLiteral("theme"), QString(),
        tr("Switch between the light and dark theme"));
    m_theme->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(m_theme, &QToolButton::clicked, this, &BrandHeader::themeToggleRequested);
    layout->addWidget(m_theme);

    applyTheme();
    clearProject();
}

void BrandHeader::applyTheme()
{
    const ThemeColors &c = Theme::colors();

    m_mark->setPixmap(Theme::logoPixmap(34, devicePixelRatioF()));

    m_wordmark->setText(
        QStringLiteral("<span style='font-size:14pt;font-weight:800;letter-spacing:-0.4px;"
                       "color:%1;'>NixOS<span style='color:%2;'>&nbsp;</span>Manager</span>")
            .arg(c.dark ? c.lightBlue.name() : c.primary.name(), c.accent.name()));

    m_meta->setText(QStringLiteral("<span style='font-size:8pt;font-weight:700;"
                                   "letter-spacing:2.4px;color:%1;'>JR-IT SERVICES</span>")
                        .arg(c.accent.name()));
    m_meta->setTextFormat(Qt::RichText);

    for (QToolButton *button : { m_open, m_save, m_reload, m_theme }) {
        button->setIcon(Theme::icon(button->property("iconName").toString(), c.text));
        button->setIconSize(QSize(17, 17));
        button->setStyleSheet(
            QStringLiteral("QToolButton { border: 1px solid transparent; border-radius: 7px;"
                           "             padding: 5px 10px; font-weight: 600; color: %1; }"
                           "QToolButton:hover { background: %2; border-color: %3; color: %4; }"
                           "QToolButton:disabled { color: %5; }")
                .arg(c.text.name(), c.surfaceAlt.name(), c.line.name(), c.brand.name(),
                    c.textMuted.name()));
    }
    update();
}

void BrandHeader::setProject(const QString &root, const QString &kind, int hostCount,
    const QString &channel)
{
    const ThemeColors &c = Theme::colors();
    m_project->setText(
        QStringLiteral("<span style='color:%1;font-weight:700;'>%2</span>"
                       "<br><span style='color:%3;font-size:8.5pt;'>%4 · %5 · %6</span>")
            .arg(c.text.name(), root.toHtmlEscaped(), c.textMuted.name(), kind.toHtmlEscaped())
            .arg(tr("%n host(s)", nullptr, hostCount))
            .arg(channel.toHtmlEscaped()));
    m_project->setVisible(true);
}

void BrandHeader::clearProject()
{
    const ThemeColors &c = Theme::colors();
    m_project->setText(QStringLiteral("<span style='color:%1;'>%2</span>")
                           .arg(c.textMuted.name(), tr("No configuration open")));
}

void BrandHeader::setDirtyCount(int count)
{
    if (count <= 0) {
        m_dirty->setVisible(false);
        return;
    }
    const ThemeColors &c = Theme::colors();
    // Amber is the app's single "needs your attention" signal.
    m_dirty->setText(
        QStringLiteral("<span style='color:%1;font-weight:700;font-size:8.5pt;'>&#9679; %2</span>")
            .arg(c.amber.name(), tr("%n unsaved file(s)", nullptr, count)));
    m_dirty->setVisible(true);
}

void BrandHeader::setSaveEnabled(bool enabled)
{
    m_save->setEnabled(enabled);
}

void BrandHeader::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    const ThemeColors &c = Theme::colors();

    QPainter p(this);
    p.fillRect(rect(), c.surface);

    // A thin brand rule along the bottom, fading from accent blue to amber —
    // the same device as the rule on the design guide's cover.
    QLinearGradient rule(0, 0, width() * 0.55, 0);
    rule.setColorAt(0.0, c.accent);
    rule.setColorAt(1.0, c.amber);
    p.fillRect(QRect(0, height() - 2, width(), 2), c.line);
    p.fillRect(QRect(0, height() - 2, int(width() * 0.28), 2), QBrush(rule));
}

} // namespace nixm
