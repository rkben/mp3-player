#pragma once

#include <QObject>
#include <QQueue>
#include <QString>
#include <QUrl>

QT_BEGIN_NAMESPACE
class QMediaPlayer;
class QTimer;
QT_END_NAMESPACE

// Metadata-only fallback for files TagLib couldn't open. Drives a headless
// QMediaPlayer (no audio output) over a queue of local paths, harvesting the
// FFmpeg backend's tags/duration without ever starting playback. Designed to
// live on its own worker thread (QMediaPlayer needs that thread's event loop);
// slow/network source opens then never touch the GUI thread.
//
// One source at a time: setSource → wait for LoadedMedia (or error/timeout) →
// emit resolved → advance. Results are routed through the same enrichMetadata
// path as play-time resolution.
class MetadataProbe : public QObject {
    Q_OBJECT
public:
    explicit MetadataProbe(QObject *parent = nullptr);

public slots:
    void init();                              // build the player on this thread
    void enqueue(const QStringList &paths);   // append work; starts if idle

signals:
    void resolved(const QString &path, const QString &title, const QString &artist,
                  const QString &album, int trackNo, qint64 durationMs);

private:
    void startNext();           // pop the queue and load, or go idle
    void finishCurrent();       // harvest + advance (also the skip/timeout path)

    QMediaPlayer *m_player = nullptr;
    QTimer *m_timeout = nullptr;
    QQueue<QString> m_queue;
    QString m_current;          // path being probed (empty == idle)
    bool m_emitted = false;     // metadata harvested for m_current this cycle
};
