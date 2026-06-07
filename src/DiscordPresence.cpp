#include "DiscordPresence.h"

#include "PlayerController.h"

#include <QLocalSocket>
#include <QTimer>
#include <QSettings>
#include <QJsonObject>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QDateTime>

#ifndef DISCORD_APP_ID
#define DISCORD_APP_ID ""   // build without -DDISCORD_APP_ID -> feature idle
#endif

namespace {
constexpr int kReconnectMs = 15000;   // retry cadence when Discord isn't running
constexpr int kDebounceMs  = 2000;    // coalesce updates (Discord rate-limits activity)

// Discord IPC opcodes.
enum Opcode { OpHandshake = 0, OpFrame = 1, OpClose = 2, OpPing = 3, OpPong = 4 };

quint32 readLE32(const QByteArray &b, int at)
{
    return quint32(quint8(b[at]))
         | quint32(quint8(b[at + 1])) << 8
         | quint32(quint8(b[at + 2])) << 16
         | quint32(quint8(b[at + 3])) << 24;
}

// Candidate IPC endpoints, in priority order. On Windows these are named-pipe names;
// on Unix they are full socket paths under the runtime/temp dirs (incl. snap/flatpak
// Discord layouts). Discord uses discord-ipc-0..9.
QStringList candidateSocketNames()
{
    QStringList out;
#ifdef Q_OS_WIN
    for (int i = 0; i < 10; ++i)
        out << QStringLiteral("discord-ipc-%1").arg(i);
#else
    QStringList bases;
    for (const char *env : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        const QString v = qEnvironmentVariable(env);
        if (!v.isEmpty() && !bases.contains(v))
            bases << v;
    }
    if (!bases.contains(QStringLiteral("/tmp")))
        bases << QStringLiteral("/tmp");
    const QStringList subs = {QString(), QStringLiteral("snap.discord/"),
                              QStringLiteral("app/com.discordapp.Discord/")};
    for (const QString &b : bases)
        for (const QString &s : subs)
            for (int i = 0; i < 10; ++i)
                out << QStringLiteral("%1/%2discord-ipc-%3").arg(b, s).arg(i);
#endif
    return out;
}

// Discord requires details/state/*_text be 2..128 chars; return a clipped value or
// an empty string (caller omits the field) so we never send an invalid activity.
QString clampField(const QString &s)
{
    const QString t = s.trimmed();
    if (t.size() < 2)
        return {};
    return t.left(128);
}
}

QString DiscordPresence::effectiveAppId()
{
    const QString custom = QSettings().value(QStringLiteral("discord/appId"))
                               .toString().trimmed();
    if (!custom.isEmpty())
        return custom;
    return QString::fromLatin1(DISCORD_APP_ID).trimmed();
}

DiscordPresence::DiscordPresence(PlayerController *player, QObject *parent)
    : QObject(parent), m_player(player), m_socket(nullptr),
      m_reconnect(nullptr), m_debounce(nullptr)
{
    m_appId = effectiveAppId();
    if (m_appId.isEmpty()) {
        // Compiled in but no application ID configured — stay completely idle.
        qInfo("Discord RPC: no application ID set; presence disabled.");
        return;
    }

    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::connected, this, &DiscordPresence::onConnected);
    connect(m_socket, &QLocalSocket::readyRead, this, &DiscordPresence::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &DiscordPresence::onDisconnected);

    m_reconnect = new QTimer(this);
    m_reconnect->setSingleShot(true);
    m_reconnect->setInterval(kReconnectMs);
    connect(m_reconnect, &QTimer::timeout, this, &DiscordPresence::tryConnect);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &DiscordPresence::pushActivity);

    // Presence follows the player. Position isn't tracked continuously (Discord
    // rate-limits updates): the elapsed counter's start is set on each track/state
    // change and Discord ticks it client-side.
    connect(m_player, &PlayerController::currentTrackChanged,
            this, &DiscordPresence::scheduleUpdate);
    connect(m_player, &PlayerController::playbackStateChanged,
            this, [this](bool) { scheduleUpdate(); });

    tryConnect();
}

DiscordPresence::~DiscordPresence()
{
    if (m_socket && m_socket->state() == QLocalSocket::ConnectedState) {
        m_ready = false;
        pushActivity();   // clears the status (no track / shutting down)
        m_socket->flush();
        m_socket->disconnectFromServer();
    }
}

void DiscordPresence::tryConnect()
{
    if (!m_socket || m_socket->state() != QLocalSocket::UnconnectedState)
        return;
    // A missing socket fails instantly (ENOENT/no pipe), so this loop is cheap when
    // Discord isn't running; the short wait only applies to a live endpoint.
    for (const QString &name : candidateSocketNames()) {
        m_socket->connectToServer(name);
        if (m_socket->waitForConnected(200))
            return;   // onConnected() handles the handshake
        m_socket->abort();
    }
    m_reconnect->start();   // none available — try again later
}

void DiscordPresence::onConnected()
{
    qInfo("[discord] IPC connected; handshaking");
    m_ready = false;
    m_rx.clear();
    QJsonObject hello{{QStringLiteral("v"), 1}, {QStringLiteral("client_id"), m_appId}};
    sendFrame(OpHandshake, hello);
}

void DiscordPresence::onReadyRead()
{
    m_rx += m_socket->readAll();
    while (m_rx.size() >= 8) {
        const quint32 op = readLE32(m_rx, 0);
        const quint32 len = readLE32(m_rx, 4);
        if (m_rx.size() < int(8 + len))
            break;   // wait for the rest of the frame
        const QByteArray payload = m_rx.mid(8, len);
        m_rx.remove(0, 8 + len);

        if (op == OpPing) {
            sendFrame(OpPong, QJsonDocument::fromJson(payload).object());
        } else if (op == OpClose) {
            m_socket->abort();
            onDisconnected();
            return;
        } else if (op == OpFrame) {
            // The handshake ack arrives as a DISPATCH/READY frame; once seen we may
            // push activity.
            const QJsonObject o = QJsonDocument::fromJson(payload).object();
            if (o.value(QStringLiteral("evt")).toString() == QLatin1String("READY")) {
                qInfo("[discord] ready; pushing presence");
                m_ready = true;
                pushActivity();
            }
        }
    }
}

void DiscordPresence::onDisconnected()
{
    if (m_ready)   // only note real drops, not the quiet connect-attempt failures
        qInfo("[discord] disconnected; will retry");
    m_ready = false;
    m_rx.clear();
    if (m_reconnect)
        m_reconnect->start();
}

void DiscordPresence::scheduleUpdate()
{
    if (m_ready && m_debounce)
        m_debounce->start();
}

void DiscordPresence::pushActivity()
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState)
        return;

    QJsonValue activity = QJsonValue::Null;   // no track -> clear status
    if (m_ready && m_player->hasTrack()) {
        const Track t = m_player->currentTrack();
        const bool playing = m_player->isPlaying();

        QJsonObject act;
        const QString details = clampField(t.title);
        if (!details.isEmpty())
            act[QStringLiteral("details")] = details;
        const QString state = clampField(
            t.artist.isEmpty() ? t.album : tr("by %1").arg(t.artist));
        if (!state.isEmpty())
            act[QStringLiteral("state")] = state;

        if (playing) {
            const qint64 startSec =
                QDateTime::currentSecsSinceEpoch() - m_player->position() / 1000;
            act[QStringLiteral("timestamps")] =
                QJsonObject{{QStringLiteral("start"), startSec}};
        }

        QJsonObject assets{
            {QStringLiteral("large_image"), QStringLiteral("logo")},
            {QStringLiteral("small_image"),
             playing ? QStringLiteral("playing") : QStringLiteral("paused")},
            {QStringLiteral("small_text"), playing ? tr("Playing") : tr("Paused")}};
        const QString album = clampField(t.album);
        if (!album.isEmpty())
            assets[QStringLiteral("large_text")] = album;
        act[QStringLiteral("assets")] = assets;

        activity = act;
    }

    const QJsonObject args{{QStringLiteral("pid"),
                            qint64(QCoreApplication::applicationPid())},
                           {QStringLiteral("activity"), activity}};
    sendFrame(OpFrame, QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
        {QStringLiteral("nonce"), QString::number(++m_nonce)},
        {QStringLiteral("args"), args}});
}

void DiscordPresence::sendFrame(int opcode, const QJsonObject &payload)
{
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.reserve(8 + json.size());
    auto put32 = [&frame](quint32 v) {
        frame.append(char(v & 0xff));
        frame.append(char((v >> 8) & 0xff));
        frame.append(char((v >> 16) & 0xff));
        frame.append(char((v >> 24) & 0xff));
    };
    put32(quint32(opcode));
    put32(quint32(json.size()));
    frame.append(json);
    m_socket->write(frame);
}
