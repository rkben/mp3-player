#pragma once
#include <QObject>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QAudioBufferOutput>
#include <QAudioBuffer>
#include <cmath>

class AudioVisualizer : public QObject {
    Q_OBJECT
public:
    explicit AudioVisualizer(QObject *parent = nullptr) : QObject(parent) {
        player = new QMediaPlayer(this);
        audioOutput = new QAudioOutput(this);
        bufferOutput = new QAudioBufferOutput(this);

        player->setAudioOutput(audioOutput);
        player->setAudioBufferOutput(bufferOutput); 

        connect(bufferOutput, &QAudioBufferOutput::audioBufferReceived,
                this, &AudioVisualizer::processAudioBuffer);
    }

    float currentAmplitude() const { return m_amplitude; }

public slots:
    void play(const QUrl &url) {
        player->setSource(url);
        player->play();
    }

signals:
    void amplitudeChanged(float newAmplitude);

private:
    void processAudioBuffer(const QAudioBuffer &buffer) {
        if (buffer.sampleCount() == 0) return;

        double sum = 0;
        
        // Qt 6 typically decodes to Float format by default
        if (buffer.format().sampleFormat() == QAudioFormat::Float) {
            const float *data = buffer.constData<float>();
            for (int i = 0; i < buffer.sampleCount(); ++i) {
                sum += data[i] * data[i];
            }
            m_amplitude = static_cast<float>(std::sqrt(sum / buffer.sampleCount()));
        } 
        // Fallback for standard 16-bit PCM
        else if (buffer.format().sampleFormat() == QAudioFormat::Int16) {
            const qint16 *data = buffer.constData<qint16>();
            for (int i = 0; i < buffer.sampleCount(); ++i) {
                sum += data[i] * data[i];
            }
            m_amplitude = static_cast<float>(std::sqrt(sum / buffer.sampleCount()) / 32768.0);
        }

        // Clamp to 1.0 just in case
        m_amplitude = std::min(1.0f, m_amplitude);
        emit amplitudeChanged(m_amplitude);
    }

    QMediaPlayer* player;
    QAudioOutput* audioOutput;
    QAudioBufferOutput* bufferOutput;
    float m_amplitude = 0.0f;
};