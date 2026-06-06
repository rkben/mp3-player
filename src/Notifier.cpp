#include "Notifier.h"
#include "Toast.h"

#include <QCoreApplication>
#include <QApplication>
#include <QSystemTrayIcon>
#include <QStyle>
#include <QIcon>

#ifdef HAVE_MPRIS
#include <QDBusInterface>
#include <QDBusConnection>
#include <QVariantList>
#endif

namespace Notifier {

void notify(const QString &title, const QString &body)
{
#ifdef HAVE_MPRIS
    if (QDBusConnection::sessionBus().isConnected()) {
        QDBusInterface iface(QStringLiteral("org.freedesktop.Notifications"),
                             QStringLiteral("/org/freedesktop/Notifications"),
                             QStringLiteral("org.freedesktop.Notifications"),
                             QDBusConnection::sessionBus());
        if (iface.isValid()) {
            iface.call(QStringLiteral("Notify"),
                       QCoreApplication::applicationName(),  // app_name
                       0u,                                   // replaces_id
                       QString(),                            // app_icon
                       title, body,
                       QStringList(),                        // actions
                       QVariantMap(),                        // hints
                       5000);                                // timeout ms
            return;
        }
    }
#endif

    // Portable native path (primarily Windows/macOS, and Linux without D-Bus): a
    // lazily-created, persistent tray icon. Reused across calls; parented to the
    // app so it's cleaned up at exit.
    if (QSystemTrayIcon::isSystemTrayAvailable()
        && QSystemTrayIcon::supportsMessages()) {
        static QSystemTrayIcon *tray = nullptr;
        if (!tray) {
            tray = new QSystemTrayIcon(qApp);
            QIcon icon = qApp->windowIcon();
            if (icon.isNull())
                icon = qApp->style()->standardIcon(QStyle::SP_MediaPlay);
            tray->setIcon(icon);
            tray->setToolTip(QCoreApplication::applicationName());
            tray->show();
        }
        tray->showMessage(title, body, QSystemTrayIcon::Information, 5000);
        return;
    }

    // Last resort: surface it in-app so the message isn't lost.
    ToastArea::post(body.isEmpty() ? title : title + QStringLiteral(" — ") + body);
}

}  // namespace Notifier
