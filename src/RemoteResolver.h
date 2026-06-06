#pragma once

#include <QObject>
#include <QUrl>

// Resolves a remote (yt-dlp-supported) page URL to a directly-playable stream URL
// at playback time, via `yt-dlp -g`. Signed stream URLs expire, so we never
// persist them — each play resolves fresh. Uses an async QProcess, so it never
// blocks the GUI thread; multiple in-flight resolves are fine (callers match the
// result by pageUrl). The yt-dlp executable path comes from QSettings
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
};
