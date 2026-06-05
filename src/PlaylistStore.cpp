#include "PlaylistStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

PlaylistStore::PlaylistStore()
{
    const QString appData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    m_playlistDir = appData + "/playlists";
    m_queueCache = appData + "/queue.m3u8";
}

QString PlaylistStore::sanitize(const QString &name)
{
    // Strip path separators and characters that are awkward in filenames; collapse
    // to a safe stem. Empty -> "playlist" so we always have a valid target.
    QString out;
    out.reserve(name.size());
    for (const QChar c : name.trimmed()) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    return out.isEmpty() ? QStringLiteral("playlist") : out;
}

QString PlaylistStore::playlistPath(const QString &name) const
{
    return m_playlistDir + '/' + sanitize(name) + ".m3u8";
}

QStringList PlaylistStore::names() const
{
    QDir dir(m_playlistDir);
    QStringList out;
    for (const QFileInfo &fi : dir.entryInfoList({"*.m3u8"}, QDir::Files, QDir::Name))
        out << fi.completeBaseName();
    return out;
}

bool PlaylistStore::exists(const QString &name) const
{
    return QFileInfo::exists(playlistPath(name));
}

QStringList PlaylistStore::readFile(const QString &path)
{
    QStringList paths;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return paths;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;   // skip blanks and #EXTM3U/#EXTINF comments
        paths << line;
    }
    return paths;
}

bool PlaylistStore::writeFile(const QString &path, const QStringList &paths,
                              bool appendMode) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    const auto mode = appendMode ? QIODevice::Append : QIODevice::WriteOnly;
    if (!f.open(mode | QIODevice::Text))
        return false;
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    for (const QString &p : paths)
        out << p << '\n';
    return true;
}

QStringList PlaylistStore::readPaths(const QString &name) const
{
    return readFile(playlistPath(name));
}

bool PlaylistStore::write(const QString &name, const QStringList &paths)
{
    return writeFile(playlistPath(name), paths, /*appendMode=*/false);
}

bool PlaylistStore::append(const QString &name, const QStringList &paths)
{
    return writeFile(playlistPath(name), paths, /*appendMode=*/true);
}

bool PlaylistStore::remove(const QString &name)
{
    return QFile::remove(playlistPath(name));
}

bool PlaylistStore::rename(const QString &from, const QString &to)
{
    return QFile::rename(playlistPath(from), playlistPath(to));
}

void PlaylistStore::saveQueueCache(const QStringList &paths)
{
    writeFile(m_queueCache, paths, /*appendMode=*/false);
}

QStringList PlaylistStore::readQueueCache() const
{
    return readFile(m_queueCache);
}
