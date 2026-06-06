#pragma once

#include <QObject>
#include <QStringList>

#include "PlaylistModel.h"   // Track

class QProcess;
class QNetworkAccessManager;

// Background yt-dlp import. Runs `yt-dlp --dump-json <url>` (async QProcess, no GUI
// block), maps each JSON entry to a remote Track (page URL kept for stream-on-play),
// downloads each cover into the art cache, then hands the finished set back. Single
// import at a time. Per-track failures are surfaced as toasts; whole-import success
// or failure as OS notifications (wired by the caller).
class Importer : public QObject
{
    Q_OBJECT
public:
    explicit Importer(QObject *parent = nullptr);

    bool busy() const { return m_busy; }

    // Import everything at `url`. If createPlaylist, a new playlist named after the
    // source (yt-dlp playlist_title) is made from the results; else if
    // appendPlaylist is non-empty, results are appended to it. Both off = library only.
    void start(const QString &url, bool createPlaylist, const QString &appendPlaylist);

signals:
    void status(const QString &message);    // progress line for the status bar
    void trackFailed(const QString &message);   // single-entry problem (toast)
    // Whole import done: the resolved tracks plus the create/append playlist names
    // (createName is empty unless a playlist was requested; resolved from the source).
    void finished(const QList<Track> &tracks, const QString &createName,
                  const QString &appendName);
    void failed(const QString &message);     // nothing imported (OS notification)

private:
    void onStdout();
    void onProcessFinished();
    void parseLine(const QByteArray &line);
    void startCoverDownloads();
    void finishCover();        // one cover resolved (ok or not); finalize when 0 left
    void finalize();
    void reset();

    QProcess *m_proc = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QByteArray m_partial;       // incomplete trailing stdout line between reads
    QList<Track> m_tracks;
    QStringList m_thumbs;       // parallel to m_tracks: cover URL per track
    QStringList m_directUrls;   // parallel: per-entry direct media URL (dedup fallback)
    QString m_playlistTitle;    // best-effort source title (status + create name)
    bool m_createPlaylist = false;
    QString m_appendPlaylist;
    QString m_artDir;
    int m_pendingCovers = 0;
    bool m_busy = false;
};
