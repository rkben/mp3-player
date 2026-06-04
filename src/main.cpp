#include "MainWindow.h"
#include "PlaylistModel.h"
#include "Theme.h"

#include <QApplication>
#include <QMetaType>
#include <QSettings>

int main(int argc, char *argv[])
{
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
