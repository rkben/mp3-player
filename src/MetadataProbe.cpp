#include "MetadataProbe.h"

#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QTimer>

namespace {
// Cap per file so a source that never reaches LoadedMedia (corrupt/unsupported,
// a stalled network mount) can't wedge the queue. Generous: local opens are
// fast, and TagLib already handled everything well-formed.
constexpr int kProbeTimeoutMs = 15000;
}  // namespace

MetadataProbe::MetadataProbe(QObject *parent) : QObject(parent) {}

void MetadataProbe::init()
{
    // Built here (not the constructor) so the player gets this worker thread's
    // affinity — QMediaPlayer needs the event loop of the thread it lives on.
    // No audio output: we only want the backend to decode metadata, not play.
    m_player = new QMediaPlayer(this);

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(kProbeTimeoutMs);
    connect(m_timeout, &QTimer::timeout, this, &MetadataProbe::finishCurrent);

    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::LoadedMedia
                    || status == QMediaPlayer::BufferedMedia
                    || status == QMediaPlayer::InvalidMedia)
                    finishCurrent();
            });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error) {
                if (error != QMediaPlayer::NoError)
                    finishCurrent();
            });
}

void MetadataProbe::enqueue(const QStringList &paths)
{
    for (const QString &p : paths)
        m_queue.enqueue(p);
    if (m_current.isEmpty())
        startNext();
}

void MetadataProbe::startNext()
{
    if (m_queue.isEmpty()) {
        m_current.clear();
        m_player->setSource(QUrl());   // release the last file handle
        return;
    }
    m_current = m_queue.dequeue();
    m_emitted = false;
    m_timeout->start();
    m_player->setSource(QUrl::fromLocalFile(m_current));
}

void MetadataProbe::finishCurrent()
{
    if (m_current.isEmpty())
        return;   // a late signal after we already advanced
    m_timeout->stop();

    // Harvest once per file: mediaStatusChanged + a trailing metaDataChanged can
    // both land. Guard so we don't re-emit (and don't recurse into startNext).
    if (!m_emitted) {
        m_emitted = true;
        const QMediaMetaData md = m_player->metaData();
        const QString title = md.stringValue(QMediaMetaData::Title);
        QString artist = md.stringValue(QMediaMetaData::ContributingArtist);
        if (artist.isEmpty())
            artist = md.stringValue(QMediaMetaData::AlbumArtist);
        const QString album = md.stringValue(QMediaMetaData::AlbumTitle);
        const int trackNo = md.value(QMediaMetaData::TrackNumber).toInt();
        const qint64 durationMs = md.value(QMediaMetaData::Duration).toLongLong();

        // Only worth a DB write if FFmpeg found something TagLib couldn't.
        if (!title.isEmpty() || !artist.isEmpty() || !album.isEmpty()
            || durationMs > 0)
            emit resolved(m_current, title, artist, album, trackNo, durationMs);
    }

    m_current.clear();   // mark idle before loading the next source
    startNext();
}
