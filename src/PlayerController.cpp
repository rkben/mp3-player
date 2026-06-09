#include "PlayerController.h"
#include "MediaEngine.h"
#include "RemoteResolver.h"

#include <QThread>
#include <QRandomGenerator>
#include <QFileInfo>
#include <QSet>
#include <QHash>
#include <QVector>

namespace {
// Quality tier from the file extension: lossless > lossy; -1 = not a local file
// (remote tracks are never grouped/deduped).
int qualityTier(const Track &t)
{
    if (!t.url.isLocalFile())
        return -1;
    static const QSet<QString> lossless = {
        QStringLiteral("flac"), QStringLiteral("wav"),  QStringLiteral("wave"),
        QStringLiteral("aiff"), QStringLiteral("aif"),  QStringLiteral("alac"),
        QStringLiteral("ape"),  QStringLiteral("wv"),   QStringLiteral("tta"),
        QStringLiteral("tak")};
    const QString ext = QFileInfo(t.url.toLocalFile()).suffix().toLower();
    return lossless.contains(ext) ? 2 : 1;
}

// Collapse same artist+title local tracks to the best copy, preserving the first
// occurrence's position. mode: 0=off, 1=naive (format tier), 2=naive + bitrate
// tiebreak. If `focus` points at a dropped track it's remapped to the group winner.
QList<Track> applyPreferHq(const QList<Track> &in, int mode, int *focus = nullptr)
{
    if (mode <= 0 || in.size() < 2)
        return in;
    QList<Track> out;
    out.reserve(in.size());
    QHash<QString, int> slotForKey;        // group key -> position in `out`
    QVector<int> inToOut(in.size(), -1);   // input index -> output index (focus remap)

    for (int i = 0; i < in.size(); ++i) {
        const Track &t = in.at(i);
        const int tier = qualityTier(t);
        // Ungroupable (remote, or no artist+title): pass through in place.
        QString key;
        if (tier > 0 && !(t.artist.trimmed().isEmpty() && t.title.trimmed().isEmpty()))
            key = t.artist.trimmed().toLower() + QChar(0x1f) + t.title.trimmed().toLower();
        if (key.isEmpty()) {
            inToOut[i] = out.size();
            out.append(t);
            continue;
        }
        const auto it = slotForKey.constFind(key);
        if (it == slotForKey.constEnd()) {
            slotForKey.insert(key, out.size());
            inToOut[i] = out.size();
            out.append(t);
        } else {
            const int pos = it.value();
            const Track &cur = out.at(pos);
            const int curTier = qualityTier(cur);
            bool better;
            if (tier != curTier)
                better = tier > curTier;
            else if (mode >= 2)
                better = t.bitrate > cur.bitrate;   // Yes: bitrate breaks a tier tie
            else
                better = false;                     // Naive: keep first
            if (better)
                out[pos] = t;
            inToOut[i] = pos;   // a dropped duplicate maps to its group's slot
        }
    }
    if (focus && *focus >= 0 && *focus < inToOut.size())
        *focus = inToOut[*focus];
    return out;
}
}

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
    , m_engineThread(new QThread(this))
    , m_engine(new MediaEngine)   // no parent: moved to the worker thread
    , m_resolver(new RemoteResolver(this))
{
    // These cross the thread boundary via queued connections, so they must be
    // registered as metatypes (the QMediaPlayer enums and metadata value type).
    qRegisterMetaType<QMediaPlayer::MediaStatus>();
    qRegisterMetaType<QMediaPlayer::PlaybackState>();
    qRegisterMetaType<QMediaPlayer::Error>();
    qRegisterMetaType<QMediaMetaData>();

    m_engine->moveToThread(m_engineThread);
    connect(m_engineThread, &QThread::started, m_engine, &MediaEngine::init);
    connect(m_engineThread, &QThread::finished, m_engine, &QObject::deleteLater);

    // Controller -> engine (queued: runs on the worker thread that owns the player).
    connect(this, &PlayerController::engineLoad, m_engine, &MediaEngine::load);
    connect(this, &PlayerController::enginePlay, m_engine, &MediaEngine::play);
    connect(this, &PlayerController::enginePause, m_engine, &MediaEngine::pause);
    connect(this, &PlayerController::engineStop, m_engine, &MediaEngine::stop);
    connect(this, &PlayerController::engineSetPosition, m_engine, &MediaEngine::setPosition);
    connect(this, &PlayerController::engineSetVolume, m_engine, &MediaEngine::setVolume);
    connect(this, &PlayerController::engineSetAudioDevice, m_engine, &MediaEngine::setAudioDevice);

    // Engine -> controller (queued: marshalled back to the GUI thread). Position
    // and duration are cached for the synchronous getters and re-emitted.
    connect(m_engine, &MediaEngine::positionChanged, this, [this](qint64 ms) {
        m_position = ms;
        emit positionChanged(ms);
    });
    connect(m_engine, &MediaEngine::durationChanged, this, [this](qint64 ms) {
        m_duration = ms;
        emit durationChanged(ms);
    });
    connect(m_engine, &MediaEngine::mediaStatusChanged,
            this, &PlayerController::onMediaStatusChanged);
    connect(m_engine, &MediaEngine::metaDataChanged,
            this, &PlayerController::onMetaDataChanged);
    connect(m_engine, &MediaEngine::errorOccurred,
            this, &PlayerController::onErrorOccurred);
    connect(m_engine, &MediaEngine::playbackStateChanged,
            this, &PlayerController::onPlaybackStateChanged);

    // Remote stream resolution. The result is only loaded if it's still the
    // current track (the user may have skipped while yt-dlp was running).
    connect(m_resolver, &RemoteResolver::resolved, this,
            [this](const QUrl &pageUrl, const QUrl &streamUrl) {
                // Keep the "fetching" hint up through the engine opening/buffering the
                // stream URL; it clears when playback actually starts (or on skip).
                if (hasTrack() && m_current.url == pageUrl)
                    emit engineLoad(streamUrl, /*autoplay=*/true);
            });
    connect(m_resolver, &RemoteResolver::failed, this,
            [this](const QUrl &pageUrl, const QString &message) {
                if (hasTrack() && m_current.url == pageUrl)
                    skipBadTrack(message);
            });

    m_engineThread->start();
}

PlayerController::~PlayerController()
{
    m_engineThread->quit();
    m_engineThread->wait();
}

void PlayerController::onPlaybackStateChanged(QMediaPlayer::PlaybackState s)
{
    m_playbackState = s;
    if (s == QMediaPlayer::PlayingState) {
        m_consecutiveErrors = 0;   // a track played -> chain is healthy
        m_lastErrorUrl = QUrl{};
        emit remoteResolving(false);   // stream is open and playing; drop the fetch hint
    }
    emit playbackStateChanged(s == QMediaPlayer::PlayingState);
}

void PlayerController::playInternal(int qindex)
{
    m_index = qindex;
    m_metaResolvedUrl = QUrl{};  // a new track; resolve its metadata afresh
    m_duration = 0;             // reset cached duration until the new source reports
    m_position = 0;
    m_current = m_queue.at(qindex);   // the source of truth for "what's playing"
    const Track &t = m_current;
    qInfo().noquote() << QStringLiteral("[play] %1%2")
                             .arg(t.displayText(),
                                  t.isRemote() ? QStringLiteral(" (remote)") : QString());
    emit remoteResolving(t.isRemote());   // show the fetch hint for remote; clear for local
    if (t.isRemote())
        m_resolver->resolve(t.url);   // engineLoad fires once the stream resolves
    else
        emit engineLoad(t.url, /*autoplay=*/true);
    emit currentTrackChanged(t);
}

void PlayerController::recordHistory(int qindex)
{
    // Starting a new track drops any "forward" history we'd navigated back past.
    while (m_history.size() > m_historyPos + 1)
        m_history.removeLast();
    if (!m_history.isEmpty() && m_history.last() == qindex)
        return;   // repeat-one / replaying same track: no dup entry
    m_history.append(qindex);
    // Retain a small window so "previous" can walk back through recent plays.
    constexpr int kMaxHistory = 6;   // current + up to 5 prior
    while (m_history.size() > kMaxHistory)
        m_history.removeFirst();
    m_historyPos = m_history.size() - 1;
}

void PlayerController::playQueue(const QList<Track> &tracks, int start)
{
    // Prefer-HQ collapses same-song duplicates; remap the start index to the survivor.
    int focus = start;
    m_queue = applyPreferHq(tracks, m_preferHq, &focus);
    start = focus;
    m_history.clear();
    m_historyPos = -1;
    m_consecutiveErrors = 0;
    m_lastErrorUrl = QUrl{};

    emit queueChanged(m_queue);

    if (start < 0 || start >= m_queue.size()) {
        m_index = -1;
        m_current = Track{};
        emit engineStop();
        emit currentTrackChanged(Track{});
        return;
    }
    playInternal(start);
    recordHistory(start);
}

void PlayerController::enqueue(const QList<Track> &tracks)
{
    if (tracks.isEmpty())
        return;
    m_queue.append(applyPreferHq(tracks, m_preferHq));   // dedup the appended batch
    emit queueChanged(m_queue);
}

void PlayerController::jumpTo(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;
    m_consecutiveErrors = 0;
    m_lastErrorUrl = QUrl{};
    playInternal(index);
    recordHistory(index);
}

void PlayerController::clearQueue()
{
    // Empty the queue entirely. If something is playing it detaches (m_index = -1)
    // and keeps going — m_current still drives audio, now-playing, and MPRIS — so
    // it dangles past the now-empty queue and stops naturally at end-of-track
    // (pickNext finds nothing). The engine is deliberately not stopped here.
    m_queue.clear();
    m_index = -1;
    m_history.clear();
    m_historyPos = -1;
    m_consecutiveErrors = 0;
    m_lastErrorUrl = QUrl{};
    emit queueChanged(m_queue);
}

void PlayerController::setReadyQueue(const QList<Track> &tracks)
{
    // A silent fallback only: play() adopts it when the real queue is empty.
    // Never touches m_queue, so enqueue() builds on an empty queue, not this.
    m_readyQueue = tracks;
}

void PlayerController::play()
{
    if (m_playbackState == QMediaPlayer::PlayingState)
        return;
    if (hasTrack()) {
        emit enginePlay();   // resume a paused track
        return;
    }
    // Cold start: play the queue if the user built one, else fall back to the
    // ready queue (e.g. media-key Play before anything was enqueued).
    if (m_queue.isEmpty()) {
        if (m_readyQueue.isEmpty())
            return;
        m_queue = applyPreferHq(m_readyQueue, m_preferHq);
        emit queueChanged(m_queue);
    }
    playInternal(0);
    recordHistory(0);
}

void PlayerController::pause() { emit enginePause(); }
void PlayerController::stop() { emit engineStop(); }

void PlayerController::togglePlayPause()
{
    if (m_playbackState == QMediaPlayer::PlayingState)
        emit enginePause();
    else
        play();
}

void PlayerController::next()
{
    // A user-initiated skip is a fresh intent, like jumpTo/playQueue: clear the
    // error chain so stepping through history doesn't trip the all-bad guard early.
    m_consecutiveErrors = 0;
    // If we'd walked back into history, step forward through it before picking new.
    if (m_historyPos >= 0 && m_historyPos + 1 < m_history.size()) {
        ++m_historyPos;
        playInternal(m_history.at(m_historyPos));
        return;
    }
    const int n = pickNext(/*userInitiated=*/true);
    if (n >= 0) {
        playInternal(n);
        recordHistory(n);
    }
}

void PlayerController::previous()
{
    if (m_queue.isEmpty())
        return;
    m_consecutiveErrors = 0;   // user-initiated: fresh intent (see next())
    // Restart current track if we're more than a few seconds in (common player
    // behaviour: "previous" first rewinds, then steps back).
    constexpr qint64 kRestartThresholdMs = 3000;
    if (m_position > kRestartThresholdMs) {
        m_position = 0;
        emit engineSetPosition(0);
        return;
    }
    // Walk back through actual play history — this is what makes "previous" work
    // sensibly in shuffle (return to the song you just heard, not a new random).
    if (m_historyPos > 0) {
        --m_historyPos;
        playInternal(m_history.at(m_historyPos));
        return;
    }
    if (m_shuffle)
        return;   // no history yet: nothing sensible to go back to in shuffle
    int prev = m_index - 1;
    if (prev < 0)
        prev = (m_repeat == RepeatMode::All) ? m_queue.size() - 1 : 0;
    playInternal(prev);
    recordHistory(prev);
}

void PlayerController::setPosition(qint64 ms)
{
    m_position = ms;   // optimistic; the engine confirms via positionChanged
    emit engineSetPosition(ms);
}

void PlayerController::setVolume(float linear)
{
    if (qFuzzyCompare(m_volume, linear))
        return;
    m_volume = linear;
    emit engineSetVolume(linear);
    emit volumeChanged(linear);
}

void PlayerController::setAudioDevice(const QByteArray &id)
{
    // Forwarded to the engine on its worker thread; the QAudioOutput lives there.
    emit engineSetAudioDevice(id);
}

void PlayerController::setRepeatMode(RepeatMode mode)
{
    if (m_repeat == mode)
        return;
    m_repeat = mode;
    emit repeatModeChanged(mode);
}

void PlayerController::setShuffle(bool on)
{
    if (m_shuffle == on)
        return;
    m_shuffle = on;
    emit shuffleChanged(on);
}

void PlayerController::setCurrentArt(const QUrl &url, const QString &artUrl)
{
    if (!hasTrack() || m_current.url != url)
        return;
    if (m_current.artUrl == artUrl)
        return;   // unchanged — avoid a redundant currentTrackChanged (re-request loop)
    m_current.artUrl = artUrl;
    if (m_index >= 0 && m_index < m_queue.size())
        m_queue[m_index].artUrl = artUrl;   // keep the queue copy in sync (if still queued)
    emit currentTrackChanged(m_current);    // let MPRIS pick up the art
}

int PlayerController::pickNext(bool userInitiated) const
{
    const int n = m_queue.size();
    if (n == 0)
        return -1;

    if (m_repeat == RepeatMode::One && !userInitiated)
        return m_index;

    if (m_shuffle) {
        if (n == 1)
            return 0;
        int r = m_index;
        while (r == m_index)
            r = QRandomGenerator::global()->bounded(n);
        return r;
    }

    int next = m_index + 1;
    if (next >= n)
        return (m_repeat == RepeatMode::All) ? 0 : -1;
    return next;
}

void PlayerController::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    if (status == QMediaPlayer::EndOfMedia) {
        m_consecutiveErrors = 0;   // reached the end cleanly
        const int n = pickNext(/*userInitiated=*/false);
        if (n >= 0) {
            playInternal(n);
            recordHistory(n);
        }
    } else if (status == QMediaPlayer::InvalidMedia) {
        // Unsupported codec / corrupt file: don't let it stall the queue.
        skipBadTrack(tr("Unsupported or corrupt file"));
    }
}

void PlayerController::onErrorOccurred(QMediaPlayer::Error error, const QString &errorString)
{
    if (error != QMediaPlayer::NoError)
        skipBadTrack(errorString);
}

void PlayerController::skipBadTrack(const QString &message)
{
    // errorOccurred and InvalidMedia both fire for one bad file: handle once.
    // Dedupe by track URL (not index) so a detached track is handled correctly too.
    if (!hasTrack() || m_current.url == m_lastErrorUrl)
        return;
    m_lastErrorUrl = m_current.url;
    emit remoteResolving(false);   // drop the fetch hint (next track will re-show if remote)

    qWarning().noquote() << QStringLiteral("[play] skipping '%1' — %2")
                                .arg(m_current.title, message);
    emit trackError(m_current.title, message);

    // Bail out if everything is failing, so we don't spin through a dead queue.
    // A detached track (empty queue) always stops here — there's nothing to skip to.
    if (++m_consecutiveErrors >= m_queue.size()) {
        emit engineStop();
        return;
    }
    const int n = pickNext(/*userInitiated=*/true);   // always move forward
    if (n >= 0) {
        playInternal(n);
        recordHistory(n);
    }
}

void PlayerController::onMetaDataChanged(const QMediaMetaData &md)
{
    if (!hasTrack())
        return;
    // The player uses the same FFmpeg backend as the scanner, so this fills in
    // metadata for formats TagLib couldn't tag (e.g. .tta). Persisted by the UI.
    const QString title = md.stringValue(QMediaMetaData::Title);
    QString artist = md.stringValue(QMediaMetaData::ContributingArtist);
    if (artist.isEmpty())
        artist = md.stringValue(QMediaMetaData::AlbumArtist);
    const QString album = md.stringValue(QMediaMetaData::AlbumTitle);
    const int trackNo = md.value(QMediaMetaData::TrackNumber).toInt();
    const qint64 durationMs = md.value(QMediaMetaData::Duration).toLongLong();

    if (title.isEmpty() && artist.isEmpty() && album.isEmpty())
        return;   // nothing useful to surface

    // Fill the now-playing track so the display and MPRIS reflect it; keep the
    // queue copy in sync when the track is still queued (it may be detached).
    // (Don't fold duration/trackNo into the Track — they're surfaced via
    // metadataResolved/the DB, not the now-playing labels.)
    Track &t = m_current;
    const bool changed = t.mergeFrom(title, artist, album, /*dur=*/0, /*tno=*/0);
    if (changed) {
        if (m_index >= 0 && m_index < m_queue.size())
            m_queue[m_index] = t;
        emit currentTrackChanged(t);
    }

    // metaDataChanged can fire several times per track; only persist when the
    // track changed or its tags actually moved, so we don't spam the DB writer.
    if (!changed && m_metaResolvedUrl == t.url)
        return;
    m_metaResolvedUrl = t.url;
    emit metadataResolved(t.url, title, artist, album, trackNo, durationMs);
}
