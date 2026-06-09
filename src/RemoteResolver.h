#pragma once

#include <QObject>
#include <QUrl>

#include "YtDlp.h"

// Resolves a remote (yt-dlp-supported) page URL to a directly-playable stream URL at
// playback time, via `yt-dlp -g`. Signed stream URLs expire, so we never persist them —
// each play resolves fresh. Runs through a YtDlp runner (async, never blocks the GUI); a
// new resolve supersedes any still-running one (rapid remote skips), and the runner's
// one-shot guard keeps a superseded result from firing. Callers also match the result by
// pageUrl. The yt-dlp executable path comes from YtDlp::path().
class RemoteResolver : public QObject
{
    Q_OBJECT
public:
    explicit RemoteResolver(QObject *parent = nullptr);

    // Resolve `pageUrl` to a stream URL. Emits resolved() or failed() exactly once.
    void resolve(const QUrl &pageUrl);

    // The configured (or auto-discovered) yt-dlp path; empty if none is found.
    // Thin wrapper over YtDlp::path() for existing callers.
    static QString ytDlpPath() { return YtDlp::path(); }

signals:
    void resolved(const QUrl &pageUrl, const QUrl &streamUrl);
    void failed(const QUrl &pageUrl, const QString &message);

private:
    YtDlp m_dlp;
    QUrl m_pageUrl;   // page URL of the in-flight resolve (matched in the result handler)
};
