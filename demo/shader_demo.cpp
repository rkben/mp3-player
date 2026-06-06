#include <QApplication>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QDebug>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <random>

#include "../src/ShaderArt.h"

class AudioVisualizer : public QObject {
public:
  // Pass the main window pointer so we can update the title dynamically
  explicit AudioVisualizer(ShaderArt *shader, QWidget *mainWindow,
                           QObject *parent = nullptr)
      : QObject(parent), m_shader(shader), m_window(mainWindow) {
    player = new QMediaPlayer(this);
    audioOutput = new QAudioOutput(this);
    bufferOutput = new QAudioBufferOutput(this);

    player->setAudioOutput(audioOutput);
    player->setAudioBufferOutput(bufferOutput);

    // 1. Capture the raw audio buffer and feed it to the shader
    connect(bufferOutput, &QAudioBufferOutput::audioBufferReceived, this,
            [this](const QAudioBuffer &buffer) {
              if (buffer.sampleCount() == 0)
                return;
              double sum = 0;

              auto format = buffer.format().sampleFormat();
              if (format == QAudioFormat::Float) {
                const float *data = buffer.constData<float>();
                for (int i = 0; i < buffer.sampleCount(); ++i)
                  sum += data[i] * data[i];
                m_amplitude = std::sqrt(sum / buffer.sampleCount());
              } else if (format == QAudioFormat::Int16) {
                const qint16 *data = buffer.constData<qint16>();
                for (int i = 0; i < buffer.sampleCount(); ++i)
                  sum += data[i] * data[i];
                m_amplitude = std::sqrt(sum / buffer.sampleCount()) / 32768.0f;
              } else if (format == QAudioFormat::Int32) {
                const qint32 *data = buffer.constData<qint32>();
                for (int i = 0; i < buffer.sampleCount(); ++i) {
                  double val = data[i] / 2147483648.0;
                  sum += val * val;
                }
                m_amplitude = std::sqrt(sum / buffer.sampleCount());
              }

              // Envelope Follower (Sensitivity + Smooth Attack & Decay)
              float sensitivity = 2.2f;
              float target = std::min(1.0f, m_amplitude * sensitivity);
              if (target > m_smoothAmplitude) {
                m_smoothAmplitude = target;
              } else {
                m_smoothAmplitude =
                    (m_smoothAmplitude * 0.872f) + (target * 0.09f);
              }

              m_shader->setAmplitude(m_smoothAmplitude);
            });

    // 2. Queue Engine: Listen for track completion to auto-play the next song
    connect(
        player, &QMediaPlayer::mediaStatusChanged, this,
        [this](QMediaPlayer::MediaStatus status) {
          if (status == QMediaPlayer::EndOfMedia) {
            qDebug() << "[Playlist] Track finished. Auto-playing next track...";
            playNext();
          }
        });
  }

  void loadDirectory(const QString &dirPath) {
    m_playlist.clear();
    m_currentIndex = -1;

    // Walk directory recursively finding matching extensions
    QDirIterator it(dirPath, QStringList() << "*.mp3" << "*.flac", QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      m_playlist.append(it.next());
    }

    if (m_playlist.isEmpty()) {
      qWarning() << "[Playlist] No MP3 or FLAC files found in:" << dirPath;
      if (m_window) {
        m_window->setWindowTitle("Visualizer (No playable audio files found)");
      }
      return;
    }

    qDebug() << "[Playlist] Found" << m_playlist.size()
             << "tracks. Shuffling...";

    // Shuffle tracks using modern standard C++ algorithms
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(m_playlist.begin(), m_playlist.end(), g);

    playNext();
  }

  void playNext() {
    if (m_playlist.isEmpty())
      return;

    m_currentIndex++;

    // Loop back and re-shuffle when the entire playlist has finished
    if (m_currentIndex >= m_playlist.size()) {
      qDebug() << "[Playlist] End of cycle reached. Re-shuffling...";
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(m_playlist.begin(), m_playlist.end(), g);
      m_currentIndex = 0;
    }

    QString trackPath = m_playlist[m_currentIndex];
    QFileInfo info(trackPath);

    qDebug() << "[Playlist] Now Playing [" << (m_currentIndex + 1) << "/"
             << m_playlist.size() << "]:" << info.fileName();

    if (m_window) {
      m_window->setWindowTitle(QString("Playing [%1/%2]: %3")
                                   .arg(m_currentIndex + 1)
                                   .arg(m_playlist.size())
                                   .arg(info.baseName()));
    }

    player->setSource(QUrl::fromLocalFile(trackPath));
    player->play();
  }

private:
  QMediaPlayer *player;
  QAudioOutput *audioOutput;
  QAudioBufferOutput *bufferOutput;
  ShaderArt *m_shader;
  QWidget *m_window;
  float m_amplitude = 0.0f;
  float m_smoothAmplitude = 0.0f;

  QStringList m_playlist;
  int m_currentIndex = -1;
};

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  QWidget window;
  window.setWindowTitle("Audio Shader Visualizer");
  QVBoxLayout *layout = new QVBoxLayout(&window);

  ShaderArt *shaderWidget = new ShaderArt();
  layout->addWidget(shaderWidget, 1);

  // Pass the 'window' pointer directly to the visualizer to avoid standard moc
  // build issues
  AudioVisualizer visualizer(shaderWidget, &window);

  QPushButton *btnPlay = new QPushButton("Select Directory & Shuffle Play");
  layout->addWidget(btnPlay);

  QObject::connect(btnPlay, &QPushButton::clicked, [&visualizer]() {
    QString dir = QFileDialog::getExistingDirectory(
        nullptr, "Select Music Directory", "");
    if (!dir.isEmpty()) {
      visualizer.loadDirectory(dir);
    }
  });

  window.resize(800, 600);
  window.show();

  return app.exec();
}