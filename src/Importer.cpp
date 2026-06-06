#include "Importer.h"
#include "RemoteResolver.h"   // ytDlpPath()
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
#include <QSet>
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

// yt-dlp emits no JSON for a DRM-protected entry, only one
// "ERROR: [soundcloud] <id>: This video is DRM protected" line on stderr (one per
// track, even inside an album/playlist run). Split stderr into a DRM-protected
// count and the remaining genuine ERROR lines so we can collapse the (often many)
// DRM rejections into a single readable toast.
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
}

void Importer::start(const QString &url, bool createPlaylist,
                     const QString &appendPlaylist)
{
    if (m_busy)
        return;
    const QString exe = RemoteResolver::ytDlpPath();
    if (exe.isEmpty()) {
        emit failed(tr("yt-dlp not found — set its path in Settings"));
        return;
    }

    reset();
    m_busy = true;
    m_createPlaylist = createPlaylist;
    m_appendPlaylist = appendPlaylist;

    emit status(tr("Importing… %1").arg(url));

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &Importer::onStdout);
    // QProcess::finished passes (int, ExitStatus); the slot ignores both.
    connect(m_proc, &QProcess::finished, this, &Importer::onProcessFinished);
    // --dump-json: one JSON object per entry. --ignore-errors so a single bad
    // entry (e.g. DRM on SoundCloud) doesn't abort a whole playlist.
    suppressConsoleWindow(m_proc);
    m_proc->start(exe, {QStringLiteral("--dump-json"), QStringLiteral("--no-warnings"),
                        QStringLiteral("--ignore-errors"), url});
}

void Importer::onStdout()
{
    m_partial += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_partial.indexOf('\n')) >= 0) {
        const QByteArray line = m_partial.left(nl);
        m_partial.remove(0, nl + 1);
        if (!line.trimmed().isEmpty())
            parseLine(line);
    }
}

void Importer::parseLine(const QByteArray &line)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;   // non-JSON progress/noise line
    const QJsonObject o = doc.object();

    Track t;
    const QString page = firstOf(o, {"webpage_url", "original_url", "url"});
    if (page.isEmpty())
        return;
    t.url = QUrl(page);
    // Some extractors (e.g. archive.org) give every entry the same webpage_url;
    // keep the per-entry direct media URL to disambiguate identities below.
    m_directUrls.append(o.value(QLatin1String("url")).toString());

    // YouTube's `uploader`/`channel` is the channel, not the performer — only its
    // `artist` field (present on music tracks) is meaningful. Use it when present,
    // otherwise leave the artist unset (stored NULL). Other sources keep the
    // uploader/channel/creator fallback chain.
    const bool isYoutube = page.contains(QLatin1String("youtube.com"))
        || page.contains(QLatin1String("youtu.be"))
        || o.value(QLatin1String("extractor_key")).toString()
               .startsWith(QLatin1String("Youtube"), Qt::CaseInsensitive);
    t.artist = isYoutube ? o.value(QLatin1String("artist")).toString().trimmed()
                         : firstOf(o, {"artist", "uploader", "channel", "creator"});
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

    m_tracks.append(t);
    m_thumbs.append(firstOf(o, {"thumbnail"}));

    if (m_playlistTitle.isEmpty())
        m_playlistTitle = firstOf(o, {"playlist_title", "playlist"});
    const QString pl = m_playlistTitle.isEmpty() ? tr("import") : m_playlistTitle;

    // yt-dlp tags each playlist entry with its 1-based index and the total count;
    // both are absent for a single non-playlist URL, where we fall back to the
    // running parsed count (total unknown).
    const int total = o.value(QLatin1String("playlist_count")).toInt(
                          o.value(QLatin1String("n_entries")).toInt());
    const int index = o.value(QLatin1String("playlist_index")).toInt(m_tracks.size());
    if (total > 0)
        emit status(tr("Importing from %1 (%L2/%L3): %4")
                        .arg(pl).arg(index).arg(total).arg(t.title));
    else
        emit status(tr("Importing from %1 (%L2): %3").arg(pl).arg(index).arg(t.title));
}

void Importer::onProcessFinished()
{
    // Drain any stdout not yet delivered and flush the final line — yt-dlp's last
    // JSON object may arrive with/after finished, or without a trailing newline,
    // and onStdout only parses newline-terminated lines. Without this the last
    // track is silently dropped.
    m_partial += m_proc->readAllStandardOutput();
    for (const QByteArray &line : m_partial.split('\n'))
        if (!line.trimmed().isEmpty())
            parseLine(line);
    m_partial.clear();

    // Surface any errors yt-dlp wrote as a single toast rather than one per stderr
    // line. DRM-protected entries (SoundCloud Go+) are collapsed into one count —
    // an all-DRM album would otherwise produce dozens of identical lines.
    const QString errText = QString::fromUtf8(m_proc->readAllStandardError()).trimmed();
    const ErrorSummary errs = summarizeErrors(errText);
    QStringList toast;
    if (errs.drmCount > 0)
        toast << tr("%n track(s) skipped — DRM-protected (e.g. SoundCloud Go+)",
                    nullptr, errs.drmCount);
    toast += errs.others;
    if (!toast.isEmpty())
        emit trackFailed(toast.join('\n'));

    if (m_tracks.isEmpty()) {
        QString why;
        if (errs.drmCount > 0 && errs.others.isEmpty())
            why = tr("All tracks are DRM-protected and can't be imported");
        else if (!errs.others.isEmpty())
            why = errs.others.first();
        else
            why = errText.isEmpty() ? tr("yt-dlp returned nothing")
                                    : errText.section('\n', 0, 0);
        reset();
        emit failed(why);
        return;
    }

    // Disambiguate identities: if an entry's page URL repeats (archive.org gives
    // every track the same details page), fall back to its unique direct URL so
    // the rows don't collide on the uri primary key.
    QSet<QString> seen;
    for (int i = 0; i < m_tracks.size(); ++i) {
        QString key = m_tracks[i].url.toString();
        if (seen.contains(key) && !m_directUrls.at(i).isEmpty()) {
            m_tracks[i].url = QUrl(m_directUrls.at(i));
            key = m_tracks[i].url.toString();
        }
        seen.insert(key);
    }

    startCoverDownloads();
}

void Importer::startCoverDownloads()
{
    QDir().mkpath(m_artDir);
    m_pendingCovers = 0;
    for (int i = 0; i < m_tracks.size(); ++i) {
        const QString thumb = m_thumbs.at(i);
        if (thumb.isEmpty())
            continue;
        const QString suffix = QFileInfo(QUrl(thumb).path()).suffix().toLower();
        const QString ext = suffix == QLatin1String("png") ? QStringLiteral("png")
                                                           : QStringLiteral("jpg");
        const QString key = QCryptographicHash::hash(thumb.toUtf8(),
                                QCryptographicHash::Sha1).toHex();
        const QString out = m_artDir + '/' + key + '.' + ext;
        if (QFileInfo::exists(out)) {   // cached already
            m_tracks[i].artUrl = QUrl::fromLocalFile(out).toString();
            continue;
        }
        ++m_pendingCovers;
        QNetworkReply *reply = m_net->get(QNetworkRequest(QUrl(thumb)));
        connect(reply, &QNetworkReply::finished, this, [this, reply, out, i] {
            if (reply->error() == QNetworkReply::NoError) {
                const QByteArray data = reply->readAll();
                QFile f(out);
                if (!data.isEmpty() && f.open(QIODevice::WriteOnly)) {
                    f.write(data);
                    f.close();
                    m_tracks[i].artUrl = QUrl::fromLocalFile(out).toString();
                }
            }
            reply->deleteLater();
            finishCover();
        });
    }
    if (m_pendingCovers == 0)
        finalize();
}

void Importer::finishCover()
{
    if (--m_pendingCovers <= 0)
        finalize();
}

void Importer::finalize()
{
    const QList<Track> tracks = m_tracks;
    // Resolve the create-name from the source title now (empty if not creating).
    QString createName;
    if (m_createPlaylist)
        createName = m_playlistTitle.isEmpty() ? tr("Imported") : m_playlistTitle;
    const QString append = m_appendPlaylist;
    reset();
    emit status(QString());   // clear the status bar
    emit finished(tracks, createName, append);
}

void Importer::reset()
{
    if (m_proc) { m_proc->deleteLater(); m_proc = nullptr; }
    m_partial.clear();
    m_tracks.clear();
    m_thumbs.clear();
    m_directUrls.clear();
    m_playlistTitle.clear();
    m_pendingCovers = 0;
    m_busy = false;
}
