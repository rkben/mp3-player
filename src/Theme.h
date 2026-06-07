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
//
// These are the built-in *Dark* theme's knobs. They only affect Dark mode — System
// inherits the platform style untouched (see the macOS notes by apply() below), and
// Custom authors its own sheet. Tune a value here and the whole Dark theme follows.
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
    // Geometry knobs (px). Pulled into the QSS so corner rounding and the round
    // transport buttons are tunable from here rather than buried as literals.
    int radius = 6;        // inputs, cover, general corner rounding
    int buttonRadius = 24; // flat round transport buttons (half the 44px hit target)
    // Slider + scrollbar dimensions. Only the primary thickness is a knob; the
    // dependent corner radii and the handle's centring margin are derived from
    // these in buildStyleSheet so changing one value keeps the shape coherent.
    int scrollbarWidth = 10; // vertical scrollbar track/handle thickness
    int sliderGroove = 5;    // seek/volume groove height
    int sliderHandle = 16;   // seek/volume handle diameter
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
