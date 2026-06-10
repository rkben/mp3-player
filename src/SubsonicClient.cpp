#include "SubsonicClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QSettings>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QUuid>
#include <QDebug>

#include <utility>

namespace {
// A request URL with the auth secrets masked, for safe logging.
QString redacted(const QUrl &url)
{
    QUrlQuery q(url);
    for (const QString &k : {QStringLiteral("t"), QStringLiteral("s"), QStringLiteral("p")})
        if (q.hasQueryItem(k))
            q.removeQueryItem(k), q.addQueryItem(k, QStringLiteral("***"));
    QUrl u(url);
    u.setQuery(q);
    return u.toString();
}
}

SubsonicClient::SubsonicClient(SubsonicServer server, QObject *parent)
    : QObject(parent)
    , m_server(std::move(server))
    , m_net(new QNetworkAccessManager(this))
{
}

QList<SubsonicServer> SubsonicClient::servers()
{
    QList<SubsonicServer> out;
    QSettings s;
    const int n = s.beginReadArray(QStringLiteral("subsonic/servers"));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        SubsonicServer sv;
        sv.id = s.value(QStringLiteral("id")).toString();
        sv.name = s.value(QStringLiteral("name")).toString();
        sv.url = s.value(QStringLiteral("url")).toString();
        sv.user = s.value(QStringLiteral("user")).toString();
        sv.password = s.value(QStringLiteral("password")).toString();
        if (!sv.id.isEmpty())
            out.append(sv);
    }
    s.endArray();
    return out;
}

void SubsonicClient::saveServers(const QList<SubsonicServer> &servers)
{
    QSettings s;
    s.remove(QStringLiteral("subsonic/servers"));   // rewrite the whole array
    s.beginWriteArray(QStringLiteral("subsonic/servers"));
    for (int i = 0; i < servers.size(); ++i) {
        s.setArrayIndex(i);
        const SubsonicServer &sv = servers.at(i);
        s.setValue(QStringLiteral("id"), sv.id);
        s.setValue(QStringLiteral("name"), sv.name);
        s.setValue(QStringLiteral("url"), sv.url);
        s.setValue(QStringLiteral("user"), sv.user);
        s.setValue(QStringLiteral("password"), sv.password);
    }
    s.endArray();
}

SubsonicServer SubsonicClient::server(const QString &id)
{
    for (const SubsonicServer &sv : servers())
        if (sv.id == id)
            return sv;
    return SubsonicServer{};
}

QString SubsonicClient::newId()
{
    // Lowercase hex, valid as a URL host component (uri = subsonic://<id>/<songId>).
    return QUuid::createUuid().toString(QUuid::Id128);
}

QUrl SubsonicClient::restUrl(const SubsonicServer &sv, const QString &method,
                             const QList<QPair<QString, QString>> &params)
{
    QString base = sv.url.trimmed();
    if (base.isEmpty())
        return QUrl();
    while (base.endsWith('/'))
        base.chop(1);

    QUrl url(base + QStringLiteral("/rest/") + method + QStringLiteral(".view"));

    // Salted-token auth: a fresh salt per request, token = md5(password + salt).
    const QString salt = QString::number(QRandomGenerator::global()->generate(), 16)
                       + QString::number(QRandomGenerator::global()->generate(), 16);
    const QByteArray token = QCryptographicHash::hash((sv.password + salt).toUtf8(),
                                                      QCryptographicHash::Md5).toHex();

    QUrlQuery q;
    for (const auto &p : params)
        q.addQueryItem(p.first, p.second);
    q.addQueryItem(QStringLiteral("u"), sv.user);
    q.addQueryItem(QStringLiteral("t"), QString::fromLatin1(token));
    q.addQueryItem(QStringLiteral("s"), salt);
    q.addQueryItem(QStringLiteral("v"), apiVersion());
    q.addQueryItem(QStringLiteral("c"), clientName());
    q.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    url.setQuery(q);
    return url;
}

QString SubsonicClient::streamUrl(const QUrl &trackUri)
{
    const SubsonicServer sv = server(serverIdFromUri(trackUri));
    if (!sv.valid())
        return QString();
    return restUrl(sv, QStringLiteral("stream"),
                   {{QStringLiteral("id"), songIdFromUri(trackUri)}}).toString();
}

QString SubsonicClient::coverArtUrl(const SubsonicServer &sv, const QString &coverId)
{
    return restUrl(sv, QStringLiteral("getCoverArt"),
                   {{QStringLiteral("id"), coverId}}).toString();
}

QUrl SubsonicClient::trackUri(const QString &serverId, const QString &songId)
{
    QUrl u;
    u.setScheme(QStringLiteral("subsonic"));
    u.setHost(serverId);
    u.setPath('/' + songId);
    return u;
}

QString SubsonicClient::songIdFromUri(const QUrl &uri)
{
    QString p = uri.path();
    if (p.startsWith('/'))
        p.remove(0, 1);
    return p;
}

void SubsonicClient::ping()
{
    const QUrl url = restUrl(m_server, QStringLiteral("ping"));
    if (url.isEmpty()) {
        emit pinged(false, tr("Set the server URL, username and password first."));
        return;
    }
    // The scheme://host:port/path actually being hit — shown on failure so a wrong
    // scheme/port is obvious without digging through logs.
    const QString target = url.adjusted(QUrl::RemoveQuery).toString();
    qInfo().noquote() << QStringLiteral("[subsonic] ping %1").arg(redacted(url));
    QNetworkReply *reply = m_net->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, target] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning().noquote() << QStringLiteral("[subsonic] ping failed: %1 (%2) -> %3")
                                        .arg(reply->errorString())
                                        .arg(int(reply->error())).arg(target);
            emit pinged(false, tr("%1 (tried %2)").arg(reply->errorString(), target));
            return;
        }
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object()
                .value(QStringLiteral("subsonic-response")).toObject();
        if (resp.value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
            const QString msg = resp.value(QStringLiteral("error")).toObject()
                                    .value(QStringLiteral("message")).toString();
            emit pinged(false, msg.isEmpty() ? tr("Server rejected the request.") : msg);
            return;
        }
        QString ver = resp.value(QStringLiteral("type")).toString();
        const QString sv = resp.value(QStringLiteral("serverVersion")).toString();
        if (!sv.isEmpty())
            ver += ver.isEmpty() ? sv : QStringLiteral(" ") + sv;
        emit pinged(true, ver.isEmpty() ? tr("Connected.") : ver);
    });
}

// --- Sync (paged getAlbumList2 -> getAlbum, bounded concurrency) ----------------

namespace {
constexpr int kAlbumPage = 500;   // getAlbumList2 page size (max 500)
constexpr int kConcurrency = 6;   // getAlbum requests in flight
}

void SubsonicClient::getJson(const QUrl &url, std::function<void(const QJsonObject &)> ok,
                             std::function<void(const QString &)> fail)
{
    if (url.isEmpty()) {
        fail(tr("Server not configured."));
        return;
    }
    qInfo().noquote() << QStringLiteral("[subsonic] GET %1").arg(redacted(url));
    QNetworkReply *reply = m_net->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [reply, ok, fail] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning().noquote() << QStringLiteral("[subsonic] network error: %1 (%2)")
                                        .arg(reply->errorString())
                                        .arg(int(reply->error()));
            fail(reply->errorString());
            return;
        }
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http != 0 && http != 200)
            qWarning().noquote() << QStringLiteral("[subsonic] HTTP %1").arg(http);
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object()
                .value(QStringLiteral("subsonic-response")).toObject();
        if (resp.value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
            fail(resp.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("message")).toString());
            return;
        }
        ok(resp);
    });
}

void SubsonicClient::startSync()
{
    if (m_syncing)
        return;
    if (!m_server.valid()) {
        emit syncFailed(m_server.id, tr("Server not configured."));
        return;
    }
    m_syncing = true;
    m_epoch = QDateTime::currentSecsSinceEpoch();
    m_albumQueue.clear();
    m_albumsInFlight = 0;
    m_albumsDone = 0;
    m_enumDone = false;
    const QString label = m_server.name.isEmpty() ? m_server.id : m_server.name;
    qInfo().noquote() << QStringLiteral("[subsonic] sync start: %1 (%2)")
                             .arg(label, m_server.url);
    emit syncStatus(tr("Syncing %1…").arg(label));
    fetchAlbumPage(0);
}

void SubsonicClient::cancelSync()
{
    if (!m_syncing)
        return;
    m_syncing = false;              // in-flight replies finish but their handlers no-op
    m_albumQueue.clear();
    emit syncStatus(QString());
}

void SubsonicClient::fetchAlbumPage(int offset)
{
    const QUrl url = restUrl(m_server, QStringLiteral("getAlbumList2"),
                             {{QStringLiteral("type"), QStringLiteral("alphabeticalByName")},
                              {QStringLiteral("size"), QString::number(kAlbumPage)},
                              {QStringLiteral("offset"), QString::number(offset)}});
    getJson(url,
            [this, offset](const QJsonObject &resp) {
                if (!m_syncing)
                    return;
                const QJsonArray albums = resp.value(QStringLiteral("albumList2")).toObject()
                                              .value(QStringLiteral("album")).toArray();
                for (const QJsonValue &a : albums) {
                    const QString id = a.toObject().value(QStringLiteral("id")).toString();
                    if (!id.isEmpty())
                        m_albumQueue << id;
                }
                if (albums.size() >= kAlbumPage)
                    fetchAlbumPage(offset + kAlbumPage);   // another page
                else
                    m_enumDone = true;                      // last page
                pumpAlbums();
                checkDone();
            },
            [this](const QString &err) {
                if (!m_syncing)
                    return;
                m_syncing = false;
                emit syncStatus(QString());
                emit syncFailed(m_server.id, err);
            });
}

void SubsonicClient::pumpAlbums()
{
    const QString label = m_server.name.isEmpty() ? m_server.id : m_server.name;
    while (m_syncing && m_albumsInFlight < kConcurrency && !m_albumQueue.isEmpty()) {
        const QString albumId = m_albumQueue.takeFirst();
        ++m_albumsInFlight;
        const QUrl url = restUrl(m_server, QStringLiteral("getAlbum"),
                                 {{QStringLiteral("id"), albumId}});
        getJson(url,
                [this, label](const QJsonObject &resp) {
                    --m_albumsInFlight;
                    ++m_albumsDone;
                    if (m_syncing) {
                        const QJsonArray songs = resp.value(QStringLiteral("album")).toObject()
                                                     .value(QStringLiteral("song")).toArray();
                        QList<Track> batch;
                        batch.reserve(songs.size());
                        for (const QJsonValue &sv : songs) {
                            const QJsonObject o = sv.toObject();
                            Track t;
                            t.url = trackUri(m_server.id, o.value(QStringLiteral("id")).toString());
                            t.title = o.value(QStringLiteral("title")).toString();
                            t.artist = o.value(QStringLiteral("artist")).toString();
                            t.album = o.value(QStringLiteral("album")).toString();
                            t.durationMs = qint64(o.value(QStringLiteral("duration")).toDouble() * 1000.0);
                            t.trackNo = o.value(QStringLiteral("track")).toInt();
                            t.year = o.value(QStringLiteral("year")).toInt();
                            const QString cover = o.value(QStringLiteral("coverArt")).toString();
                            if (!cover.isEmpty())   // deferred cover: fetched lazily on display
                                t.artUrl = QStringLiteral("subsonic-cover:%1:%2").arg(m_server.id, cover);
                            batch << t;
                        }
                        if (!batch.isEmpty())
                            emit songsReady(m_server.id, batch, m_epoch);
                        emit syncStatus(tr("Syncing %1 — %n album(s)…", nullptr, m_albumsDone)
                                            .arg(label));
                    }
                    pumpAlbums();
                    checkDone();
                },
                [this](const QString &) {
                    --m_albumsInFlight;   // skip a failed album, keep going
                    ++m_albumsDone;
                    pumpAlbums();
                    checkDone();
                });
    }
}

void SubsonicClient::checkDone()
{
    if (m_syncing && m_enumDone && m_albumQueue.isEmpty() && m_albumsInFlight == 0) {
        m_syncing = false;
        emit syncStatus(QString());
        emit syncFinished(m_server.id, m_epoch);
    }
}
