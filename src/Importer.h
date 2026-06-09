#pragma once

#include <QObject>
#include <QStringList>
#include <QQueue>
#include <QSet>

#include "PlaylistModel.h"   // Track

class QProcess;
class QNetworkAccessManager;

// Background yt-dlp import, two-phase and resumable. A job is first *enumerated*
// (`yt-dlp --flat-playlist`) into per-entry page URLs, then *hydrated* (one streaming
// `yt-dlp --dump-json` fed those URLs on stdin) into remote Tracks whose covers are
// cached. Each hydrated entry is handed back individually (`trackImported`) so the
// caller can persist it and clear its resume row atomically — an interrupted import
// resumes via resumeImportJob() on the next launch. Jobs run one at a time (FIFO);
// resume jobs are enqueued ahead of user-started ones. Per-track failures surface as
// toasts; a whole-job failure as an OS notification (wired by the caller).
class Importer : public QObject
{
    Q_OBJECT
public:
    explicit Importer(QObject *parent = nullptr);

    bool busy() const { return m_running || !m_queue.isEmpty(); }

    // Import everything at `url`. If createPlaylist, a new playlist named after the
    // source (yt-dlp playlist_title) is made; else if appendPlaylist is non-empty,
    // results are appended to it. Both off = library only. Enqueued (FIFO).
    void start(const QString &url, bool createPlaylist, const QString &appendPlaylist);

public slots:
    // Re-hydrate a job's still-pending entries (driven by MusicLibrary on startup).
    void resumeImportJob(const QString &jobId, const QString &createName,
                         const QString &appendName, const QStringList &entryUrls);

signals:
    void status(const QString &message);        // progress line for the status bar
    void trackFailed(const QString &message);   // single-entry problem (toast)
    void failed(const QString &message);        // nothing imported (OS notification)

    // A fresh source enumerated into per-entry URLs: persist as resume rows.
    void enumerated(const QString &jobId, const QString &createName,
                    const QString &appendName, const QStringList &entryUrls);
    // One hydrated entry ready to persist (upsert + clear its resume row).
    void trackImported(const QString &jobId, const QString &entryUrl, const Track &track);
    // Entries that produced nothing on this pass (bump attempts / eventually drop).
    void importEntriesFailed(const QString &jobId, const QStringList &entryUrls);

private:
    struct Job {
        QString jobId;
        QString sourceUrl;          // set for fresh jobs (needs enumeration)
        bool createPlaylist = false;
        QString createName;         // resolved after enumerate, or carried on resume
        QString appendName;
        QStringList entryUrls;      // hydrate targets (from enumerate or resume)
        bool needEnumerate = false;
        bool fromResume = false;    // suppress the whole-job failed() notification
    };

    void enqueue(Job job);
    void pump();                    // start the next job if idle
    void clearProc();

    void startEnumerate();
    void onEnumerateStdout();
    void onEnumerateFinished();

    void startHydrate();
    void onHydrateStdout();
    void onHydrateFinished();
    void parseHydratedLine(const QByteArray &line);
    void startCover(const QString &entryUrl, Track track, const QString &thumb);
    void emitImported(const QString &entryUrl, const Track &track);
    void finishHydratePassIfReady();

    QProcess *m_proc = nullptr;
    QNetworkAccessManager *m_net = nullptr;
    QByteArray m_partial;           // incomplete trailing stdout line between reads
    QString m_artDir;

    QQueue<Job> m_queue;
    Job m_cur;
    bool m_running = false;

    // Enumerate-pass state.
    QStringList m_enumEntries;
    QString m_enumPlaylistTitle;

    // Hydrate-pass state.
    QSet<QString> m_pending;        // fed entries not yet satisfied (leftover = failed)
    QSet<QString> m_seenUris;       // identities seen this pass (archive.org dedup)
    int m_committed = 0;            // tracks handed back this pass
    int m_pendingCovers = 0;
    bool m_hydrateProcDone = false;
};
