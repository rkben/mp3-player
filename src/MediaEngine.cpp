#include "MediaEngine.h"

#include <QAudioOutput>

MediaEngine::MediaEngine(QObject *parent)
    : QObject(parent)
{
}

void MediaEngine::init()
{
    // Created here (not in the constructor) so the player and its audio output
    // get the worker thread's affinity — QMediaPlayer needs the event loop of
    // the thread it lives on.
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_audio->setVolume(m_pendingVolume);

    connect(m_player, &QMediaPlayer::positionChanged, this, &MediaEngine::positionChanged);
    connect(m_player, &QMediaPlayer::durationChanged, this, &MediaEngine::durationChanged);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, &MediaEngine::mediaStatusChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, &MediaEngine::playbackStateChanged);
    connect(m_player, &QMediaPlayer::errorOccurred, this, &MediaEngine::errorOccurred);
    connect(m_player, &QMediaPlayer::metaDataChanged, this, [this] {
        emit metaDataChanged(m_player->metaData());
    });
}

void MediaEngine::load(const QUrl &url, bool autoplay)
{
    if (!m_player)
        return;
    m_player->setSource(url);   // the blocking open happens here, off the GUI thread
    if (autoplay)
        m_player->play();
}

void MediaEngine::play()
{
    if (m_player)
        m_player->play();
}

void MediaEngine::pause()
{
    if (m_player)
        m_player->pause();
}

void MediaEngine::stop()
{
    if (m_player)
        m_player->stop();
}

void MediaEngine::setPosition(qint64 ms)
{
    if (m_player)
        m_player->setPosition(ms);
}

void MediaEngine::setVolume(float linear)
{
    m_pendingVolume = linear;   // remembered for init() if the output isn't up yet
    if (m_audio)
        m_audio->setVolume(linear);
}
