#include "TrackListModel.h"

#include <QMap>

#include <utility>

qint64 totalDurationMs(const QList<Track> &tracks)
{
    qint64 total = 0;
    for (const Track &t : tracks)
        total += t.durationMs;
    return total;
}

TrackListModel::TrackListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

void TrackListModel::setSource(QList<Track> tracks)
{
    m_all = std::move(tracks);
    rebuild();
}

void TrackListModel::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    rebuild();
}

void TrackListModel::setFilter(const QString &text, bool includeRemote)
{
    const QString lowered = text.trimmed().toLower();
    if (lowered == m_filter && includeRemote == m_includeRemote)
        return;
    m_filter = lowered;
    m_includeRemote = includeRemote;
    rebuild();
}

bool TrackListModel::passesFilter(const Track &t) const
{
    if (t.isRemote() && !m_includeRemote)
        return false;
    if (m_filter.isEmpty())
        return true;
    return t.title.toLower().contains(m_filter)
        || t.artist.toLower().contains(m_filter)
        || t.album.toLower().contains(m_filter);
}

void TrackListModel::rebuild()
{
    beginResetModel();
    m_entries.clear();
    m_visibleTrackCount = 0;
    m_visibleDurationMs = 0;

    if (m_mode == Tracks) {
        for (const Track &t : m_all) {
            if (!passesFilter(t))
                continue;
            m_entries.append({t.displayText(), {t}, t.durationMs});
        }
    } else {
        // Group by album or artist. QMap keeps the groups sorted by label; an empty
        // tag buckets under "Unknown" so those tracks aren't silently dropped.
        const QString unknown = tr("Unknown");
        QMap<QString, Entry> groups;
        for (const Track &t : m_all) {
            if (!passesFilter(t))
                continue;
            QString key = (m_mode == Albums) ? t.album : t.artist;
            if (key.isEmpty())
                key = unknown;
            Entry &e = groups[key];
            if (e.label.isEmpty())
                e.label = key;
            e.tracks.append(t);
            e.durationMs += t.durationMs;
        }
        m_entries = groups.values();
    }

    for (const Entry &e : m_entries) {
        m_visibleTrackCount += e.tracks.size();
        m_visibleDurationMs += e.durationMs;
    }
    endResetModel();
}

int TrackListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant TrackListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:    return e.label;
    case TrackCountRole:     return e.tracks.size();
    default:                 return {};
    }
}

QList<Track> TrackListModel::tracksForRow(int row) const
{
    if (row < 0 || row >= m_entries.size())
        return {};
    return m_entries.at(row).tracks;
}
