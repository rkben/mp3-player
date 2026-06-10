#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QHash>
#include <QUrl>
#include <QMetaType>

struct Track {
    QUrl url;
    QString title;      // best-effort display name (filename fallback)
    QString artist;
    QString album;
    QString artUrl;       // file:// URL to cover art, resolved lazily on play
    qint64 durationMs = 0;
    int trackNo = 0;
    int year = 0;         // 0 = unknown
    int bitrate = 0;      // kbps, from TagLib AudioProperties; 0 = unknown/remote

    // "Artist — Title" when an artist tag exists, else just the title.
    QString displayText() const {
        return artist.isEmpty() ? title : artist + QStringLiteral(" — ") + title;
    }

    // Stable identity for a track, local or remote: the full URL string. Local
    // files are "file://…", remote tracks their "https://…" page URL. Used as the
    // DB key, the model row index, and the MPRIS track id.
    QString key() const { return url.toString(QUrl::FullyEncoded); }
    bool isRemote() const { return !url.isLocalFile(); }
    // A Subsonic-server track (subsonic://<serverId>/<songId>): remote, but its stream
    // URL is built synchronously from config — no yt-dlp resolve/prefetch needed.
    bool isSubsonic() const { return url.scheme() == QLatin1String("subsonic"); }

    // Overlay non-empty/positive fields from `other` onto this track, leaving the
    // rest intact. Returns true if anything actually changed. Shared by the model
    // and the player so the "keep existing values" rule lives in one place.
    bool mergeFrom(const QString &t, const QString &a, const QString &al,
                   qint64 dur, int tno) {
        bool changed = false;
        if (!t.isEmpty()  && title  != t)  { title  = t;  changed = true; }
        if (!a.isEmpty()  && artist != a)  { artist = a;  changed = true; }
        if (!al.isEmpty() && album  != al) { album  = al; changed = true; }
        if (dur > 0       && durationMs != dur) { durationMs = dur; changed = true; }
        if (tno > 0       && trackNo != tno)    { trackNo = tno;    changed = true; }
        return changed;
    }
};

// Tracks as a sortable table model (Title/Artist/Album/#/Duration) driving a
// QTableView. Rows index into a flat QList<Track>; callers index by source row.
// A QListView bound to column 0 still works for compact/mobile presentation.
class PlaylistModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Title = 0, Artist, Album, Year, TrackNo, Duration, ColumnCount };

    explicit PlaylistModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    void setTracks(QList<Track> tracks);
    void clear();
    bool isEmpty() const { return m_tracks.isEmpty(); }
    int count() const { return m_tracks.size(); }

    const Track &at(int row) const { return m_tracks.at(row); }
    // O(1) row lookup by track key (url string); -1 if not present. Kept in sync
    // with the row list so callers don't have to linear-scan on every track change.
    int rowForKey(const QString &key) const { return m_indexByKey.value(key, -1); }
    void setArtUrl(int row, const QString &artUrl);
    // Merge in player-resolved metadata; only non-empty/positive fields overwrite.
    void updateTrack(int row, const QString &title, const QString &artist,
                     const QString &album, qint64 durationMs, int trackNo);

private:
    void rebuildIndex();   // refresh m_indexByKey from m_tracks

    QList<Track> m_tracks;
    QHash<QString, int> m_indexByKey;   // track key (url string) -> row, O(1) lookup
};

Q_DECLARE_METATYPE(Track)
