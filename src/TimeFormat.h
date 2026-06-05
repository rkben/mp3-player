#pragma once

#include <QString>

// Format a millisecond duration as "m:ss" (e.g. 0:00, 3:07, 72:15). Negative or
// zero input yields "0:00"; callers that want a blank for "unknown duration"
// (e.g. a table cell) should guard on the value before calling.
//
// QStringLiteral avoids re-allocating the template each call — this sits on the
// per-tick playback path, so it matters for a power-conscious player.
inline QString formatTime(qint64 ms)
{
    const qint64 totalSecs = ms > 0 ? ms / 1000 : 0;
    return QStringLiteral("%1:%2")
        .arg(totalSecs / 60)
        .arg(totalSecs % 60, 2, 10, QLatin1Char('0'));
}
