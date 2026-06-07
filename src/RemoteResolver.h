#pragma once

#include <QObject>
#include <QUrl>

#include <memory>

class QProcess;

// Resolves a remote (yt-dlp-supported) page URL to a directly-playable stream URL
// at playback time, via `yt-dlp -g`. Signed stream URLs expire, so we never
// persist them — each play resolves fresh. Uses an async QProcess, so it never
// blocks the GUI thread. A new resolve cancels any still-running one (rapid remote
// skips would otherwise leave several yt-dlp processes running); callers also match
// the result by pageUrl. The yt-dlp executable path comes from QSettings
// ("ytdlp/path"), falling back to the one found on $PATH.
class RemoteResolver : public QObject
{
    Q_OBJECT
public:
    explicit RemoteResolver(QObject *parent = nullptr);

    // Resolve `pageUrl` to a stream URL. Emits resolved() or failed() exactly once.
    void resolve(const QUrl &pageUrl);

    // The configured (or auto-discovered) yt-dlp path; empty if none is found.
    static QString ytDlpPath();

signals:
    void resolved(const QUrl &pageUrl, const QUrl &streamUrl);
    void failed(const QUrl &pageUrl, const QString &message);

private:
    void cancelCurrent();   // suppress + kill any in-flight resolve

    QProcess *m_proc = nullptr;       // the in-flight resolve, or null
    std::shared_ptr<bool> m_done;     // its one-shot guard, so we can mute it
};
