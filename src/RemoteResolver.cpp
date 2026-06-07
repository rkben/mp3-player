#include "RemoteResolver.h"
#include "ProcUtil.h"
#include "YtDlpManager.h"

#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>

#include <memory>

QString RemoteResolver::ytDlpPath()
{
    // 1. Explicit override (advanced Settings field) always wins.
    const QString configured = QSettings().value("ytdlp/path").toString();
    if (!configured.isEmpty())
        return configured;

    // 2. The app-managed binary, when enabled and installed. Lives under AppData so a
    //    full reset removes it; the "Use managed" toggle lets the user prefer $PATH.
    if (QSettings().value("ytdlp/useManaged", true).toBool()
        && YtDlpManager::isManagedInstalled())
        return YtDlpManager::managedPath();

    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    if (!onPath.isEmpty())
        return onPath;

    // PATH lookup misses for GUI launches: apps started via Finder/Dock/`open`
    // inherit launchd's minimal PATH (/usr/bin:/bin:/usr/sbin:/sbin), without the
    // Homebrew/MacPorts/pipx dirs where yt-dlp usually lives. Probe the common
    // install locations so a default install "just works" unbundled. (Windows uses
    // the PATH lookup above, which resolves .exe; the Settings field covers the rest.)
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

void RemoteResolver::resolve(const QUrl &pageUrl)
{
    const QString exe = ytDlpPath();
    if (exe.isEmpty()) {
        emit failed(pageUrl, tr("yt-dlp not found — set its path in Settings"));
        return;
    }

    qInfo().noquote() << QStringLiteral("[resolve] %1").arg(pageUrl.toString());
    cancelCurrent();   // a newer request supersedes any still-running resolve

    auto *proc = new QProcess(this);
    // finished and errorOccurred can both fire for one failure (e.g. a crash);
    // this one-shot guard ensures we emit + clean up exactly once.
    auto done = std::make_shared<bool>(false);
    m_proc = proc;
    m_done = done;
    // Best audio-only stream (falls back to best combined if none), URL only.
    const QStringList args{QStringLiteral("-f"), QStringLiteral("bestaudio/best"),
                           QStringLiteral("-g"), pageUrl.toString()};

    connect(proc, &QProcess::finished, this,
            [this, proc, pageUrl, done](int code, QProcess::ExitStatus status) {
                if (*done) return;
                *done = true;
                if (m_proc == proc) m_proc = nullptr;   // it's no longer in-flight
                proc->deleteLater();
                if (status != QProcess::NormalExit || code != 0) {
                    const QString err = QString::fromUtf8(proc->readAllStandardError())
                                            .trimmed();
                    // SoundCloud Go+ (and other DRM) streams can't be played — yt-dlp
                    // refuses to hand back a URL. Give a plain reason instead of the
                    // raw "[soundcloud] <id>: This video is DRM protected" line.
                    QString msg;
                    if (err.contains(QStringLiteral("DRM"), Qt::CaseInsensitive))
                        msg = tr("This track is DRM-protected and can't be streamed");
                    else
                        msg = err.isEmpty()
                                  ? tr("yt-dlp failed to resolve the stream")
                                  : err;
                    qWarning().noquote()
                        << QStringLiteral("[resolve] failed %1 — %2")
                               .arg(pageUrl.toString(), msg);
                    emit failed(pageUrl, msg);
                    return;
                }
                // `-g` prints one URL per selected stream; take the first non-empty.
                const QByteArray out = proc->readAllStandardOutput();
                for (const QByteArray &line : out.split('\n')) {
                    const QString s = QString::fromUtf8(line).trimmed();
                    if (!s.isEmpty()) {
                        qInfo().noquote()
                            << QStringLiteral("[resolve] ok %1").arg(pageUrl.toString());
                        emit resolved(pageUrl, QUrl(s));
                        return;
                    }
                }
                qWarning().noquote()
                    << QStringLiteral("[resolve] failed %1 — no stream URL")
                           .arg(pageUrl.toString());
                emit failed(pageUrl, tr("yt-dlp returned no stream URL"));
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, pageUrl, done](QProcess::ProcessError) {
                // finished() may not fire on a start failure; report and clean up.
                if (*done) return;
                *done = true;
                if (m_proc == proc) m_proc = nullptr;
                emit failed(pageUrl, proc->errorString());
                proc->deleteLater();
            });

    suppressConsoleWindow(proc);
    proc->start(exe, args);
}

void RemoteResolver::cancelCurrent()
{
    if (!m_proc)
        return;
    // Mute the one-shot guard so the kill's finished()/errorOccurred() can't emit a
    // spurious failed() for a request the caller has already moved past.
    if (m_done)
        *m_done = true;
    m_proc->kill();
    m_proc->deleteLater();
    m_proc = nullptr;
    m_done.reset();
}

RemoteResolver::RemoteResolver(QObject *parent) : QObject(parent) {}
