#pragma once

#include <QWidget>

class QLabel;
class QToolButton;

namespace nixm {

/// The JR-IT lockup across the top of the window, with the current
/// configuration and the three actions that apply everywhere.
class BrandHeader : public QWidget
{
    Q_OBJECT

public:
    explicit BrandHeader(QWidget *parent = nullptr);

    void setProject(const QString &root, const QString &kind, int hostCount,
        const QString &channel);
    void clearProject();
    void setDirtyCount(int count);
    void setSaveEnabled(bool enabled);
    void applyTheme();

signals:
    void openRequested();
    void saveRequested();
    void reloadRequested();
    void themeToggleRequested();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_mark = nullptr;
    QLabel *m_wordmark = nullptr;
    QLabel *m_project = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_dirty = nullptr;
    QToolButton *m_open = nullptr;
    QToolButton *m_save = nullptr;
    QToolButton *m_reload = nullptr;
    QToolButton *m_theme = nullptr;
};

} // namespace nixm
