#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QHash>
#include <QAtomicInt>
#include <QThreadPool>

#include <functional>

#include "PlaylistModel.h"   // Track

class QTimer;

// Owns the SQLite track database and the folder scan. Designed to live on a
// dedicated worker thread: every method that touches the DB or filesystem runs
// there, results come back to the UI via queued signals.
//
// Scan strategy (cheap at 45k files):
//   1. Emit the cached library immediately (instant warm start, no file read).
//   2. Walk the folder, TagLib-parse only files whose mtime changed or are new,
//      prune deleted rows, all in batched transactions.
//   3. If anything changed, re-emit the full sorted library once.
class MusicLibrary : public QObject
{
    Q_OBJECT
public:
    // Which column(s) a search targets. All = title OR artist OR album. Year is
    // numeric (queried directly), the rest go through the FTS5 index.
    enum SearchScope { ScopeAll = 0, ScopeTitle, ScopeArtist, ScopeAlbum, ScopeYear };

    explicit MusicLibrary(QObject *parent = nullptr);
    ~MusicLibrary() override;

public slots:
    // Invoke via queued connection so it executes on the worker thread. Walks
    // every configured folder; rows whose files are gone from all of them are
    // pruned.
    void scan(const QStringList &folders);

    // Thread-safe: may be called from any thread to abort an in-flight scan.
    void cancel() { m_cancel.storeRelaxed(1); }

    // Resolve cover art for one file (embedded picture -> cache file, else a
    // folder cover image). Lazy/on-demand so the bulk scan stays cheap. Result
    // is cached in the DB; emits artResolved.
    void resolveArt(const QString &path);

    // Full-text search (FTS5) over title/artist/album. scope is a SearchScope.
    // Runs on the worker thread; emits searchResults.
    void search(const QString &text, int scope);

    // Persist metadata the player resolved at play time (e.g. for formats TagLib
    // couldn't tag). Only overwrites fields that are non-empty; leaves mtime
    // untouched so the next scan won't re-parse and clobber it.
    void enrichMetadata(const QString &path, const QString &title,
                        const QString &artist, const QString &album,
                        int trackNo, qint64 durationMs);

    // Insert/replace remote (streamed) tracks. These carry a full URL identity
    // (uri = Track::key()), no local path, a pre-resolved cover art_url, and are
    // never touched by the folder scan/prune. Emits libraryLoaded when done.
    void importTracks(const QList<Track> &tracks);

    // Remove remote tracks by uri (the Remote tree's "Remove"). Only remote rows are
    // touched; local rows are owned by the folder scan. Emits libraryLoaded when done.
    void removeTracks(const QStringList &uris);

    // --- Resumable imports (imports_resume table) -------------------------------
    // An import is two phases: the Importer enumerates a source into per-entry page
    // URLs, then hydrates each into a remote Track. These slots own the persisted
    // resume state so a track-insert and its resume-row clearing commit atomically,
    // and an interrupted import picks back up on the next launch.

    // Record one import job: a pending resume row per entry. createName/appendName
    // capture the playlist intent so it survives a restart.
    void beginImportJob(const QString &jobId, const QString &createName,
                        const QString &appendName, const QStringList &entryUrls);
    // One hydrated entry: upsert the track and mark its resume row imported, in a
    // single transaction. Finalizes the job once no pending rows remain.
    void commitImportedTrack(const QString &jobId, const QString &entryUrl,
                             const Track &track);
    // Entries that produced nothing on a hydration pass: bump their attempt count;
    // drop (and log) any that hit the retry cap so they can't loop forever.
    void failImportEntries(const QString &jobId, const QStringList &entryUrls);
    // On startup: replay any leftover pending rows (resumeImportJob per job).
    void loadPendingImports();

    // Cancel all imports: clear the resume table so nothing resumes; if removeTracks,
    // also delete the tracks committed by those jobs (rollback). Emits importsCancelled.
    void cancelAllImports(bool removeTracks);

    // Upsert a batch of a Subsonic server's tracks (remote rows; mtime carries the sync
    // `epoch` so a final prune can drop this server's rows not seen this sync). `art_url`
    // holds a deferred "subsonic-cover:…" token resolved lazily on display.
    void syncSubsonic(const QString &serverId, const QList<Track> &tracks,
                      qint64 epoch, bool finalPrune);

signals:
    void libraryLoaded(const QList<Track> &tracks);   // full replace (cache + final)
    void tracksAppended(const QList<Track> &tracks);  // incremental, during cold scan
    void scanProgress(int done, int total, const QString &sourceLabel,
                      const QString &fileName);
    void scanStatus(const QString &message);          // empty string == idle/finished
    void artResolved(const QString &path, const QString &artUrl);
    void searchResults(const QString &query, const QList<Track> &tracks);

    // Resume one interrupted job: the Importer re-hydrates the pending entry URLs.
    void resumeImportJob(const QString &jobId, const QString &createName,
                         const QString &appendName, const QStringList &entryUrls);
    // A job's last entry cleared: the tracks (url-only is enough) plus the playlist
    // intent, so the caller writes/appends the m3u8 and notifies — once per job.
    void importJobFinished(const QString &createName, const QString &appendName,
                           const QList<Track> &tracks);
    void importEntriesDropped(int count);   // entries given up on (toast)
    void importsCancelled(int trackCount, bool removed);   // cancel done (toast)
    void subsonicSynced(const QString &serverId, int trackCount);   // sync done (toast)

private:
    bool ensureDb();
    void createTracksTable();   // uri-keyed schema
    void createImportsTable();  // imports_resume (additive; no schema-version bump)
    void finalizeImportJobIfDone(const QString &jobId);   // emit + clear when drained
    void scheduleImportRefresh();   // debounced libraryLoaded during a busy import
    void createFtsSchema();     // FTS5 index + sync triggers (shared with rebuild)
    void rebuildSchema(int fromVersion);          // drop + recreate, salvaging remotes
    QList<Track> salvageRemoteRows();             // read remote rows before a rebuild
    void insertRemoteRows(const QList<Track> &t); // remote upsert, no libraryLoaded emit
    QList<Track> loadAll(QHash<QString, qint64> *mtimesOut);
    QSqlQuery prepareUpsert();                              // hoisted out of the scan loop
    void flushPendingImports();   // drain imports deferred during a scan
    static void upsert(QSqlQuery &q, const Track &t, qint64 mtime);
    static Track parseTags(const QString &path, qint64 mtime);
    QString extractArt(const QString &path, qint64 mtime);   // -> file:// URL or empty
    static QString buildMatchQuery(const QString &text, int scope);  // FTS5 MATCH expr

    QSqlDatabase m_db;
    QString m_artDir;     // cache dir for extracted embedded covers
    bool m_opened = false;
    bool m_scanning = false;   // re-entrancy guard while pumping events mid-scan
    QList<Track> m_pendingImports;   // importTracks() calls deferred while scanning
    // Resume-table ops that arrived mid-scan (their transactions would nest inside
    // the scan's). Replayed in order once the scan's transaction closes.
    QList<std::function<void()>> m_deferredImports;
    QTimer *m_importRefreshTimer = nullptr;   // coalesces libraryLoaded during import
    QAtomicInt m_cancel{0};
    QThreadPool m_pool;   // dedicated pool for parsing; cap set per scan
};
