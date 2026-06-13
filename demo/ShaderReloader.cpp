#include "ShaderReloader.h"

#include "../src/ShaderArt.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLibraryInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

ShaderReloader::ShaderReloader(ShaderArt *target, QString fragPath,
                               QObject *parent)
    : QObject(parent), m_target(target), m_fragPath(std::move(fragPath)),
      m_tmp(std::make_unique<QTemporaryDir>()) {
  // qsb ships in Qt's bin dir; resolve it once.
  m_qsb = QLibraryInfo::path(QLibraryInfo::BinariesPath) + "/qsb";
#ifdef Q_OS_WIN
  m_qsb += ".exe";
#endif

  // Coalesce bursts of save events (editors often write+rename+chmod).
  m_debounce = new QTimer(this);
  m_debounce->setSingleShot(true);
  m_debounce->setInterval(120);
  connect(m_debounce, &QTimer::timeout, this, &ShaderReloader::recompile);

  m_watcher = new QFileSystemWatcher(this);
  connect(m_watcher, &QFileSystemWatcher::fileChanged, this,
          &ShaderReloader::onFileChanged);

  if (QFile::exists(m_fragPath)) {
    m_watcher->addPath(m_fragPath);
    qInfo() << "[Reload] Watching" << m_fragPath;
    recompile();   // show the watched shader immediately, not the baked default
  } else {
    qWarning() << "[Reload] Shader to watch not found:" << m_fragPath
               << "\n         (using ShaderArt's baked fallback)";
  }
}

ShaderReloader::~ShaderReloader() = default;

void ShaderReloader::onFileChanged() {
  rearmWatch();
  m_debounce->start();
}

void ShaderReloader::rearmWatch() {
  // An atomic-rename save replaces the inode and drops the watch. If the file is
  // present, make sure it's watched again; if it vanished mid-rename, retry soon.
  if (!QFile::exists(m_fragPath)) {
    QTimer::singleShot(50, this, &ShaderReloader::rearmWatch);
    return;
  }
  if (!m_watcher->files().contains(m_fragPath))
    m_watcher->addPath(m_fragPath);
}

void ShaderReloader::recompile() {
  if (m_proc && m_proc->state() != QProcess::NotRunning)
    return;   // a compile is already in flight; the watcher will fire again

  const QString out = m_tmp->filePath(QStringLiteral("live.frag.qsb"));
  qInfo() << "[Reload] Recompiling" << QFileInfo(m_fragPath).fileName();

  if (!m_proc) {
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this,
            [this, out](int exitCode, QProcess::ExitStatus) {
              if (exitCode != 0) {
                qWarning() << "[Reload] qsb failed (exit" << exitCode
                           << ") — keeping last good shader:";
                qWarning().noquote() << m_proc->readAllStandardError();
                return;
              }
              QFile f(out);
              if (!f.open(QIODevice::ReadOnly)) {
                qWarning() << "[Reload] cannot read compiled shader:" << out;
                return;
              }
              m_target->setFragmentShader(f.readAll());   // ignores invalid
            });
  }

  m_proc->start(m_qsb, {QStringLiteral("--qt6"), QStringLiteral("-o"), out,
                        m_fragPath});
}
