#include "Importer.h"
#include "ProcUtil.h"

#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QUuid>
#include <QUrl>

namespace {
// First non-empty string value among `keys` in `o`.
QString firstOf(const QJsonObject &o, std::initializer_list<const char *> keys)
{
    for (const char *k : keys) {
        const QString v = o.value(QLatin1String(k)).toString().trimmed();
        if (!v.isEmpty())
            return v;
    }
    return {};
}

// Sources that need bespoke field mapping; everything else is Generic. Detected by
// page-URL host or yt-dlp's extractor_key (the host check alone misses CDN/redirect
// URLs, the key alone misses mirrors — either is sufficient).
enum class Source { Generic, Youtube, Mixcloud, HearThis, ReverbNation };

Source detectSource(const QString &page, const QString &extractorKey)
{
    auto has = [&](const char *host, const char *key) {
        return page.contains(QLatin1String(host), Qt::CaseInsensitive)
            || extractorKey.startsWith(QLatin1String(key), Qt::CaseInsensitive);
    };
    if (has("youtube.com", "Youtube") || page.contains(QLatin1String("youtu.be")))
        return Source::Youtube;
    if (has("mixcloud.com", "Mixcloud"))       return Source::Mixcloud;
    if (has("hearthis.at", "HearThis"))        return Source::HearThis;
    if (has("reverbnation.com", "ReverbNation")) return Source::ReverbNation;
    return Source::Generic;
}

// Map one --dump-json object to a Track's metadata (identity is set by the caller to
// the resume entry URL). Artist mapping is per-source (see notes/new_remote_sources.md):
//  - YouTube: `uploader`/`channel` is the channel, not the performer — only the
//    `artist` field (present on music tracks) is meaningful; else leave unset.
//  - Mixcloud/ReverbNation: the uploader *is* the artist (DJ sets / the song's artist
//    page); Mixcloud's own `artist` tag is redundant, so prefer uploader.
//  - HearThis: no reliable artist/uploader — leave unset rather than guess.
//  - Generic: the uploader/channel/creator fallback chain.
Track trackFromJson(const QJsonObject &o, const QString &page)
{
    Track t;
    const QString extractorKey = o.value(QLatin1String("extractor_key")).toString();
    switch (detectSource(page, extractorKey)) {
    case Source::Youtube:
        t.artist = o.value(QLatin1String("artist")).toString().trimmed();
        break;
    case Source::Mixcloud:
    case Source::ReverbNation:
        t.artist = firstOf(o, {"uploader", "artist"});
        break;
    case Source::HearThis:
        t.artist.clear();
        break;
    case Source::Generic:
        t.artist = firstOf(o, {"artist", "uploader", "channel", "creator"});
        break;
    }
    t.album = firstOf(o, {"album", "playlist", "playlist_title"});
    if (t.album.isEmpty())
        t.album = QStringLiteral("_unknown");
    t.title = firstOf(o, {"track", "title", "fulltitle"});
    if (t.title.isEmpty())
        t.title = page;
    t.durationMs = qint64(o.value(QLatin1String("duration")).toDouble() * 1000.0);
    const int ry = o.value(QLatin1String("release_year")).toInt();
    if (ry > 0)
        t.year = ry;
    else
        t.year = firstOf(o, {"upload_date"}).left(4).toInt();   // YYYYMMDD -> YYYY
    return t;
}

// yt-dlp emits no JSON for a DRM-protected entry, only one
// "ERROR: [soundcloud] <id>: This video is DRM protected" line on stderr (one per
// track, even inside an album/playlist run). Split stderr into a DRM-protected count
// and the remaining genuine ERROR lines so we can collapse the (often many) DRM
// rejections into a single readable toast.
struct ErrorSummary {
    int drmCount = 0;
    QStringList others;
};
ErrorSummary summarizeErrors(const QString &stderrText)
{
    ErrorSummary s;
    for (const QString &l : stderrText.split('\n', Qt::SkipEmptyParts)) {
        if (!l.contains(QStringLiteral("ERROR"), Qt::CaseInsensitive))
            continue;
        if (l.contains(QStringLiteral("DRM"), Qt::CaseInsensitive))
            ++s.drmCount;
        else
            s.others << l;
    }
    return s;
}
}

Importer::Importer(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    m_artDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + QStringLiteral("/art");
    connect(&m_enumDlp, &YtDlp::finished, this, &Importer::onEnumerateDone);
    connect(&m_enumDlp, &YtDlp::startFailed, this, [this](const QString &msg) {
        if (!m_cur.fromResume)
            emit failed(msg);
        emit status(QString());
        m_running = false;
        pump();
    });
}

void Importer::start(const QString &url, bool createPlaylist,
                     const QString &appendPlaylist)
{
    Job job;
    job.jobId = QUuid::createUuid().toString(QUuid::Id128);
    job.sourceUrl = url;
    job.createPlaylist = createPlaylist;
    job.appendName = appendPlaylist;
    job.needEnumerate = true;
    enqueue(std::move(job));
}

void Importer::start(const QString &url, const QStringList &entries, bool createPlaylist,
                     const QString &appendPlaylist, const QString &playlistTitle)
{
    if (entries.isEmpty()) {   // nothing pre-enumerated: fall back to enumerating
        start(url, createPlaylist, appendPlaylist);
        return;
    }
    Job job;
    job.jobId = QUuid::createUuid().toString(QUuid::Id128);
    job.sourceUrl = url;
    job.createPlaylist = createPlaylist;
    job.appendName = appendPlaylist;
    job.entryUrls = entries;
    job.playlistTitle = playlistTitle;
    job.needEnumerate = false;     // already enumerated by the dialog's Check
    job.emitEnumerated = true;     // but still write resume rows before hydrating
    enqueue(std::move(job));
}

void Importer::resumeImportJob(const QString &jobId, const QString &createName,
                               const QString &appendName, const QStringList &entryUrls)
{
    if (entryUrls.isEmpty())
        return;
    Job job;
    job.jobId = jobId;
    job.createName = createName;
    job.appendName = appendName;
    job.entryUrls = entryUrls;
    job.needEnumerate = false;
    job.fromResume = true;
    enqueue(std::move(job));
}

void Importer::enqueue(Job job)
{
    m_queue.enqueue(std::move(job));
    pump();
}

void Importer::pump()
{
    if (m_running || m_queue.isEmpty())
        return;
    const QString exe = YtDlp::path();
    if (exe.isEmpty()) {
        // Drain the queue: without yt-dlp nothing can proceed. Only fresh jobs warrant
        // a notification (a resume should stay quiet and just wait for a future launch).
        Job job = m_queue.dequeue();
        if (!job.fromResume)
            emit failed(tr("yt-dlp not found — set its path in Settings"));
        pump();
        return;
    }

    m_cur = m_queue.dequeue();
    m_running = true;
    m_partial.clear();
    m_enumPlaylistTitle = m_cur.playlistTitle;   // known up-front on the pre-enumerated path
    m_pending.clear();
    m_committed = 0;
    m_pendingCovers = 0;
    m_hydrateProcDone = false;

    if (m_cur.needEnumerate)
        startEnumerate();         // -> onEnumerateDone -> beginHydrateJob
    else if (m_cur.emitEnumerated)
        beginHydrateJob();        // pre-enumerated fresh import: write rows, then hydrate
    else
        startHydrate();           // resume: rows already exist
}

void Importer::clearProc()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_partial.clear();
}

// --- Phase 1: enumerate ------------------------------------------------------------

void Importer::startEnumerate()
{
    qInfo().noquote() << QStringLiteral("[import] enumerate %1").arg(m_cur.sourceUrl);
    emit status(tr("Reading %1…").arg(m_cur.sourceUrl));
    // --flat-playlist: list entries without extracting each — cheap and fast. JSON per
    // entry carries its page URL (and the playlist title for the create-name). Buffered
    // via the runner; parsed in onEnumerateDone.
    m_enumDlp.run({QStringLiteral("--flat-playlist"), QStringLiteral("--dump-json"),
                   QStringLiteral("--no-warnings"), QStringLiteral("--ignore-errors"),
                   m_cur.sourceUrl});
}

void Importer::onEnumerateDone(int /*code*/, const QByteArray &out, const QByteArray &err)
{
    QStringList entries;
    for (const QByteArray &line : out.split('\n')) {
        if (line.trimmed().isEmpty())
            continue;
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        const QJsonObject o = doc.object();
        const QString url = firstOf(o, {"url", "webpage_url", "original_url"});
        if (!url.isEmpty() && !entries.contains(url))
            entries << url;
        if (m_enumPlaylistTitle.isEmpty())
            m_enumPlaylistTitle = firstOf(o, {"playlist_title", "playlist", "title"});
    }

    if (entries.isEmpty()) {
        const QString errText = QString::fromUtf8(err).trimmed();
        const QString why = errText.isEmpty() ? tr("yt-dlp returned nothing")
                                              : errText.section('\n', 0, 0);
        qWarning().noquote() << QStringLiteral("[import] enumerate failed — %1").arg(why);
        emit status(QString());
        if (!m_cur.fromResume)
            emit failed(why);
        m_running = false;
        pump();
        return;
    }

    m_cur.entryUrls = entries;
    qInfo("[import] enumerated %lld entry(ies) for job %s",
          static_cast<long long>(entries.size()), qPrintable(m_cur.jobId));
    beginHydrateJob();
}

void Importer::beginHydrateJob()
{
    if (m_cur.createPlaylist)
        m_cur.createName = m_enumPlaylistTitle.isEmpty() ? tr("Imported")
                                                         : m_enumPlaylistTitle;
    // Persist the resume rows before hydrating; commits land after via queued signals.
    emit enumerated(m_cur.jobId, m_cur.createName, m_cur.appendName, m_cur.entryUrls);
    startHydrate();
}

// --- Phase 2: hydrate --------------------------------------------------------------

void Importer::startHydrate()
{
    m_pending = QSet<QString>(m_cur.entryUrls.cbegin(), m_cur.entryUrls.cend());
    m_seenUris.clear();
    m_committed = 0;
    m_pendingCovers = 0;
    m_hydrateProcDone = false;

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &Importer::onHydrateStdout);
    connect(m_proc, &QProcess::finished, this, &Importer::onHydrateFinished);
    // Feed the entry URLs once the process is up (don't block the GUI thread waiting).
    connect(m_proc, &QProcess::started, this, [this] {
        m_proc->write(m_cur.entryUrls.join('\n').toUtf8());
        m_proc->write("\n");
        m_proc->closeWriteChannel();
    });
    suppressConsoleWindow(m_proc);
    // --dump-json over the pending entry URLs, fed on stdin via --batch-file - so a
    // huge playlist can't blow ARG_MAX. --ignore-errors so one bad entry (e.g. DRM)
    // doesn't abort the rest; those simply produce no JSON and are retried/dropped.
    m_proc->start(YtDlp::path(),
                  {QStringLiteral("--dump-json"), QStringLiteral("--no-warnings"),
                   QStringLiteral("--ignore-errors"),
                   QStringLiteral("--batch-file"), QStringLiteral("-")});
}

void Importer::onHydrateStdout()
{
    m_partial += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_partial.indexOf('\n')) >= 0) {
        const QByteArray line = m_partial.left(nl);
        m_partial.remove(0, nl + 1);
        if (!line.trimmed().isEmpty())
            parseHydratedLine(line);
    }
}

void Importer::parseHydratedLine(const QByteArray &line)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;   // non-JSON progress/noise line
    const QJsonObject o = doc.object();

    // The track's own page is its identity; yt-dlp echoes the URL we fed as
    // original_url, which ties it back to a resume row (its entry). One fed entry can
    // be a sub-playlist that hydrates into several tracks — all share that original_url.
    const QString page = firstOf(o, {"webpage_url", "original_url", "url"});
    if (page.isEmpty())
        return;
    QString entry = firstOf(o, {"original_url"});
    if (!m_pending.contains(entry)) {
        const QString wp = firstOf(o, {"webpage_url"});
        if (m_pending.contains(wp))
            entry = wp;
    }
    if (entry.isEmpty())
        entry = page;

    Track t = trackFromJson(o, page);
    t.url = QUrl(page);
    // Some sources (archive.org) give every track the same webpage_url; fall back to
    // the per-track direct URL so distinct tracks don't collide on the uri key.
    if (m_seenUris.contains(t.key())) {
        const QString direct = o.value(QLatin1String("url")).toString();
        if (!direct.isEmpty())
            t.url = QUrl(direct);
    }
    m_seenUris.insert(t.key());
    m_pending.remove(entry);   // entry satisfied (extra tracks of a set still import)

    const QString pl = m_cur.createName.isEmpty()
                           ? (m_enumPlaylistTitle.isEmpty() ? tr("import") : m_enumPlaylistTitle)
                           : m_cur.createName;
    emit status(tr("Importing from %1 (%L2/%L3): %4")
                    .arg(pl).arg(m_committed + 1).arg(m_cur.entryUrls.size()).arg(t.title));

    startCover(entry, t, firstOf(o, {"thumbnail"}));
}

void Importer::startCover(const QString &entryUrl, Track track, const QString &thumb)
{
    if (thumb.isEmpty()) {
        emitImported(entryUrl, track);
        return;
    }
    const QString suffix = QFileInfo(QUrl(thumb).path()).suffix().toLower();
    const QString ext = suffix == QLatin1String("png") ? QStringLiteral("png")
                                                       : QStringLiteral("jpg");
    const QString key = QCryptographicHash::hash(thumb.toUtf8(),
                            QCryptographicHash::Sha1).toHex();
    const QString out = m_artDir + '/' + key + '.' + ext;
    if (QFileInfo::exists(out)) {   // cached already
        track.artUrl = QUrl::fromLocalFile(out).toString();
        emitImported(entryUrl, track);
        return;
    }
    QDir().mkpath(m_artDir);
    ++m_pendingCovers;
    QNetworkReply *reply = m_net->get(QNetworkRequest(QUrl(thumb)));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, out, entryUrl, track]() mutable {
        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray data = reply->readAll();
            QFile f(out);
            if (!data.isEmpty() && f.open(QIODevice::WriteOnly)) {
                f.write(data);
                f.close();
                track.artUrl = QUrl::fromLocalFile(out).toString();
            }
        }
        reply->deleteLater();
        --m_pendingCovers;
        emitImported(entryUrl, track);
        finishHydratePassIfReady();
    });
}

void Importer::emitImported(const QString &entryUrl, const Track &track)
{
    // entryUrl ties the track to its resume row (already removed from m_pending at parse
    // time). The store clears/records the row; a sub-playlist's extra tracks reuse the
    // same entryUrl and the store files them on their own done rows.
    ++m_committed;
    emit trackImported(m_cur.jobId, entryUrl, track);
}

void Importer::onHydrateFinished()
{
    onHydrateStdout();   // drain whatever stdout hasn't been delivered yet
    if (!m_partial.trimmed().isEmpty()) {
        m_partial += '\n';
        onHydrateStdout();
    }
    m_partial.clear();

    // Collapse stderr into a single toast (DRM rejections counted, not one line each).
    const QString errText = QString::fromUtf8(m_proc->readAllStandardError()).trimmed();
    const ErrorSummary errs = summarizeErrors(errText);
    QStringList toast;
    if (errs.drmCount > 0)
        toast << tr("%n track(s) skipped — DRM-protected (e.g. SoundCloud Go+)",
                    nullptr, errs.drmCount);
    toast += errs.others;
    if (!toast.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[import] %1").arg(toast.join(QLatin1String("; ")));
        emit trackFailed(toast.join('\n'));
    }

    m_hydrateProcDone = true;
    finishHydratePassIfReady();
}

void Importer::finishHydratePassIfReady()
{
    if (!m_hydrateProcDone || m_pendingCovers > 0)
        return;   // still draining covers

    // Anything still pending produced no JSON this pass: bump its attempt count (the
    // store drops it after the retry cap). Leftovers are retried on the next launch.
    if (!m_pending.isEmpty()) {
        const QStringList failedEntries(m_pending.cbegin(), m_pending.cend());
        emit importEntriesFailed(m_cur.jobId, failedEntries);
    }

    qInfo("[import] hydrate pass for job %s — %d imported, %lld left pending",
          qPrintable(m_cur.jobId), m_committed, static_cast<long long>(m_pending.size()));

    // A user-started job that imported nothing at all warrants a notification; a
    // background resume stays quiet and simply retries next time.
    if (m_committed == 0 && !m_cur.fromResume)
        emit failed(tr("Nothing could be imported from this source"));

    clearProc();
    emit status(QString());   // clear the status bar
    m_running = false;
    pump();
}
