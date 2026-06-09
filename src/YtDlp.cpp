#include "YtDlp.h"
#include "ProcUtil.h"
#include "YtDlpManager.h"

#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>

YtDlp::YtDlp(QObject *parent) : QObject(parent) {}

YtDlp::~YtDlp() { cancel(); }

QString YtDlp::path()
{
    // 1. Explicit override (advanced Settings field) always wins.
    const QString configured = QSettings().value("ytdlp/path").toString();
    if (!configured.isEmpty())
        return configured;

    // 2. The app-managed binary, when enabled and installed. Lives under AppData so a
    //    full reset removes it; the "Use managed" toggle lets the user prefer $PATH.
    if (QSettings().value("ytdlp/useManaged", false).toBool()
        && YtDlpManager::isManagedInstalled())
        return YtDlpManager::managedPath();

    return systemPath();
}

QString YtDlp::systemPath()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    if (!onPath.isEmpty())
        return onPath;

    // PATH lookup misses for GUI launches: apps started via Finder/Dock/`open` inherit
    // launchd's minimal PATH (/usr/bin:/bin:/usr/sbin:/sbin), without the
    // Homebrew/MacPorts/pipx dirs where yt-dlp usually lives. Probe the common install
    // locations so a default install "just works" unbundled. (Windows uses the PATH
    // lookup above, which resolves .exe; the Settings field covers the rest.)
    for (const QString &dir : {QStringLiteral("/opt/homebrew/bin"),   // Homebrew (Apple Silicon)
                               QStringLiteral("/usr/local/bin"),       // Homebrew (Intel) / manual
                               QStringLiteral("/opt/local/bin"),       // MacPorts
                               QDir::homePath() + QStringLiteral("/.local/bin")}) {  // pipx / pip --user
        const QString cand = dir + QStringLiteral("/yt-dlp");
        if (QFileInfo(cand).isExecutable())
            return cand;
    }
    return QString();
}

void YtDlp::run(const QStringList &args, const QByteArray &stdinData)
{
    cancel();   // a newer request supersedes any still-running one

    const QString exe = path();
    if (exe.isEmpty()) {
        emit startFailed(tr("yt-dlp not found — set its path in Settings"));
        return;
    }

    auto *proc = new QProcess(this);
    // finished and errorOccurred can both fire for one failure (e.g. a crash); this
    // one-shot guard ensures we emit + clean up exactly once.
    auto done = std::make_shared<bool>(false);
    m_proc = proc;
    m_done = done;

    connect(proc, &QProcess::finished, this,
            [this, proc, done](int code, QProcess::ExitStatus status) {
                if (*done) return;
                *done = true;
                if (m_proc == proc) m_proc = nullptr;
                const QByteArray out = proc->readAllStandardOutput();
                QByteArray err = proc->readAllStandardError();
                proc->deleteLater();
                if (status != QProcess::NormalExit)
                    err = err.isEmpty() ? QByteArrayLiteral("yt-dlp crashed") : err;
                emit finished(code, out, err);
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, done](QProcess::ProcessError) {
                // finished() may not fire on a start failure; report and clean up.
                if (*done) return;
                *done = true;
                if (m_proc == proc) m_proc = nullptr;
                const QString msg = proc->errorString();
                proc->deleteLater();
                emit startFailed(msg);
            });
    if (!stdinData.isEmpty()) {
        connect(proc, &QProcess::started, this, [proc, stdinData] {
            proc->write(stdinData);
            proc->closeWriteChannel();
        });
    }

    suppressConsoleWindow(proc);
    proc->start(exe, args);
}

void YtDlp::cancel()
{
    if (!m_proc)
        return;
    // Mute the guard so the kill's finished()/errorOccurred() can't emit for a request
    // the caller has already moved past (or destroyed).
    if (m_done)
        *m_done = true;
    m_proc->kill();
    m_proc->deleteLater();
    m_proc = nullptr;
    m_done.reset();
}
