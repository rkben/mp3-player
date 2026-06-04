#include "YearParser.h"

#include <QRegularExpression>

namespace {
// Plausible window for a music release year. Earliest commercial recordings are
// ~1860s; the upper bound leaves slack for near-future/pre-release dates while
// rejecting obvious garbage (e.g. "9999", "3001", or a 4-digit catalogue id).
constexpr int kMinYear = 1860;
constexpr int kMaxYear = 2100;

bool plausible(int y) { return y >= kMinYear && y <= kMaxYear; }
}

int normalizeYear(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return 0;

    // Prefer a standalone 4-digit group (not embedded in a longer number), which
    // covers "2007", "2007-05-01", "05/2007", "May 2007", "1999 (Remastered)",
    // ISO timestamps, and ranges like "2007-2009" (takes the first plausible).
    static const QRegularExpression standalone(QStringLiteral(R"((?<!\d)(\d{4})(?!\d))"));
    auto it = standalone.globalMatch(s);
    while (it.hasNext()) {
        const int y = it.next().captured(1).toInt();
        if (plausible(y))
            return y;
    }

    // Fallback: a run of digits that *starts* with a year, e.g. "20070501".
    static const QRegularExpression leading(QStringLiteral(R"(^(\d{4})\d+$)"));
    const auto m = leading.match(s);
    if (m.hasMatch()) {
        const int y = m.captured(1).toInt();
        if (plausible(y))
            return y;
    }

    return 0;   // nothing usable (e.g. "", "Unknown", "0000", "07", "199x")
}
