#pragma once

#include <QObject>
#include <QString>

class PlayerController;
class QLocalSocket;
class QTimer;
class QJsonObject;

// Discord Rich Presence over Discord's local IPC socket (a named pipe on Windows, a
// unix-domain socket elsewhere). Speaks the framed-JSON protocol directly via
// QLocalSocket, so it needs no third-party library. Orthogonal to MediaSession: it
// runs alongside MPRIS/macOS on every platform, gated by the ENABLE_DISCORD_RPC build
// flag (HAVE_DISCORD_RPC).
//
// Lifecycle is fire-and-forget: construct it with the player and forget it. It tracks
// playback via the controller's signals, reconnects if Discord isn't running yet, and
// clears its status on teardown.
class DiscordPresence : public QObject
{
    Q_OBJECT
public:
    explicit DiscordPresence(PlayerController *player, QObject *parent = nullptr);
    ~DiscordPresence() override;

    // The effective application ID: the user's Settings override (discord/appId) if
    // set, else the compiled-in DISCORD_APP_ID. Empty -> the feature stays idle.
    static QString effectiveAppId();

private:
    void tryConnect();             // attempt the next/available IPC socket
    void onConnected();            // send the handshake
    void onReadyRead();            // parse incoming frames (await READY, etc.)
    void onDisconnected();         // arm the reconnect timer
    void scheduleUpdate();         // coalesce bursts into one SET_ACTIVITY
    void pushActivity();           // build + send the current activity

    void sendFrame(int opcode, const QJsonObject &payload);

    PlayerController *m_player;
    QLocalSocket *m_socket;
    QTimer *m_reconnect;           // retry when Discord is absent/closed
    QTimer *m_debounce;            // throttle activity updates (Discord rate-limits)
    QByteArray m_rx;               // accumulates partial frames
    QString m_appId;
    bool m_ready = false;          // handshake acknowledged (READY received)
    int m_nonce = 0;
};
