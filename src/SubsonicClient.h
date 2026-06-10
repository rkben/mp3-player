#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QList>
#include <QPair>
#include <QStringList>

#include <functional>

#include "PlaylistModel.h"   // Track

class QJsonObject;

class QNetworkAccessManager;

// One configured Subsonic/OpenSubsonic server. `id` is a stable, generated token used to
// namespace this server's tracks (uri = subsonic://<id>/<songId>) — independent of the
// URL/host, so the URL can change and two servers on the same host don't collide.
struct SubsonicServer {
    QString id;
    QString name;       // display label (defaults to the host)
    QString url;        // base URL, e.g. https://music.example.com
    QString user;
    QString password;

    bool valid() const { return !url.isEmpty() && !user.isEmpty() && !password.isEmpty(); }
};

// Talks to a Subsonic / OpenSubsonic server (Navidrome, Airsonic, Gonic, …). Async via
// QNetworkAccessManager; an instance is bound to one server (ping/browse/sync). The
// configured servers persist in QSettings under "subsonic/servers" (an array).
//
// Authentication is the salted-token scheme: each request carries a fresh random salt and
// token = md5(password + salt), plus u/v/c/f=json. Stream/cover URLs are stable (no
// signed-URL expiry), so they're built synchronously and namespaced by server id.
class SubsonicClient : public QObject
{
    Q_OBJECT
public:
    explicit SubsonicClient(SubsonicServer server, QObject *parent = nullptr);

    static QString clientName() { return QStringLiteral("PocketPlayer"); }
    static QString apiVersion() { return QStringLiteral("1.16.1"); }

    // --- Configured servers (QSettings subsonic/servers) -----------------------
    static QList<SubsonicServer> servers();
    static void saveServers(const QList<SubsonicServer> &servers);
    static SubsonicServer server(const QString &id);   // by id; .id empty if not found
    static QString newId();                            // a fresh stable id for a new server

    // --- URL builders (authed) -------------------------------------------------
    static QUrl restUrl(const SubsonicServer &s, const QString &method,
                        const QList<QPair<QString, QString>> &params = {});
    static QString streamUrl(const QUrl &trackUri);    // resolves the server by uri id
    static QString coverArtUrl(const SubsonicServer &s, const QString &coverId);

    // identity scheme: subsonic://<serverId>/<songId>
    static QUrl trackUri(const QString &serverId, const QString &songId);
    static QString songIdFromUri(const QUrl &uri) ;
    static QString serverIdFromUri(const QUrl &uri) { return uri.host(); }
    static bool isSubsonicUri(const QUrl &uri) { return uri.scheme() == QLatin1String("subsonic"); }

    const SubsonicServer &boundServer() const { return m_server; }

    // Verify connectivity + credentials of the bound server. Emits pinged() once.
    void ping();

    // Walk the bound server's library (paged getAlbumList2 → getAlbum, bounded
    // concurrency) and emit songs in batches. Scales to large (45k+) libraries: covers
    // are NOT downloaded here — each track's art_url is a deferred "subsonic-cover:…"
    // token fetched lazily on display.
    void startSync();
    void cancelSync();
    bool syncing() const { return m_syncing; }

signals:
    void pinged(bool ok, const QString &message);   // ok -> server version; else error

    void syncStatus(const QString &message);         // progress line for the status bar
    void songsReady(const QString &serverId, const QList<Track> &tracks, qint64 epoch);
    void syncFinished(const QString &serverId, qint64 epoch);   // -> caller prunes
    void syncFailed(const QString &serverId, const QString &message);

private:
    void fetchAlbumPage(int offset);   // getAlbumList2 paging to enumerate album ids
    void pumpAlbums();                 // start getAlbum requests up to the concurrency cap
    void checkDone();                  // emit syncFinished once everything has drained
    void getJson(const QUrl &url, std::function<void(const QJsonObject &)> ok,
                 std::function<void(const QString &)> fail);

    SubsonicServer m_server;
    QNetworkAccessManager *m_net;

    // Sync state.
    bool m_syncing = false;
    qint64 m_epoch = 0;
    QStringList m_albumQueue;     // album ids still to fetch songs for
    int m_albumsInFlight = 0;
    int m_albumsDone = 0;
    bool m_enumDone = false;      // album enumeration (getAlbumList2 paging) finished
};
