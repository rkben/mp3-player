#include "MprisController.h"
#include "PlayerController.h"
#include "PlaylistModel.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QWidget>
#include <QCoreApplication>

namespace {
constexpr auto kObjectPath = "/org/mpris/MediaPlayer2";
constexpr auto kPlayerIface = "org.mpris.MediaPlayer2.Player";

PlayerController::RepeatMode loopToRepeat(const QString &loop)
{
    if (loop == QLatin1String("Track"))
        return PlayerController::RepeatMode::One;
    if (loop == QLatin1String("Playlist"))
        return PlayerController::RepeatMode::All;
    return PlayerController::RepeatMode::None;
}

QString repeatToLoop(PlayerController::RepeatMode mode)
{
    switch (mode) {
    case PlayerController::RepeatMode::One:  return QStringLiteral("Track");
    case PlayerController::RepeatMode::All:  return QStringLiteral("Playlist");
    default:                                 return QStringLiteral("None");
    }
}
}

// ---------------------------------------------------------------- Root adaptor

MprisRootAdaptor::MprisRootAdaptor(MprisController *parent)
    : QDBusAbstractAdaptor(parent), m_c(parent) {}

QStringList MprisRootAdaptor::supportedMimeTypes() const
{
    return {QStringLiteral("audio/mpeg"), QStringLiteral("audio/flac"),
            QStringLiteral("audio/ogg"),  QStringLiteral("audio/mp4"),
            QStringLiteral("audio/x-wav")};
}

void MprisRootAdaptor::Raise()
{
    if (QWidget *w = m_c->window()) {
        w->showNormal();
        w->raise();
        w->activateWindow();
    }
}

void MprisRootAdaptor::Quit() { QCoreApplication::quit(); }

// -------------------------------------------------------------- Player adaptor

MprisPlayerAdaptor::MprisPlayerAdaptor(MprisController *parent)
    : QDBusAbstractAdaptor(parent), m_c(parent) {}

QString MprisPlayerAdaptor::playbackStatus() const { return m_c->playbackStatus(); }

QString MprisPlayerAdaptor::loopStatus() const
{
    return repeatToLoop(m_c->player()->repeatMode());
}
void MprisPlayerAdaptor::setLoopStatus(const QString &value)
{
    m_c->player()->setRepeatMode(loopToRepeat(value));
}

bool MprisPlayerAdaptor::shuffle() const { return m_c->player()->shuffle(); }
void MprisPlayerAdaptor::setShuffle(bool on) { m_c->player()->setShuffle(on); }

QVariantMap MprisPlayerAdaptor::metadata() const { return m_c->metadata(); }

double MprisPlayerAdaptor::volume() const { return m_c->player()->volume(); }
void MprisPlayerAdaptor::setVolume(double v)
{
    m_c->player()->setVolume(static_cast<float>(qBound(0.0, v, 1.0)));
}

qlonglong MprisPlayerAdaptor::position() const
{
    return static_cast<qlonglong>(m_c->player()->position()) * 1000;  // ms -> us
}

bool MprisPlayerAdaptor::canGoNext() const { return m_c->canGoNext(); }
bool MprisPlayerAdaptor::canGoPrevious() const { return m_c->canGoNext(); }
bool MprisPlayerAdaptor::canPlay() const { return m_c->player()->queueSize() > 0; }

void MprisPlayerAdaptor::Next()      { m_c->player()->next(); }
void MprisPlayerAdaptor::Previous()  { m_c->player()->previous(); }
void MprisPlayerAdaptor::Pause()     { m_c->player()->pause(); }
void MprisPlayerAdaptor::PlayPause() { m_c->player()->togglePlayPause(); }
void MprisPlayerAdaptor::Stop()      { m_c->player()->stop(); }
void MprisPlayerAdaptor::Play()      { m_c->player()->play(); }

void MprisPlayerAdaptor::Seek(qlonglong offsetUs)
{
    qint64 target = m_c->player()->position() + offsetUs / 1000;
    if (target < 0)
        target = 0;
    m_c->player()->setPosition(target);
    m_c->emitSeeked(static_cast<qlonglong>(target) * 1000);
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath &, qlonglong positionUs)
{
    const qint64 ms = positionUs / 1000;
    m_c->player()->setPosition(ms);
    m_c->emitSeeked(positionUs);
}

void MprisPlayerAdaptor::OpenUri(const QString &) { /* not supported */ }

// ----------------------------------------------------------------- Controller

MprisController::MprisController(PlayerController *player, QWidget *window,
                                 QObject *parent)
    : QObject(parent), m_player(player), m_window(window)
{
    new MprisRootAdaptor(this);
    m_playerAdaptor = new MprisPlayerAdaptor(this);

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(kObjectPath, this, QDBusConnection::ExportAdaptors))
        return;

    // Per the spec: org.mpris.MediaPlayer2.<name>, with a PID-suffixed fallback
    // so a second instance doesn't fail to claim a bus name.
    if (!bus.registerService(QStringLiteral("org.mpris.MediaPlayer2.pocketplayer"))) {
        const QString unique = QStringLiteral("org.mpris.MediaPlayer2.pocketplayer.instance%1")
                                   .arg(QCoreApplication::applicationPid());
        if (!bus.registerService(unique))
            return;
    }
    m_registered = true;

    connect(m_player, &PlayerController::playbackStateChanged,
            this, &MprisController::onPlaybackStateChanged);
    connect(m_player, &PlayerController::currentTrackChanged,
            this, &MprisController::onCurrentTrackChanged);
    connect(m_player, &PlayerController::durationChanged,
            this, &MprisController::onDurationChanged);
    connect(m_player, &PlayerController::volumeChanged,
            this, &MprisController::onVolumeChanged);
    connect(m_player, &PlayerController::shuffleChanged,
            this, &MprisController::onShuffleChanged);
    connect(m_player, &PlayerController::repeatModeChanged,
            this, &MprisController::onRepeatModeChanged);
}

QString MprisController::playbackStatus() const
{
    switch (m_player->playbackState()) {
    case QMediaPlayer::PlayingState: return QStringLiteral("Playing");
    case QMediaPlayer::PausedState:  return QStringLiteral("Paused");
    default:                         return QStringLiteral("Stopped");
    }
}

bool MprisController::canGoNext() const { return m_player->queueSize() > 0; }

QVariantMap MprisController::metadata() const
{
    QVariantMap m;
    if (!m_player->hasTrack()) {
        // A valid trackid is required; use the conventional "no track" path.
        m["mpris:trackid"] = QVariant::fromValue(
            QDBusObjectPath("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
        return m;
    }
    const Track t = m_player->currentTrack();
    m["mpris:trackid"] = QVariant::fromValue(
        QDBusObjectPath(QStringLiteral("/org/pocketplayer/track/%1")
                            .arg(m_player->currentIndex())));
    const qint64 lengthUs = m_player->duration() * 1000;
    if (lengthUs > 0)
        m["mpris:length"] = lengthUs;
    if (!t.title.isEmpty())
        m["xesam:title"] = t.title;
    if (!t.artist.isEmpty())
        m["xesam:artist"] = QStringList{t.artist};
    if (!t.album.isEmpty())
        m["xesam:album"] = t.album;
    if (t.trackNo > 0)
        m["xesam:trackNumber"] = t.trackNo;
    if (!t.artUrl.isEmpty())
        m["mpris:artUrl"] = t.artUrl;
    m["xesam:url"] = t.url.toString();
    return m;
}

void MprisController::refreshMetadata()
{
    pushPlayerProps({{"Metadata", metadata()}});
}

void MprisController::emitSeeked(qlonglong us)
{
    if (m_playerAdaptor)
        emit m_playerAdaptor->Seeked(us);
}

void MprisController::pushPlayerProps(const QVariantMap &changed)
{
    QDBusMessage msg = QDBusMessage::createSignal(
        kObjectPath, "org.freedesktop.DBus.Properties", "PropertiesChanged");
    msg << QString(kPlayerIface) << changed << QStringList();
    QDBusConnection::sessionBus().send(msg);
}

void MprisController::onPlaybackStateChanged()
{
    pushPlayerProps({{"PlaybackStatus", playbackStatus()}});
}

void MprisController::onCurrentTrackChanged()
{
    pushPlayerProps({{"Metadata", metadata()},
                     {"CanGoNext", canGoNext()},
                     {"CanGoPrevious", canGoNext()},
                     {"CanPlay", m_player->queueSize() > 0}});
}

void MprisController::onDurationChanged()
{
    pushPlayerProps({{"Metadata", metadata()}});   // mpris:length just resolved
}

void MprisController::onVolumeChanged()
{
    pushPlayerProps({{"Volume", static_cast<double>(m_player->volume())}});
}

void MprisController::onShuffleChanged()
{
    pushPlayerProps({{"Shuffle", m_player->shuffle()}});
}

void MprisController::onRepeatModeChanged()
{
    pushPlayerProps({{"LoopStatus", repeatToLoop(m_player->repeatMode())}});
}
