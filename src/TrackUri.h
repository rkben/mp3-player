#pragma once

#include <QString>
#include <QUrl>

// Resolve a stored playlist/queue line back to a QUrl. Lines may be either a
// legacy bare local path (e.g. "/music/x.mp3"), a "file://" URL, or a remote
// "http(s)://" page URL — so we detect a recognised scheme and otherwise treat
// the line as a local filesystem path. Keeps m3u8 files interoperable (plain
// paths for local tracks) while still round-tripping remote URLs.
inline QUrl urlFromStored(const QString &line)
{
    const QString s = line.trimmed();
    if (s.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || s.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        || s.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
        return QUrl(s);
    return QUrl::fromLocalFile(s);
}

// The form a track is written as in a playlist/queue file: a plain local path
// for local tracks (human-readable, interoperable), the full URL for remotes.
inline QString storedForm(const QUrl &url)
{
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}
