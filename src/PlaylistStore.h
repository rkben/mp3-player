#pragma once

#include <QString>
#include <QStringList>

// Minimal m3u8-backed playlist storage under the app data directory.
//
// Playlists live at <AppData>/playlists/<name>.m3u8; the resumable current-queue
// cache sits at <AppData>/queue.m3u8 (outside the playlists dir so it isn't
// listed). Files hold one absolute path per line, UTF-8, no EXTINF — reading
// tolerates blank lines and '#' comments so external #EXTM3U files load too.
//
// This class only moves path lists; resolving paths back to Track metadata is the
// caller's job (MainWindow uses the scanned library), so the store stays
// independent of the player/library.
class PlaylistStore
{
public:
    PlaylistStore();

    QStringList names() const;                       // playlist names (no ext), sorted
    bool exists(const QString &name) const;
    QStringList readPaths(const QString &name) const;

    bool write(const QString &name, const QStringList &paths);   // create or overwrite
    bool append(const QString &name, const QStringList &paths);
    bool remove(const QString &name);
    bool rename(const QString &from, const QString &to);

    void saveQueueCache(const QStringList &paths);
    QStringList readQueueCache() const;

private:
    QString playlistPath(const QString &name) const;
    bool writeFile(const QString &path, const QStringList &paths, bool appendMode) const;
    static QStringList readFile(const QString &path);
    static QString sanitize(const QString &name);   // -> safe filename stem

    QString m_playlistDir;   // <AppData>/playlists
    QString m_queueCache;    // <AppData>/queue.m3u8
};
