#include "MediaEngine.h"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QAudioDevice>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
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

    if (m_pendingVisualizer)
        setVisualizerActive(true);   // requested before the player existed
}

void MediaEngine::setVisualizerActive(bool on)
{
    if (!m_player) {            // not initialised yet — apply in init()
        m_pendingVisualizer = on;
        return;
    }
    if (on == (m_bufferOutput != nullptr))
        return;                // already in the requested state

    if (on) {
        m_bufferOutput = new QAudioBufferOutput(this);
        connect(m_bufferOutput, &QAudioBufferOutput::audioBufferReceived, this,
                &MediaEngine::onAudioBuffer);
        m_player->setAudioBufferOutput(m_bufferOutput);
    } else {
        m_player->setAudioBufferOutput(nullptr);
        delete m_bufferOutput;
        m_bufferOutput = nullptr;
        emit amplitudeChanged(0.0f);   // let the visualizer settle to rest
        m_analyzer.reset();
        emit spectrumChanged(QList<float>(SpectrumAnalyzer::kNumBands, 0.0f));
    }
}

void MediaEngine::onAudioBuffer(const QAudioBuffer &buffer)
{
    const int n = buffer.sampleCount();
    if (n == 0)
        return;
    const int channels = std::max(1, buffer.format().channelCount());
    const int frames = n / channels;
    if (frames == 0)
        return;

    // Downmix to a mono float scratch buffer once (normalised to [-1..1]), then
    // reuse it for both the RMS amplitude and the FFT spectrum — one walk of the
    // buffer feeds both. The scratch vector is a member so it isn't reallocated
    // on every callback.
    m_mono.resize(frames);
    auto fill = [&](const auto *d, double scale) {
        for (int f = 0; f < frames; ++f) {
            double acc = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                acc += double(d[f * channels + ch]) * scale;
            m_mono[f] = float(acc / channels);
        }
    };
    switch (buffer.format().sampleFormat()) {
    case QAudioFormat::Float: fill(buffer.constData<float>(), 1.0); break;
    case QAudioFormat::Int16: fill(buffer.constData<qint16>(), 1.0 / 32768.0); break;
    case QAudioFormat::Int32: fill(buffer.constData<qint32>(), 1.0 / 2147483648.0); break;
    default:
        return;   // UInt8/unknown — skip rather than misread
    }

    // RMS of the mono frames, normalised to [0..1].
    double sum = 0.0;
    for (float v : m_mono)
        sum += double(v) * v;
    const float rms = float(std::sqrt(sum / frames));

    // Feed the mono frames to the analyzer and emit on its fixed, codec-agnostic
    // cadence (see SpectrumAnalyzer::kHop). Gate the loudness on the same tick so
    // amplitude and spectrum update together at the same rate regardless of the
    // decoder's block size — otherwise small-block codecs (MP3) would update far
    // more often, and look jumpier, than large-block ones (FLAC). ShaderArt does
    // the snap-up/ease-down smoothing per painted frame for both.
    if (m_analyzer.push(m_mono.data(), frames, buffer.format().sampleRate())) {
        emit amplitudeChanged(std::min(1.0f, rms * 2.2f));
        const auto &b = m_analyzer.bands();
        emit spectrumChanged(QList<float>(b.begin(), b.end()));
    }
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
