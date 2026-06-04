#pragma once

#include <QAbstractTableModel>
#include <QList>
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

    // "Artist — Title" when an artist tag exists, else just the title.
    QString displayText() const {
        return artist.isEmpty() ? title : artist + QStringLiteral(" — ") + title;
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
    void appendTracks(const QList<Track> &tracks);
    void clear();
    bool isEmpty() const { return m_tracks.isEmpty(); }
    int count() const { return m_tracks.size(); }

    const Track &at(int row) const { return m_tracks.at(row); }
    void setTitle(int row, const QString &title);
    void setArtUrl(int row, const QString &artUrl);
    // Merge in player-resolved metadata; only non-empty/positive fields overwrite.
    void updateTrack(int row, const QString &title, const QString &artist,
                     const QString &album, qint64 durationMs, int trackNo);

private:
    QList<Track> m_tracks;
};

Q_DECLARE_METATYPE(Track)
