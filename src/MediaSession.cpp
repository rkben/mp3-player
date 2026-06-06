#include "MediaSession.h"

#ifdef HAVE_MPRIS
#include "MprisController.h"
#endif

#ifdef Q_OS_MACOS
#include "NowPlayingSession.h"
#endif

MediaSession *MediaSession::create(PlayerController *player, QWidget *window,
                                   QObject *parent)
{
#ifdef HAVE_MPRIS
    return new MprisController(player, window, parent);
#elif defined(Q_OS_MACOS)
    return new NowPlayingSession(player, window, parent);
#else
    // No OS media session on this platform/build yet (Windows SMTC implementation
    // slots in here).
    Q_UNUSED(player);
    Q_UNUSED(window);
    Q_UNUSED(parent);
    return nullptr;
#endif
}
