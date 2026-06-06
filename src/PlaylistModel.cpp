#include "PlaylistModel.h"
#include "TimeFormat.h"

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int PlaylistModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_tracks.size();
}

int PlaylistModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tracks.size())
        return {};

    const Track &t = m_tracks.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Title:    return t.title;
        case Artist:   return t.artist;
        case Album:    return t.album;
        case Year:     return t.year > 0 ? QString::number(t.year) : QString();
        case TrackNo:  return t.trackNo > 0 ? QString::number(t.trackNo) : QString();
        case Duration: return t.durationMs > 0 ? formatTime(t.durationMs) : QString();
        }
        return {};
    case Qt::TextAlignmentRole:
        if (index.column() == Year || index.column() == TrackNo
            || index.column() == Duration)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    case Qt::ToolTipRole:
        return t.isRemote() ? t.url.toString() : t.url.toLocalFile();
    default:
        return {};
    }
}

QVariant PlaylistModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Title:    return tr("Title");
    case Artist:   return tr("Artist");
    case Album:    return tr("Album");
    case Year:     return tr("Year");
    case TrackNo:  return tr("#");
    case Duration: return tr("Length");
    }
    return {};
}

void PlaylistModel::rebuildIndex()
{
    m_indexByKey.clear();
    m_indexByKey.reserve(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i)
        m_indexByKey.insert(m_tracks.at(i).key(), i);
}

void PlaylistModel::setTracks(QList<Track> tracks)
{
    beginResetModel();
    m_tracks = std::move(tracks);
    rebuildIndex();
    endResetModel();
}

void PlaylistModel::setArtUrl(int row, const QString &artUrl)
{
    if (row < 0 || row >= m_tracks.size())
        return;
    m_tracks[row].artUrl = artUrl;   // not shown in the list; no dataChanged needed
}

void PlaylistModel::updateTrack(int row, const QString &title, const QString &artist,
                                const QString &album, qint64 durationMs, int trackNo)
{
    if (row < 0 || row >= m_tracks.size())
        return;
    if (m_tracks[row].mergeFrom(title, artist, album, durationMs, trackNo))
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1),
                         {Qt::DisplayRole, Qt::ToolTipRole});
}

void PlaylistModel::clear()
{
    beginResetModel();
    m_tracks.clear();
    m_indexByKey.clear();
    endResetModel();
}
