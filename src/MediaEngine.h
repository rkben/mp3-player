#pragma once

#include <QObject>
#include <QMediaPlayer>
#include <QMediaMetaData>
#include <QUrl>

class QAudioOutput;
class QMediaDevices;

// Owns the QMediaPlayer + QAudioOutput and nothing else. Designed to live on a
// dedicated worker thread so the potentially-blocking source open (FFmpeg's
// avformat_open_input + stream probe — slow over NFS) never stalls the GUI
// thread. All control comes in via queued slots; all state goes back out via
// queued signals. PlayerController (on the GUI thread) owns the queue/history
// and drives this; it never touches the QMediaPlayer directly.
class MediaEngine : public QObject
{
    Q_OBJECT
public:
    explicit MediaEngine(QObject *parent = nullptr);

public slots:
    void init();   // create the player on the worker thread (QThread::started)
    // Set the source and, if autoplay, start it. The open happens here, on the
    // worker thread, so a slow NFS file doesn't freeze the UI.
    void load(const QUrl &url, bool autoplay);
    void play();
    void pause();
    void stop();
    void setPosition(qint64 ms);
    void setVolume(float level);    // 0.0 .. 1.0 perceptual (converted to linear gain)
    // Route output to the QAudioDevice with this id (QAudioDevice::id()); an empty
    // id, or one that no longer exists, means the current system default.
    void setAudioDevice(const QByteArray &id);

signals:
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void mediaStatusChanged(QMediaPlayer::MediaStatus status);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);
    void errorOccurred(QMediaPlayer::Error error, const QString &errorString);
    void metaDataChanged(const QMediaMetaData &metaData);

private:
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QMediaDevices *m_devices = nullptr;   // watches for OS device add/remove/default change
    float m_pendingVolume = 0.8f;   // perceptual level; applied once the output exists
    QByteArray m_pendingDeviceId;   // output device id; applied once the output exists

    void applyOutputDevice();   // (re)point the sink at the configured/default device
};
