#pragma once

#include <QString>

// App theming. Three modes:
//   System  - no stylesheet; inherit the platform theme (Breeze/KDE colour
//             scheme, fonts, icons via the KDE platform theme plugin).
//   Dark    - a built-in dark theme generated from a small token set.
//   Custom  - load an external Qt stylesheet (.qss) chosen by the user.
//
// The built-in theme is deliberately tiny: a handful of colour and font-size
// tokens feed one QSS template, rather than hand-tuned rules per widget.
namespace Theme {

enum class Mode { System, Dark, Custom };

// Colour + font tokens that drive the built-in stylesheet template.
struct Tokens {
    QString bg;          // window background
    QString surface;     // panels, inputs, headers, lists
    QString surfaceAlt;  // alternate rows / inset backdrops
    QString text;        // primary text
    QString textDim;     // secondary/dim text
    QString accent;      // highlights, selection, active tab
    QString border;      // separators, outlines
    int fontBase = 14;   // body text (px)
    int fontTitle = 15;  // track title (px)
    int fontSmall = 12;  // captions/status (px)
};

Tokens darkTokens();

// Build a QSS stylesheet string from a token set.
QString buildStyleSheet(const Tokens &t);

// Apply a theme to the whole application (qApp). For Custom, customPath points
// to a .qss file; if it can't be read, falls back to System (empty stylesheet).
void apply(Mode mode, const QString &customPath = QString());

// Persistence helpers: stable strings for QSettings.
QString modeToString(Mode mode);
Mode modeFromString(const QString &s);

// Log (qInfo) which platform/widget theme resolved at startup: the active QStyle
// (e.g. "breeze" means KDE's platform theme plugin is active), QPA platform,
// icon theme, and the env vars that drive theme resolution. Call after apply().
void logPlatformTheme(Mode mode);

} // namespace Theme
