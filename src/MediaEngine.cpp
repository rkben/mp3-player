#include "MediaEngine.h"

#include <QAudioOutput>
#include <QAudioDevice>
#include <QMediaDevices>
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#  include <QtAudio>      // QtAudio::convertVolume (the namespace was renamed in 6.7)
#else
#  include <QAudio>
#endif

namespace {
// QAudioOutput::setVolume() takes a *linear* gain, but the ear hears loudness
// logarithmically — feeding the slider straight in bunches the audible range up
// near the top. Map the slider's [0..1] (logarithmic, what a volume control
// should be) to the linear gain the sink expects.
inline float toLinearGain(float level)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    return float(QtAudio::convertVolume(level, QtAudio::LogarithmicVolumeScale,
                                        QtAudio::LinearVolumeScale));
#else
    return float(QAudio::convertVolume(level, QAudio::LogarithmicVolumeScale,
                                       QAudio::LinearVolumeScale));
#endif
}

// Resolve a stored device id to a live QAudioDevice. An empty id, or one whose
// device has gone away (unplugged, profile switch), falls back to the current
// system default — Qt's QMediaDevices is the cross-platform seam here.
inline QAudioDevice resolveOutput(const QByteArray &id)
{
    if (!id.isEmpty()) {
        const auto outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice &dev : outputs)
            if (dev.id() == id)
                return dev;
    }
    return QMediaDevices::defaultAudioOutput();
}
}  // namespace

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
    applyOutputDevice();
    m_audio->setVolume(toLinearGain(m_pendingVolume));

    // Watch for OS audio changes. When following the system default (empty id) and
    // the user switches output — e.g. plugs in Bluetooth and the OS moves the
    // default — re-point the sink so playback follows. audioOutputsChanged fires
    // for add/remove and default changes alike; applyOutputDevice() re-resolves.
    m_devices = new QMediaDevices(this);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, this,
            &MediaEngine::applyOutputDevice);

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

void MediaEngine::setVolume(float level)
{
    // `level` is the perceptual slider value (0..1); keep it for a later init()
    // and push the equivalent linear gain to the sink now if it's up.
    m_pendingVolume = level;
    if (m_audio)
        m_audio->setVolume(toLinearGain(level));
}

void MediaEngine::setAudioDevice(const QByteArray &id)
{
    // Cache for a later init() and switch the live sink now if it's up. Switching
    // mid-playback is seamless — QAudioOutput re-routes without dropping the stream.
    m_pendingDeviceId = id;
    applyOutputDevice();
}

void MediaEngine::applyOutputDevice()
{
    if (!m_audio)
        return;   // not up yet; init() will apply m_pendingDeviceId
    const QAudioDevice target = resolveOutput(m_pendingDeviceId);
    if (m_audio->device() != target)   // avoid a needless glitch when nothing moved
        m_audio->setDevice(target);
}
