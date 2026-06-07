#pragma once

#include <QAbstractListModel>
#include <QList>

#include "PlaylistModel.h"   // Track

// Sum the duration of a track list. Free helper so the count/runtime footers in the
// playlist editor and the queue header share one definition (see TimeFormat.h for
// the formatting side).
qint64 totalDurationMs(const QList<Track> &tracks);

// A flat, read-only list of tracks for the playlist editor's left pane, with
// built-in filtering (text + include-remote) and a grouping view mode. The full
// catalogue is held once; the visible rows are recomputed whenever the mode or
// filter changes. In Albums/Artists mode each row is a group (distinct album/artist)
// aggregating its member tracks, so the move arrow can pull the whole group.
class TrackListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Mode { Tracks, Albums, Artists };

    // Extra roles beyond DisplayRole (the row label): the member-track count for a
    // row (1 in Tracks mode, group size otherwise).
    enum Roles { TrackCountRole = Qt::UserRole + 1 };

    explicit TrackListModel(QObject *parent = nullptr);

    void setSource(QList<Track> tracks);                 // the full catalogue
    void setMode(Mode mode);
    void setFilter(const QString &text, bool includeRemote);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // The track(s) behind a visible row: one in Tracks mode, the whole group in
    // Albums/Artists mode. Empty for an invalid row.
    QList<Track> tracksForRow(int row) const;

    Mode mode() const { return m_mode; }
    int visibleTrackCount() const { return m_visibleTrackCount; }
    qint64 visibleDurationMs() const { return m_visibleDurationMs; }

private:
    // One visible row: a label plus the tracks it represents (and their total time).
    struct Entry {
        QString label;
        QList<Track> tracks;
        qint64 durationMs = 0;
    };

    void rebuild();   // recompute m_entries from m_all under the current mode/filter
    bool passesFilter(const Track &t) const;

    QList<Track> m_all;          // full catalogue (unfiltered)
    QList<Entry> m_entries;      // visible rows
    Mode m_mode = Tracks;
    QString m_filter;            // lower-cased search text
    bool m_includeRemote = false;
    int m_visibleTrackCount = 0;
    qint64 m_visibleDurationMs = 0;
};
