#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QPalette>
#include <QStyle>
#include <QTextStream>

#include <cstdio>

namespace Theme {

Tokens darkTokens()
{
    Tokens t;
    t.bg = "#15171c";
    t.surface = "#1b1e25";
    t.surfaceAlt = "#23272f";
    t.text = "#e8eaed";
    t.textDim = "#8a93a3";
    t.accent = "#2f6df0";
    t.border = "#2a2f3a";
    t.fontBase = 14;
    t.fontTitle = 15;
    t.fontSmall = 12;
    return t;
}

// Base widget colours (window/base/text/highlight/…) come from the palette; this
// slim QSS only covers what a palette can't express: elevated surfaces, the
// active-tab underline, rounded inputs, flat round buttons, and slider/scrollbar
// shapes. Everything that varies is still a token.
QString buildStyleSheet(const Tokens &t)
{
    return QStringLiteral(R"(
        /* Elevated surfaces (distinct from the window background). */
        QWidget#infoPanel, QFrame#transportBar { background:%2; }
        QFrame#transportBar { border-top:1px solid %7; }
        QLabel#coverArt { background:%3; border-radius:6px; }
        QLabel#status {
            color:%5; font-size:%10px; background:%2;
            border-top:1px solid %7; padding:4px 12px;
        }

        /* Typography accents. */
        QLabel#infoTitle, QLabel#queueTitle { font-size:%9px; font-weight:700; }
        QLabel#infoArtist { color:%6; }
        QLabel#infoAlbum { color:%5; font-size:%10px; }

        /* Tabs: flat pane, no rounding, accent underline on the active tab. */
        QTabWidget::pane { border:0; }
        QTabBar { background:transparent; }
        QTabBar::tab {
            background:transparent; color:%5; padding:7px 14px;
            border:0; border-radius:0;
        }
        QTabBar::tab:selected { color:%4; border-bottom:2px solid %6; }

        /* Lists: flat (no frame/rounding); colours come from the palette. */
        QTreeView, QTableView { border:0; border-radius:0; }
        QHeaderView::section {
            background:%3; color:%5; padding:5px 8px; border:0;
            border-right:1px solid %1;
        }
        QSplitter::handle { background:%7; }

        /* Inputs: rounded, accent focus ring. */
        QLineEdit, QComboBox { border:1px solid %7; border-radius:6px; padding:5px 8px; }
        QLineEdit:focus { border:1px solid %6; }

        /* Flat round transport buttons; accent fill when toggled on. */
        QToolButton { background:transparent; border-radius:24px; }
        QToolButton:checked { background:%6; }

        /* Slider + scrollbar shapes. */
        QScrollBar:vertical { background:transparent; width:10px; margin:0; }
        QScrollBar::handle:vertical { background:%7; border-radius:5px; min-height:24px; }
        QScrollBar::add-line, QScrollBar::sub-line { height:0; }
        QSlider::groove:horizontal { height:5px; background:%7; border-radius:2px; }
        QSlider::sub-page:horizontal { background:%6; border-radius:2px; }
        QSlider::handle:horizontal { background:%4; width:16px; margin:-6px 0; border-radius:8px; }
    )")
        .arg(t.bg, t.surface, t.surfaceAlt, t.text, t.textDim, t.accent, t.border)
        .arg(t.fontTitle).arg(t.fontSmall);
}

// A dark QPalette built from the same tokens, so native widgets, icon tinting,
// and placeholder colours all resolve correctly under the built-in dark theme.
static QPalette darkPalette(const Tokens &t)
{
    const QColor bg(t.bg), surface(t.surface), surfaceAlt(t.surfaceAlt),
                 text(t.text), textDim(t.textDim), accent(t.accent);
    QPalette p;
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, surface);
    p.setColor(QPalette::AlternateBase, surfaceAlt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, surface);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, textDim);
    p.setColor(QPalette::ToolTipBase, surfaceAlt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Disabled, QPalette::Text, textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDim);
    return p;
}

void apply(Mode mode, const QString &customPath)
{
    // The platform/native palette, captured once before we override it, so System
    // and Custom can restore it.
    static const QPalette nativePalette = qApp->palette();

    switch (mode) {
    case Mode::System:
        qApp->setStyleSheet(QString());      // inherit the platform theme
        qApp->setPalette(nativePalette);
        break;
    case Mode::Dark:
        qApp->setPalette(darkPalette(darkTokens()));
        qApp->setStyleSheet(buildStyleSheet(darkTokens()));
        break;
    case Mode::Custom: {
        qApp->setPalette(nativePalette);     // custom sheets author their own colours
        QFile f(customPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            qApp->setStyleSheet(in.readAll());
        } else {
            qApp->setStyleSheet(QString());   // unreadable -> fall back to native
        }
        break;
    }
    }
}

QString modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Dark:   return QStringLiteral("dark");
    case Mode::Custom: return QStringLiteral("custom");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

Mode modeFromString(const QString &s)
{
    if (s == QLatin1String("dark"))   return Mode::Dark;
    if (s == QLatin1String("custom")) return Mode::Custom;
    return Mode::System;
}

void logPlatformTheme(Mode mode)
{
    const QString style = qApp->style() ? qApp->style()->objectName() : QStringLiteral("?");
    const bool kdeNative = style.compare(QLatin1String("breeze"), Qt::CaseInsensitive) == 0;

    auto envOrUnset = [](const char *name) {
        return qEnvironmentVariableIsSet(name) ? qEnvironmentVariable(name)
                                               : QStringLiteral("<unset>");
    };

    QString verdict;
    if (mode == Mode::System)
        verdict = kdeNative ? QStringLiteral(" -> KDE platform theme active")
                            : QStringLiteral(" -> non-KDE style (plasma-integration missing?)");

    // qInfo() is suppressed in this Release build, so log to stderr directly.
    fprintf(stderr,
            "Theme: mode=%s style=%s platform=%s iconTheme=%s | "
            "QT_QPA_PLATFORMTHEME=%s XDG_CURRENT_DESKTOP=%s%s\n",
            qPrintable(modeToString(mode)),
            qPrintable(style),
            qPrintable(qApp->platformName()),
            qPrintable(QIcon::themeName()),
            qPrintable(envOrUnset("QT_QPA_PLATFORMTHEME")),
            qPrintable(envOrUnset("XDG_CURRENT_DESKTOP")),
            qPrintable(verdict));
}

} // namespace Theme
