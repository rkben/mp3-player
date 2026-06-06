#include "MainWindow.h"
#include "PlaylistModel.h"
#include "Theme.h"
#include "Logger.h"

#include <QApplication>
#include <QMetaType>
#include <QSettings>

int main(int argc, char *argv[])
{
    // Capture qDebug/qWarning/etc into the in-memory log (Settings ▸ Log) before any
    // other code runs, so startup messages are caught too.
    Logger::install();

    // Pin the FFmpeg multimedia backend (Qt 6.5+ default) so streaming + the
    // worker-thread open behave identically on every platform, rather than
    // falling back to WMF (Windows) / AVFoundation (macOS). User override wins.
    if (!qEnvironmentVariableIsSet("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    QApplication app(argc, argv);
    QApplication::setApplicationName("Pocket Player");
    QApplication::setOrganizationName("PocketPlayer");

    // Track / QList<Track> cross the worker-thread boundary via queued signals.
    qRegisterMetaType<QList<Track>>("QList<Track>");

    // Apply the saved theme before showing any UI. Default is System (native).
    QSettings s;
    const Theme::Mode mode = Theme::modeFromString(s.value("ui/theme", "system").toString());
    Theme::apply(mode, s.value("ui/themeFile").toString());
    Theme::logPlatformTheme(mode);

    MainWindow w;
    w.show();
    return app.exec();
}
