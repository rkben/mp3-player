#include "PlaylistModel.h"

namespace {
QString formatDuration(qint64 ms)
{
    if (ms <= 0)
        return {};
    const qint64 secs = ms / 1000;
    return QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
}
}

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
        case Duration: return formatDuration(t.durationMs);
        }
        return {};
    case Qt::TextAlignmentRole:
        if (index.column() == Year || index.column() == TrackNo
            || index.column() == Duration)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    case Qt::ToolTipRole:
        return t.url.toLocalFile();
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

void PlaylistModel::setTracks(QList<Track> tracks)
{
    beginResetModel();
    m_tracks = std::move(tracks);
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
    Track &t = m_tracks[row];
    bool changed = false;
    if (!title.isEmpty()  && t.title  != title)  { t.title  = title;  changed = true; }
    if (!artist.isEmpty() && t.artist != artist) { t.artist = artist; changed = true; }
    if (!album.isEmpty()  && t.album  != album)  { t.album  = album;  changed = true; }
    if (durationMs > 0    && t.durationMs != durationMs) { t.durationMs = durationMs; changed = true; }
    if (trackNo > 0       && t.trackNo != trackNo) { t.trackNo = trackNo; changed = true; }
    if (changed)
        emit dataChanged(index(row, 0), index(row, ColumnCount - 1),
                         {Qt::DisplayRole, Qt::ToolTipRole});
}

void PlaylistModel::appendTracks(const QList<Track> &tracks)
{
    if (tracks.isEmpty())
        return;
    const int first = m_tracks.size();
    beginInsertRows({}, first, first + tracks.size() - 1);
    m_tracks.append(tracks);
    endInsertRows();
}

void PlaylistModel::clear()
{
    beginResetModel();
    m_tracks.clear();
    endResetModel();
}

void PlaylistModel::setTitle(int row, const QString &title)
{
    if (row < 0 || row >= m_tracks.size() || title.isEmpty())
        return;
    if (m_tracks[row].title == title)
        return;
    m_tracks[row].title = title;
    const QModelIndex idx = index(row, Title);
    emit dataChanged(idx, idx, {Qt::DisplayRole});
}
