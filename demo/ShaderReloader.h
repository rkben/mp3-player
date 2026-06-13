#pragma once

#include <QObject>
#include <QString>
#include <memory>

class ShaderArt;
class QFileSystemWatcher;
class QTemporaryDir;
class QTimer;
class QProcess;

// Demo-only live shader reload. Watches a fragment-shader *source* file; on
// change it debounces, recompiles with `qsb` into a temp dir, and swaps the
// result into a ShaderArt via setFragmentShader(). The last good shader stays on
// a compile error (the bad edit just logs). This lives in the demo, not in
// ShaderArt, so the shipping widget carries no file-watching or qsb dependency.
//
// Robust against editors that save via atomic rename (write-temp + rename over):
// such a save replaces the inode, which drops a QFileSystemWatcher path, so the
// watch is re-armed on every change — with a short retry if the file is briefly
// absent mid-rename.
class ShaderReloader : public QObject {
  Q_OBJECT
public:
  // fragPath is the .frag source to watch (e.g. SHADER_DIR "/live.frag").
  ShaderReloader(ShaderArt *target, QString fragPath, QObject *parent = nullptr);
  ~ShaderReloader() override;

private:
  void onFileChanged();
  void rearmWatch();   // re-add the path after an atomic-rename save
  void recompile();

  ShaderArt *m_target;
  QString m_fragPath;
  QString m_qsb;       // resolved path to the qsb compiler
  QFileSystemWatcher *m_watcher = nullptr;
  QTimer *m_debounce = nullptr;
  QProcess *m_proc = nullptr;
  std::unique_ptr<QTemporaryDir> m_tmp;
};
