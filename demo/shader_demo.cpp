#include <QApplication>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QDebug>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QMediaPlayer>
#include <QPushButton>
#include <QStyle>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <random>

#include "../src/ShaderArt.h"
#include "ShaderReloader.h"

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

  void loadDirectory(const QString &dirPath, bool autoPlay = true) {
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

    if (autoPlay)
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

  // Play a specific file immediately (e.g. from a double-click in the tree).
  // If the file is part of the loaded playlist, sync the index so next/prev
  // continue from there.
  void playFile(const QString &filePath) {
    int idx = m_playlist.indexOf(filePath);
    if (idx >= 0) {
      // playNext() increments first, so point one before the target.
      m_currentIndex = idx - 1;
      playNext();
      return;
    }

    QFileInfo info(filePath);
    qDebug() << "[Playlist] Now Playing (direct):" << info.fileName();
    if (m_window)
      m_window->setWindowTitle(QString("Playing: %1").arg(info.baseName()));

    player->setSource(QUrl::fromLocalFile(filePath));
    player->play();
  }

  void playPrevious() {
    if (m_playlist.isEmpty())
      return;

    // Step back two so the shared increment in playNext() lands on the
    // previous track; wrap to the end of the list when at the start.
    m_currentIndex -= 2;
    if (m_currentIndex < -1)
      m_currentIndex = m_playlist.size() - 2;

    playNext();
  }

  void togglePlayPause() {
    if (m_playlist.isEmpty())
      return;

    if (player->playbackState() == QMediaPlayer::PlayingState)
      player->pause();
    else
      player->play();
  }

  // Exposed so the UI can track playback state (e.g. swap the play/pause icon)
  // without AudioVisualizer needing its own moc-generated signals.
  QMediaPlayer *mediaPlayer() const { return player; }

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

  // --- Window 1: the 3D shader visualizer (its own top-level window) ---
  QWidget shaderWindow;
  shaderWindow.setWindowTitle("Audio Shader Visualizer");
  QVBoxLayout *shaderLayout = new QVBoxLayout(&shaderWindow);
  shaderLayout->setContentsMargins(0, 0, 0, 0);
  ShaderArt *shaderWidget = new ShaderArt();
  shaderLayout->addWidget(shaderWidget, 1);

  // Live-reload the scratch shader: edit demo/live.frag and the visual updates.
  // (Copy any preset .frag over live.frag to try it.) Demo-only — the widget
  // itself has no file-watching.
  new ShaderReloader(shaderWidget,
                     QStringLiteral(SHADER_DIR "/live.frag"), &shaderWindow);

  // --- Window 2: controls — directory tree + transport buttons ---
  QWidget controls;
  controls.setWindowTitle("Pocket Player — Library");
  QVBoxLayout *layout = new QVBoxLayout(&controls);

  // Pass the controls window so the visualizer can update its title; this
  // also avoids giving AudioVisualizer its own moc-generated signals.
  AudioVisualizer visualizer(shaderWidget, &controls);

  QPushButton *btnOpen = new QPushButton("Select Music Directory…");
  layout->addWidget(btnOpen);

  // File tree rooted at the chosen directory, filtered to playable audio.
  QFileSystemModel *fsModel = new QFileSystemModel(&controls);
  fsModel->setNameFilters(QStringList() << "*.mp3" << "*.flac");
  fsModel->setNameFilterDisables(false); // hide non-matching files entirely
  QTreeView *tree = new QTreeView();
  tree->setModel(fsModel);
  // Only the Name column is useful here.
  tree->setColumnHidden(1, true);
  tree->setColumnHidden(2, true);
  tree->setColumnHidden(3, true);
  tree->setHeaderHidden(true);
  layout->addWidget(tree, 1);

  // Transport controls: previous / play-pause / next.
  QHBoxLayout *transport = new QHBoxLayout();
  QStyle *style = controls.style();
  QPushButton *btnPrev = new QPushButton();
  QPushButton *btnPlay = new QPushButton();
  QPushButton *btnNext = new QPushButton();
  btnPrev->setIcon(style->standardIcon(QStyle::SP_MediaSkipBackward));
  btnPlay->setIcon(style->standardIcon(QStyle::SP_MediaPlay));
  btnNext->setIcon(style->standardIcon(QStyle::SP_MediaSkipForward));
  transport->addStretch();
  transport->addWidget(btnPrev);
  transport->addWidget(btnPlay);
  transport->addWidget(btnNext);
  transport->addStretch();
  layout->addLayout(transport);

  QObject::connect(btnOpen, &QPushButton::clicked, [&visualizer, fsModel, tree]() {
    QString dir = QFileDialog::getExistingDirectory(
        nullptr, "Select Music Directory", "");
    if (!dir.isEmpty()) {
      tree->setRootIndex(fsModel->setRootPath(dir));
      // Build the playlist so next/prev work, but don't auto-play — the user
      // picks a track from the tree.
      visualizer.loadDirectory(dir, /*autoPlay=*/false);
    }
  });

  // Double-click a file in the tree to play it immediately.
  QObject::connect(tree, &QTreeView::doubleClicked,
                   [&visualizer, fsModel](const QModelIndex &index) {
                     if (fsModel->isDir(index))
                       return;
                     visualizer.playFile(fsModel->filePath(index));
                   });

  QObject::connect(btnPrev, &QPushButton::clicked,
                   [&visualizer]() { visualizer.playPrevious(); });
  QObject::connect(btnPlay, &QPushButton::clicked,
                   [&visualizer]() { visualizer.togglePlayPause(); });
  QObject::connect(btnNext, &QPushButton::clicked,
                   [&visualizer]() { visualizer.playNext(); });

  // Keep the play/pause icon in sync with actual playback state.
  QObject::connect(visualizer.mediaPlayer(), &QMediaPlayer::playbackStateChanged,
                   [btnPlay, style](QMediaPlayer::PlaybackState state) {
                     btnPlay->setIcon(style->standardIcon(
                         state == QMediaPlayer::PlayingState
                             ? QStyle::SP_MediaPause
                             : QStyle::SP_MediaPlay));
                   });

  shaderWindow.resize(800, 600);
  shaderWindow.show();
  controls.resize(360, 520);
  controls.show();

  return app.exec();
}