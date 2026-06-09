#include "MusicLibrary.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QDebug>
#include <QtConcurrent>
#include <QFuture>
#include <QThread>
#include <QSettings>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QTimer>

#include <QCryptographicHash>
#include <QDate>
#include <QRegularExpression>
#include <QUrl>

#include "AudioFormats.h"

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <taglib/tvariant.h>
#include <taglib/tpropertymap.h>

namespace {
// Bump on any `tracks` schema change. A DB whose PRAGMA user_version differs is
// rebuilt on open (remote rows salvaged, local rows re-derived by the folder scan).
constexpr int kSchemaVersion = 1;
constexpr int kMaxImportAttempts = 3;   // resume tries before an entry is given up on
constexpr int kCommitEvery = 1000;   // rows per transaction batch
constexpr int kAppendBatch = 200;    // tracks per progressive UI append
constexpr int kPumpEvery = 250;      // event-pump cadence (keep coarse: cheap GUI breathing room)

// Status-line progress emit cadence (files per scanProgress signal). 1 = every
// file. Override at runtime with PP_SCAN_PROGRESS_EVERY for testing.
int progressEvery()
{
    static const int n = []() {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue("PP_SCAN_PROGRESS_EVERY", &ok);
        return (ok && v > 0) ? v : 1;
    }();
    return n;
}

// A file that needs (re)parsing, plus the freshly parsed result.
struct ScanItem { QString path; qint64 mtime; QString sourceLabel; };
struct ParsedRow { Track track; qint64 mtime; };
}

MusicLibrary::MusicLibrary(QObject *parent)
    : QObject(parent)
{
    // Coalesces the per-track libraryLoaded refresh during a busy import: reloading
    // and re-sorting the whole library on every committed entry would be wasteful on
    // a large playlist. Lives on the worker thread (moves with this object).
    m_importRefreshTimer = new QTimer(this);
    m_importRefreshTimer->setSingleShot(true);
    m_importRefreshTimer->setInterval(300);
    connect(m_importRefreshTimer, &QTimer::timeout, this,
            [this] { if (m_opened) emit libraryLoaded(loadAll(nullptr)); });
}

MusicLibrary::~MusicLibrary()
{
    // Close and drop our connection so Qt doesn't warn about a still-open
    // connection at teardown. Runs on the worker thread (the one that opened it).
    if (m_opened) {
        const QString name = m_db.connectionName();
        m_db.close();
        m_db = QSqlDatabase();   // release our handle before removing the connection
        QSqlDatabase::removeDatabase(name);
    }
}

bool MusicLibrary::ensureDb()
{
    if (m_opened)
        return true;

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString dbPath = dir + "/library.db";
    m_artDir = dir + "/art";

    // Unique connection name: QSqlDatabase connections are bound to the thread
    // that opens them, which is this worker thread.
    m_db = QSqlDatabase::addDatabase("QSQLITE", "library");
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        qWarning() << "Failed to open library DB:" << m_db.lastError().text();
        return false;
    }

    QSqlQuery q(m_db);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA synchronous=NORMAL");

    // Schema versioning: a DB whose version differs from kSchemaVersion is rebuilt
    // (remote rows salvaged). Existing DBs predate this and read 0, so they rebuild
    // once on upgrade. Identity is the `uri` so a track may be local or remote.
    QSqlQuery pv(m_db);
    const int version = pv.exec("PRAGMA user_version") && pv.next()
                            ? pv.value(0).toInt() : 0;
    if (version != kSchemaVersion) {
        rebuildSchema(version);
    } else {
        createTracksTable();
        createFtsSchema();
    }
    createImportsTable();   // additive; survives a schema rebuild untouched

    m_opened = true;
    return true;
}

void MusicLibrary::createImportsTable()
{
    // In-flight import state so an interrupted import resumes on the next launch.
    // One row per enumerated entry; status flips 0->1 atomically with the track's
    // insert (see commitImportedTrack). Independent of the `tracks` schema version,
    // so it's created unconditionally and not dropped by rebuildSchema().
    // entry_url is the enumerated (flat-playlist) URL we hand back to yt-dlp; one
    // entry can hydrate into several tracks (a sub-playlist), so track_uri records the
    // actual committed track identity (which may differ from entry_url — e.g. the
    // archive.org direct-URL fallback). status flips 0->1 when an entry is satisfied.
    QSqlQuery q(m_db);
    q.exec(R"(CREATE TABLE IF NOT EXISTS imports_resume(
                job_id    TEXT NOT NULL,
                entry_url TEXT NOT NULL,
                track_uri TEXT,
                seq       INTEGER,
                create_pl TEXT,
                append_pl TEXT,
                status    INTEGER NOT NULL DEFAULT 0,
                attempts  INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY(job_id, entry_url)))");
}

void MusicLibrary::createFtsSchema()
{
    // Full-text index over title/artist/album, external-content (no data dup), kept
    // in sync with `tracks` via triggers. Back-filled once when first created.
    QSqlQuery q(m_db);
    QSqlQuery ftsCheck(m_db);
    ftsCheck.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='tracks_fts'");
    const bool ftsExists = ftsCheck.next();

    q.exec("CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
           "title, artist, album, content='tracks', content_rowid='rowid')");
    q.exec("CREATE TRIGGER IF NOT EXISTS tracks_ai AFTER INSERT ON tracks BEGIN "
           "INSERT INTO tracks_fts(rowid, title, artist, album) "
           "VALUES(new.rowid, new.title, new.artist, new.album); END");
    q.exec("CREATE TRIGGER IF NOT EXISTS tracks_ad AFTER DELETE ON tracks BEGIN "
           "INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album) "
           "VALUES('delete', old.rowid, old.title, old.artist, old.album); END");
    q.exec("CREATE TRIGGER IF NOT EXISTS tracks_au AFTER UPDATE ON tracks BEGIN "
           "INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album) "
           "VALUES('delete', old.rowid, old.title, old.artist, old.album); "
           "INSERT INTO tracks_fts(rowid, title, artist, album) "
           "VALUES(new.rowid, new.title, new.artist, new.album); END");
    if (!ftsExists)   // back-fill from any pre-existing rows
        q.exec("INSERT INTO tracks_fts(tracks_fts) VALUES('rebuild')");
}

void MusicLibrary::rebuildSchema(int fromVersion)
{
    // Local rows are re-derivable from the folder scan; remote rows are not, so save
    // them across the drop and re-insert afterwards.
    const QList<Track> remotes = salvageRemoteRows();

    QSqlQuery q(m_db);
    q.exec("DROP TRIGGER IF EXISTS tracks_ai");
    q.exec("DROP TRIGGER IF EXISTS tracks_ad");
    q.exec("DROP TRIGGER IF EXISTS tracks_au");
    q.exec("DROP TABLE IF EXISTS tracks_fts");
    q.exec("DROP TABLE IF EXISTS tracks");

    createTracksTable();
    createFtsSchema();   // triggers exist before the reinsert, so FTS stays populated
    if (!remotes.isEmpty())
        insertRemoteRows(remotes);

    q.exec(QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion));
    qInfo("[db] schema v%d -> v%d; salvaged %d remote row(s)",
          fromVersion, kSchemaVersion, int(remotes.size()));
}

QList<Track> MusicLibrary::salvageRemoteRows()
{
    QList<Track> out;
    QSqlQuery check(m_db);
    check.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='tracks'");
    if (!check.next())
        return out;   // fresh DB: nothing to salvage

    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    // SELECT * + read by column name so any prior schema works (missing columns
    // simply default).
    if (!q.exec("SELECT * FROM tracks WHERE remote=1")) {
        qWarning() << "[db] salvage failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        const QSqlRecord r = q.record();
        auto val = [&r](const char *name) -> QVariant {
            const int i = r.indexOf(QLatin1String(name));
            return i >= 0 ? r.value(i) : QVariant();
        };
        Track t;
        t.url = QUrl(val("uri").toString());
        t.artist = val("artist").toString();
        t.title = val("title").toString();
        t.album = val("album").toString();
        t.durationMs = val("duration").toLongLong();
        t.trackNo = val("track_no").toInt();
        t.year = val("year").toInt();
        t.artUrl = val("art_url").toString();
        out.append(t);
    }
    return out;
}

void MusicLibrary::createTracksTable()
{
    // Identity is `uri` (url string). `path` is the local filesystem path for
    // local rows and NULL for remote ones; `remote` flags which is which so the
    // folder scan/prune and art extraction only ever touch local rows.
    QSqlQuery q(m_db);
    q.exec(R"(CREATE TABLE IF NOT EXISTS tracks(
                uri      TEXT PRIMARY KEY,
                path     TEXT,
                remote   INTEGER NOT NULL DEFAULT 0,
                mtime    INTEGER NOT NULL DEFAULT 0,
                artist   TEXT,
                title    TEXT,
                album    TEXT,
                duration INTEGER DEFAULT 0,
                track_no INTEGER DEFAULT 0,
                year     INTEGER DEFAULT 0,
                bitrate  INTEGER DEFAULT 0,
                art_url  TEXT))");
}

QList<Track> MusicLibrary::loadAll(QHash<QString, qint64> *mtimesOut)
{
    QList<Track> tracks;
    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    q.exec("SELECT uri, path, remote, mtime, artist, title, album, duration, "
           "track_no, art_url, year, bitrate FROM tracks "
           "ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE, "
           "track_no, title COLLATE NOCASE");
    while (q.next()) {
        const QString uri = q.value(0).toString();
        const QString path = q.value(1).toString();
        const bool remote = q.value(2).toInt() != 0;
        Track t;
        t.url = QUrl(uri);
        t.artist = q.value(4).toString();
        t.title = q.value(5).toString();
        t.album = q.value(6).toString();
        t.durationMs = q.value(7).toLongLong();
        t.trackNo = q.value(8).toInt();
        t.artUrl = q.value(9).toString();
        t.year = q.value(10).toInt();
        t.bitrate = q.value(11).toInt();
        if (t.title.isEmpty() && !remote)
            t.title = QFileInfo(path).completeBaseName();
        tracks.append(t);
        // Only local rows feed the scan's mtime reconciliation; remote rows are
        // never walked or pruned (no filesystem path).
        if (mtimesOut && !remote)
            mtimesOut->insert(path, q.value(3).toLongLong());
    }
    return tracks;
}

QSqlQuery MusicLibrary::prepareUpsert()
{
    // Prepared once per scan and re-bound per row (the cold-scan loop runs this up
    // to 45k times, so re-parsing the SQL each call is a real cost). On an mtime
    // change we null art_url so re-tagged files re-extract their (possibly new)
    // cover; an unchanged mtime keeps the cached art.
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO tracks(uri, path, remote, mtime, artist, title, album, "
              "duration, track_no, year, bitrate) VALUES(?,?,0,?,?,?,?,?,?,?,?) "
              "ON CONFLICT(uri) DO UPDATE SET "
              "path=excluded.path, mtime=excluded.mtime, artist=excluded.artist, "
              "title=excluded.title, album=excluded.album, "
              "duration=excluded.duration, track_no=excluded.track_no, "
              "year=excluded.year, bitrate=excluded.bitrate, "
              "art_url=CASE WHEN tracks.mtime<>excluded.mtime "
              "THEN NULL ELSE tracks.art_url END");
    return q;
}

void MusicLibrary::upsert(QSqlQuery &q, const Track &t, qint64 mtime)
{
    q.bindValue(0, t.url.toString(QUrl::FullyEncoded));   // uri = Track::key()
    q.bindValue(1, t.url.toLocalFile());                  // local filesystem path
    q.bindValue(2, mtime);
    q.bindValue(3, t.artist);
    q.bindValue(4, t.title);
    q.bindValue(5, t.album);
    q.bindValue(6, t.durationMs);
    q.bindValue(7, t.trackNo);
    q.bindValue(8, t.year);
    q.bindValue(9, t.bitrate);
    if (!q.exec())
        qWarning() << "upsert failed:" << q.lastError().text();
}

Track MusicLibrary::parseTags(const QString &path, qint64 /*mtime*/)
{
    Track t;
    t.url = QUrl::fromLocalFile(path);

    TagLib::FileRef f(QFile::encodeName(path).constData(), /*readAudioProperties=*/true);
    if (!f.isNull()) {
        if (TagLib::Tag *tag = f.tag()) {
            t.title = QString::fromStdString(tag->title().to8Bit(true));
            t.artist = QString::fromStdString(tag->artist().to8Bit(true));
            t.album = QString::fromStdString(tag->album().to8Bit(true));
            t.trackNo = static_cast<int>(tag->track());
            t.year = static_cast<int>(tag->year());   // TagLib's best guess
        }
        if (TagLib::AudioProperties *props = f.audioProperties()) {
            t.durationMs = props->lengthInMilliseconds();
            t.bitrate = props->bitrate();   // kbps; drives Prefer-HQ tiebreaks
        }

        // TagLib's year() is 0 for many formats. Fall back to the first 4-digit
        // run in the raw date properties (handles "2011-03-18", "2011/03", …).
        if (t.year == 0) {
            const TagLib::PropertyMap props = f.properties();
            for (const char *key : {"DATE", "ORIGINALDATE", "YEAR", "ORIGINALYEAR"}) {
                if (!props.contains(key) || props[key].isEmpty())
                    continue;
                const QString s = QString::fromStdString(props[key].front().to8Bit(true));
                const auto m = QRegularExpression(QStringLiteral("\\d{4}")).match(s);
                if (m.hasMatch()) {
                    const int y = m.captured().toInt();
                    // Bound to plausible recorded-audio years so a separatorless
                    // MMDD value (e.g. "1231") isn't mistaken for a year.
                    if (y >= 1860 && y <= QDate::currentDate().year() + 1) {
                        t.year = y;
                        break;
                    }
                }
            }
        }
    }
    if (t.title.isEmpty())
        t.title = QFileInfo(path).completeBaseName();
    return t;
}

void MusicLibrary::enrichMetadata(const QString &path, const QString &title,
                                  const QString &artist, const QString &album,
                                  int trackNo, qint64 durationMs)
{
    if (!ensureDb())
        return;

    // Only fill fields that have content; keep existing values otherwise. mtime
    // is deliberately untouched so the next scan treats the row as up to date.
    QSqlQuery q(m_db);
    q.prepare("UPDATE tracks SET "
              "title    = COALESCE(NULLIF(?,''), title), "
              "artist   = COALESCE(NULLIF(?,''), artist), "
              "album    = COALESCE(NULLIF(?,''), album), "
              "track_no = CASE WHEN ? > 0 THEN ? ELSE track_no END, "
              "duration = CASE WHEN ? > 0 THEN ? ELSE duration END "
              "WHERE path = ?");
    q.addBindValue(title);
    q.addBindValue(artist);
    q.addBindValue(album);
    q.addBindValue(trackNo);
    q.addBindValue(trackNo);
    q.addBindValue(durationMs);
    q.addBindValue(durationMs);
    q.addBindValue(path);
    if (!q.exec())
        qWarning() << "enrichMetadata failed:" << q.lastError().text();
}

void MusicLibrary::importTracks(const QList<Track> &tracks)
{
    if (tracks.isEmpty())
        return;

    // A cold scan pumps the worker event loop mid-transaction (see scan()), so this
    // slot can land while the scan's batch transaction is open. SQLite has no nested
    // transactions — committing here would close the scan's batch early. Defer the
    // import; flushPendingImports() drains it once the scan finishes.
    if (m_scanning) {
        m_pendingImports += tracks;
        return;
    }

    if (!ensureDb())
        return;

    insertRemoteRows(tracks);
    qInfo("[import] stored %lld remote track(s) in the library",
          static_cast<long long>(tracks.size()));
    emit libraryLoaded(loadAll(nullptr));   // refresh the UI with the new rows
}

void MusicLibrary::insertRemoteRows(const QList<Track> &tracks)
{
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO tracks(uri, path, remote, mtime, artist, title, album, "
              "duration, track_no, year, art_url) VALUES(?,NULL,1,0,?,?,?,?,?,?,?) "
              "ON CONFLICT(uri) DO UPDATE SET "
              "artist=excluded.artist, title=excluded.title, album=excluded.album, "
              "duration=excluded.duration, track_no=excluded.track_no, "
              "year=excluded.year, art_url=excluded.art_url");
    for (const Track &t : tracks) {
        q.addBindValue(t.url.toString(QUrl::FullyEncoded));   // uri = Track::key()
        // Store NULL (not "") for a missing artist, e.g. a YouTube video with no
        // artist tag, so it reads as genuinely unknown rather than blank.
        q.addBindValue(t.artist.isEmpty() ? QVariant() : QVariant(t.artist));
        q.addBindValue(t.title);
        q.addBindValue(t.album);
        q.addBindValue(t.durationMs);
        q.addBindValue(t.trackNo);
        q.addBindValue(t.year);
        q.addBindValue(t.artUrl);
        if (!q.exec())
            qWarning() << "insert remote row failed:" << q.lastError().text();
    }
    m_db.commit();
}

void MusicLibrary::flushPendingImports()
{
    // m_scanning is already false here, so the replayed ops run their own
    // transactions normally rather than re-deferring. Move-out guards re-entrancy.
    if (!m_pendingImports.isEmpty()) {
        const QList<Track> pending = std::move(m_pendingImports);
        m_pendingImports.clear();
        importTracks(pending);
    }
    if (!m_deferredImports.isEmpty()) {
        const QList<std::function<void()>> ops = std::move(m_deferredImports);
        m_deferredImports.clear();
        for (const auto &op : ops)
            op();
    }
}

void MusicLibrary::scheduleImportRefresh()
{
    // Called on the worker thread; the timer lives here too.
    if (m_importRefreshTimer && !m_importRefreshTimer->isActive())
        m_importRefreshTimer->start();
}

void MusicLibrary::beginImportJob(const QString &jobId, const QString &createName,
                                  const QString &appendName, const QStringList &entryUrls)
{
    if (entryUrls.isEmpty() || !ensureDb())
        return;
    if (m_scanning) {
        m_deferredImports.append(
            [=] { beginImportJob(jobId, createName, appendName, entryUrls); });
        return;
    }
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare("INSERT OR IGNORE INTO imports_resume"
              "(job_id, entry_url, seq, create_pl, append_pl) VALUES(?,?,?,?,?)");
    for (int i = 0; i < entryUrls.size(); ++i) {
        q.addBindValue(jobId);
        q.addBindValue(entryUrls.at(i));
        q.addBindValue(i);
        q.addBindValue(createName);
        q.addBindValue(appendName);
        if (!q.exec())
            qWarning() << "[import] begin job row failed:" << q.lastError().text();
    }
    m_db.commit();
    qInfo("[import] job %s — %lld entry(ies) queued",
          qPrintable(jobId), static_cast<long long>(entryUrls.size()));
}

void MusicLibrary::commitImportedTrack(const QString &jobId, const QString &entryUrl,
                                       const Track &track)
{
    if (!ensureDb())
        return;
    if (m_scanning) {
        m_deferredImports.append(
            [=] { commitImportedTrack(jobId, entryUrl, track); });
        return;
    }
    // Upsert the track and clear its resume row in one transaction: a crash can never
    // leave a track inserted-but-still-pending or a row cleared-but-not-inserted.
    // (Inlined rather than insertRemoteRows() so it shares this one transaction —
    // that helper manages its own, and SQLite has no nested transactions.)
    m_db.transaction();
    QSqlQuery ins(m_db);
    ins.prepare("INSERT INTO tracks(uri, path, remote, mtime, artist, title, album, "
                "duration, track_no, year, art_url) VALUES(?,NULL,1,0,?,?,?,?,?,?,?) "
                "ON CONFLICT(uri) DO UPDATE SET "
                "artist=excluded.artist, title=excluded.title, album=excluded.album, "
                "duration=excluded.duration, track_no=excluded.track_no, "
                "year=excluded.year, art_url=excluded.art_url");
    ins.addBindValue(track.url.toString(QUrl::FullyEncoded));   // uri = Track::key()
    ins.addBindValue(track.artist.isEmpty() ? QVariant() : QVariant(track.artist));
    ins.addBindValue(track.title);
    ins.addBindValue(track.album);
    ins.addBindValue(track.durationMs);
    ins.addBindValue(track.trackNo);
    ins.addBindValue(track.year);
    ins.addBindValue(track.artUrl);
    if (!ins.exec())
        qWarning() << "[import] commit track failed:" << ins.lastError().text();

    const QString trackUri = track.url.toString(QUrl::FullyEncoded);
    // Claim the entry's pending row (record which track satisfied it). If the entry was
    // already satisfied — a sub-playlist entry that hydrated into several tracks — log
    // this extra track on its own done row so the m3u8 lists every track, not just one.
    QSqlQuery up(m_db);
    up.prepare("UPDATE imports_resume SET status=1, track_uri=? "
               "WHERE job_id=? AND entry_url=? AND status=0");
    up.addBindValue(trackUri);
    up.addBindValue(jobId);
    up.addBindValue(entryUrl);
    if (!up.exec())
        qWarning() << "[import] mark imported failed:" << up.lastError().text();
    if (up.numRowsAffected() <= 0) {
        QSqlQuery extra(m_db);
        extra.prepare("INSERT OR REPLACE INTO imports_resume"
                      "(job_id, entry_url, track_uri, status) VALUES(?,?,?,1)");
        extra.addBindValue(jobId);
        extra.addBindValue(trackUri);   // keyed by the track itself (its own row)
        extra.addBindValue(trackUri);
        extra.exec();
    }
    m_db.commit();

    scheduleImportRefresh();
    finalizeImportJobIfDone(jobId);
}

void MusicLibrary::failImportEntries(const QString &jobId, const QStringList &entryUrls)
{
    if (entryUrls.isEmpty() || !ensureDb())
        return;
    if (m_scanning) {
        m_deferredImports.append([=] { failImportEntries(jobId, entryUrls); });
        return;
    }
    int dropped = 0;
    m_db.transaction();
    for (const QString &url : entryUrls) {
        QSqlQuery up(m_db);
        up.prepare("UPDATE imports_resume SET attempts=attempts+1 "
                   "WHERE job_id=? AND entry_url=? AND status=0");
        up.addBindValue(jobId);
        up.addBindValue(url);
        up.exec();

        QSqlQuery sel(m_db);
        sel.prepare("SELECT attempts FROM imports_resume "
                    "WHERE job_id=? AND entry_url=? AND status=0");
        sel.addBindValue(jobId);
        sel.addBindValue(url);
        if (sel.exec() && sel.next() && sel.value(0).toInt() >= kMaxImportAttempts) {
            QSqlQuery del(m_db);
            del.prepare("DELETE FROM imports_resume WHERE job_id=? AND entry_url=?");
            del.addBindValue(jobId);
            del.addBindValue(url);
            del.exec();
            ++dropped;
            qWarning().noquote()
                << QStringLiteral("[import] giving up on entry after %1 attempts: %2")
                       .arg(kMaxImportAttempts).arg(url);
        }
    }
    m_db.commit();
    if (dropped > 0)
        emit importEntriesDropped(dropped);
    finalizeImportJobIfDone(jobId);   // dropping the last laggard may complete the job
}

void MusicLibrary::finalizeImportJobIfDone(const QString &jobId)
{
    QSqlQuery pend(m_db);
    pend.prepare("SELECT COUNT(*) FROM imports_resume WHERE job_id=? AND status=0");
    pend.addBindValue(jobId);
    if (!pend.exec() || !pend.next() || pend.value(0).toInt() > 0)
        return;   // still entries to hydrate (or the query failed): not done

    // Playlist intent lives on the enumerated rows (the extra sub-playlist rows carry
    // NULL); pull it from one of them.
    QString createName, appendName;
    QSqlQuery pl(m_db);
    pl.prepare("SELECT create_pl, append_pl FROM imports_resume "
               "WHERE job_id=? AND create_pl IS NOT NULL LIMIT 1");
    pl.addBindValue(jobId);
    if (pl.exec() && pl.next()) {
        createName = pl.value(0).toString();
        appendName = pl.value(1).toString();
    }

    // Job drained: gather the committed tracks (enumeration order) for the m3u8/notify,
    // then drop all its rows. track_uri is the real identity (an entry may have yielded
    // several, or differ from entry_url via the dedup fallback); url-only rows suffice.
    QSqlQuery q(m_db);
    q.prepare("SELECT track_uri FROM imports_resume "
              "WHERE job_id=? AND track_uri IS NOT NULL ORDER BY seq");
    q.addBindValue(jobId);
    if (!q.exec())
        return;
    QList<Track> tracks;
    while (q.next()) {
        Track t;
        t.url = QUrl(q.value(0).toString());
        tracks.append(t);
    }
    if (tracks.isEmpty())
        return;   // nothing was ever imported (whole job failed out); nothing to finish

    QSqlQuery del(m_db);
    del.prepare("DELETE FROM imports_resume WHERE job_id=?");
    del.addBindValue(jobId);
    del.exec();

    qInfo("[import] job %s finished — %lld track(s)",
          qPrintable(jobId), static_cast<long long>(tracks.size()));
    emit libraryLoaded(loadAll(nullptr));   // ensure the UI has every row before m3u8
    emit importJobFinished(createName, appendName, tracks);
}

void MusicLibrary::loadPendingImports()
{
    if (!ensureDb())
        return;
    if (m_scanning) {   // finalize/replay below would nest in the scan's transaction
        m_deferredImports.append([this] { loadPendingImports(); });
        return;
    }
    QSqlQuery jobs(m_db);
    if (!jobs.exec("SELECT DISTINCT job_id FROM imports_resume"))
        return;
    QStringList jobIds;
    while (jobs.next())
        jobIds << jobs.value(0).toString();

    for (const QString &jobId : jobIds) {
        QSqlQuery q(m_db);
        q.prepare("SELECT entry_url, create_pl, append_pl FROM imports_resume "
                  "WHERE job_id=? AND status=0 ORDER BY seq");
        q.addBindValue(jobId);
        if (!q.exec())
            continue;
        QStringList pending;
        QString createName, appendName;
        while (q.next()) {
            pending << q.value(0).toString();
            createName = q.value(1).toString();
            appendName = q.value(2).toString();
        }
        if (pending.isEmpty()) {
            // All entries imported last session but the job never finalized (e.g. a
            // crash right after the final commit): finish it now.
            finalizeImportJobIfDone(jobId);
        } else {
            qInfo("[import] resuming job %s — %lld pending entry(ies)",
                  qPrintable(jobId), static_cast<long long>(pending.size()));
            emit resumeImportJob(jobId, createName, appendName, pending);
        }
    }
}

void MusicLibrary::resolveArt(const QString &path)
{
    if (!ensureDb())
        return;

    // Already resolved (art_url IS NOT NULL)? Reuse it, unless it points at an
    // extracted cache file that has since been deleted.
    qint64 mtime = 0;
    {
        QSqlQuery q(m_db);
        q.prepare("SELECT art_url, mtime FROM tracks WHERE path = ?");
        q.addBindValue(path);
        if (q.exec() && q.next()) {
            mtime = q.value(1).toLongLong();
            if (!q.value(0).isNull()) {
                const QString url = q.value(0).toString();
                const QString local = QUrl(url).toLocalFile();
                if (url.isEmpty() || QFileInfo::exists(local)) {
                    emit artResolved(path, url);
                    return;
                }
            }
        }
    }

    const QString url = extractArt(path, mtime);

    QSqlQuery u(m_db);
    u.prepare("UPDATE tracks SET art_url = ? WHERE path = ?");
    u.addBindValue(url);   // '' means "resolved, no art" -> won't re-extract
    u.addBindValue(path);
    u.exec();

    emit artResolved(path, url);
}

QString MusicLibrary::extractArt(const QString &path, qint64 mtime)
{
    // 1) Embedded picture (works across mp3/flac/mp4/ogg via TagLib's unified API).
    TagLib::FileRef f(QFile::encodeName(path).constData(), /*readAudioProperties=*/false);
    if (!f.isNull()) {
        const auto pictures = f.complexProperties("PICTURE");
        if (!pictures.isEmpty()) {
            // Prefer the front cover if the file tags several pictures.
            const TagLib::VariantMap *chosen = &pictures.front();
            for (const auto &p : pictures) {
                if (p.value("pictureType").value<TagLib::String>() == "Front Cover") {
                    chosen = &p;
                    break;
                }
            }
            const TagLib::ByteVector data = chosen->value("data").value<TagLib::ByteVector>();
            const QString mime = QString::fromStdString(
                chosen->value("mimeType").value<TagLib::String>().to8Bit(true));
            if (!data.isEmpty()) {
                const QString ext = mime.contains("png", Qt::CaseInsensitive) ? "png" : "jpg";
                const QString key = QCryptographicHash::hash(
                    (path + QString::number(mtime)).toUtf8(),
                    QCryptographicHash::Sha1).toHex();
                QDir().mkpath(m_artDir);
                const QString out = m_artDir + '/' + key + '.' + ext;
                if (!QFileInfo::exists(out)) {
                    QFile fo(out);
                    if (fo.open(QIODevice::WriteOnly))
                        fo.write(data.data(), data.size());
                }
                return QUrl::fromLocalFile(out).toString();
            }
        }
    }

    // 2) Folder cover image sitting next to the track (case-insensitive match).
    const QDir dir = QFileInfo(path).dir();
    const QStringList hits = dir.entryList(
        {"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg", "folder.png",
         "front.jpg", "front.png", "album.jpg", "albumart.jpg"},
        QDir::Files);
    if (!hits.isEmpty())
        return QUrl::fromLocalFile(dir.absoluteFilePath(hits.first())).toString();

    return {};   // no art found
}

QString MusicLibrary::buildMatchQuery(const QString &text, int scope)
{
    // Strip everything but letters/digits (defuses FTS5 operators and injection),
    // then prefix-match each term for type-ahead. Terms are implicitly AND-ed.
    QString cleaned;
    cleaned.reserve(text.size());
    for (const QChar c : text)
        cleaned += c.isLetterOrNumber() ? c : QChar(' ');

    const QStringList terms = cleaned.split(' ', Qt::SkipEmptyParts);
    if (terms.isEmpty())
        return {};

    QStringList prefixed;
    for (const QString &term : terms)
        prefixed << term + '*';
    const QString core = prefixed.join(' ');

    switch (scope) {
    case ScopeTitle:  return "title:("  + core + ')';
    case ScopeArtist: return "artist:(" + core + ')';
    case ScopeAlbum:  return "album:("  + core + ')';
    default:          return core;   // ScopeAll -> any indexed column
    }
}

void MusicLibrary::search(const QString &text, int scope)
{
    if (!ensureDb())
        return;

    // Same column list whichever path we take, so results build identically.
    const char *kCols = "t.uri, t.path, t.remote, t.artist, t.title, t.album, "
                        "t.duration, t.track_no, t.art_url, t.year, t.bitrate";

    QSqlQuery q(m_db);
    q.setForwardOnly(true);
    bool run = false;

    if (scope == ScopeYear) {
        // Numeric: prefix-match the year digits (e.g. "199" -> the 1990s).
        QString digits;
        for (const QChar c : text)
            if (c.isDigit())
                digits += c;
        if (!digits.isEmpty()) {
            q.prepare(QString("SELECT %1 FROM tracks t WHERE t.year > 0 AND "
                              "CAST(t.year AS TEXT) LIKE ? "
                              "ORDER BY t.year, t.artist COLLATE NOCASE").arg(kCols));
            q.addBindValue(digits + '%');
            run = true;
        }
    } else {
        const QString match = buildMatchQuery(text, scope);
        if (!match.isEmpty()) {
            q.prepare(QString("SELECT %1 FROM tracks_fts f "
                              "JOIN tracks t ON t.rowid = f.rowid "
                              "WHERE f.tracks_fts MATCH ? ORDER BY f.rank").arg(kCols));
            q.addBindValue(match);
            run = true;
        }
    }

    QList<Track> results;
    if (run) {
        if (q.exec()) {
            while (q.next()) {
                const QString path = q.value(1).toString();
                const bool remote = q.value(2).toInt() != 0;
                Track t;
                t.url = QUrl(q.value(0).toString());
                t.artist = q.value(3).toString();
                t.title = q.value(4).toString();
                t.album = q.value(5).toString();
                t.durationMs = q.value(6).toLongLong();
                t.trackNo = q.value(7).toInt();
                t.artUrl = q.value(8).toString();
                t.year = q.value(9).toInt();
                t.bitrate = q.value(10).toInt();
                if (t.title.isEmpty() && !remote)
                    t.title = QFileInfo(path).completeBaseName();
                results.append(t);
            }
        } else {
            qWarning() << "search failed:" << q.lastError().text();
        }
    }

    emit searchResults(text, results);
}

void MusicLibrary::scan(const QStringList &folders)
{
    // We pump the worker's event loop mid-scan (below) so interactive queries —
    // search, art resolution — don't starve. That can re-deliver a queued scan()
    // call; ignore it rather than recurse into a half-finished transaction.
    if (m_scanning)
        return;
    m_scanning = true;
    // On every exit path clear the guard *then* drain any imports that arrived while
    // the scan held the transaction open (m_scanning is false again, so they run).
    struct ScanGuard {
        MusicLibrary *self;
        ~ScanGuard() { self->m_scanning = false; self->flushPendingImports(); }
    } scanGuard{this};

    m_cancel.storeRelaxed(0);

    if (!ensureDb())
        return;

    qInfo("[scan] starting — %lld folder(s)",
          static_cast<long long>(folders.size()));

    // Timing instrumentation (PP_SCAN_BENCH): works on real disk scans, not just
    // the synthetic benchmark. Lets you tune scan/threads against a live library.
    const bool bench = qEnvironmentVariableIsSet("PP_SCAN_BENCH");
    QElapsedTimer scanTimer;
    scanTimer.start();

    // Phase 1 — emit whatever is cached, immediately (instant warm start).
    QHash<QString, qint64> cachedMtimes;
    const QList<Track> cached = loadAll(&cachedMtimes);
    const bool cold = cached.isEmpty();   // nothing shown yet -> safe to append live
    const qint64 loadMs = scanTimer.elapsed();
    emit libraryLoaded(cached);

    if (folders.isEmpty())
        return;

    // Walk every configured folder and decide which files actually need parsing.
    emit scanStatus(tr("Scanning library…"));
    QList<ScanItem> todo;
    QSet<QString> seen;
    for (const QString &folder : folders) {
        if (folder.isEmpty())
            continue;
        // Label progress by the configured folder's name, not its full path.
        const QString sourceLabel = QDir(folder).dirName();
        QDirIterator it(folder, audioGlobs(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            if (seen.contains(path))
                continue;   // skip files reachable from overlapping folders
            seen.insert(path);
            const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
            const auto c = cachedMtimes.constFind(path);
            if (c == cachedMtimes.cend() || c.value() != mtime)
                todo.append(ScanItem{path, mtime, sourceLabel});
        }
    }
    const qint64 walkMs = scanTimer.elapsed() - loadMs;
    const int onDisk = seen.size();
    const int total = todo.size();
    bool changed = false;

    // Phase 2 — parse the work set in parallel. TagLib parsing is pure and
    // independent; only the SQLite writes below stay single-threaded.
    if (total > 0) {
        // Fixed, power-conscious default. TagLib parsing is memory-bandwidth
        // bound, so more threads barely help while drawing more power.
        const int threads = qMin(2, QThread::idealThreadCount());
        m_pool.setMaxThreadCount(threads);

        QElapsedTimer timer;
        timer.start();

        QFuture<ParsedRow> future = QtConcurrent::mapped(
            &m_pool, todo, [](const ScanItem &it) {
                return ParsedRow{parseTags(it.path, it.mtime), it.mtime};
            });

        m_db.transaction();
        QSqlQuery up = prepareUpsert();
        int sinceCommit = 0;
        int done = 0;
        QList<Track> batch;
        batch.reserve(kAppendBatch);

        // Results arrive in input order; consume them as the pool produces them.
        for (auto it = future.begin(); it != future.end(); ++it) {
            if (m_cancel.loadRelaxed()) {
                future.cancel();
                break;
            }
            upsert(up, it->track, it->mtime);
            changed = true;
            if (cold) {
                batch.append(it->track);
                if (batch.size() >= kAppendBatch) {
                    emit tracksAppended(batch);
                    batch.clear();
                }
            }
            if (++sinceCommit >= kCommitEvery) {
                m_db.commit();
                m_db.transaction();
                sinceCommit = 0;
            }
            ++done;
            if (done % progressEvery() == 0 || done == total) {
                // Results arrive in input order, so todo[done-1] is the file just
                // parsed. Report its folder label and bare filename.
                const ScanItem &cur = todo.at(done - 1);
                emit scanProgress(done, total, cur.sourceLabel,
                                  QFileInfo(cur.path).fileName());
            }
            if (done % kPumpEvery == 0) {
                // Let queued search/art requests on this worker run between
                // batches instead of waiting for the whole scan to finish.
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            }
        }
        m_db.commit();
        if (cold && !batch.isEmpty())
            emit tracksAppended(batch);
        // Final 100% emit happens inside the loop above (done == total); skipped
        // on cancel so we never report a bogus complete.

        if (bench) {
            fprintf(stderr, "[scan] parsed %d files in %lld ms using %d thread(s) "
                            "(%.4f ms/file)\n",
                    total, static_cast<long long>(timer.elapsed()), threads,
                    double(timer.elapsed()) / total);
            fflush(stderr);
        }
    }

    if (m_cancel.loadRelaxed()) {
        emit scanStatus(QString());
        return;
    }

    // Prune local rows whose files are gone (one prepared DELETE, re-bound per
    // row). Scoped to remote=0 so streamed tracks are never pruned by the walk;
    // cachedMtimes already holds only local rows, this is belt-and-braces.
    int pruned = 0;
    QSqlQuery del(m_db);
    del.prepare("DELETE FROM tracks WHERE path=? AND remote=0");
    for (auto it = cachedMtimes.cbegin(); it != cachedMtimes.cend(); ++it) {
        if (!seen.contains(it.key())) {
            del.bindValue(0, it.key());
            del.exec();
            changed = true;
            ++pruned;
        }
    }

    // Phase 3 — normalise to the final sorted order. During a cold scan the rows
    // arrived in directory order; this one reset snaps them into artist/album
    // order (and is the only refresh needed on warm runs).
    if (changed)
        emit libraryLoaded(loadAll(nullptr));

    // Always log a one-line summary (the in-app Log + stderr); the bench env var
    // adds the finer load/walk timing breakdown for tuning.
    qInfo("[scan] done — %d on disk (%s), parsed %d, pruned %d, %lld ms",
          onDisk, cold ? "cold" : "warm", total, pruned,
          static_cast<long long>(scanTimer.elapsed()));
    if (bench) {
        fprintf(stderr, "[scan] total %lld ms — %d on disk (%s), "
                        "load %lld ms, walk %lld ms, parsed %d, pruned %d\n",
                static_cast<long long>(scanTimer.elapsed()), onDisk,
                cold ? "cold" : "warm", static_cast<long long>(loadMs),
                static_cast<long long>(walkMs), total, pruned);
        fflush(stderr);
    }

    emit scanStatus(QString());
}
