#pragma once

#include <QString>

// Normalize a raw tag date/year string to a 4-digit year, or 0 if none can be
// confidently extracted. Handles the many shapes tags use in the wild:
// "2007", "2007-05-01", ISO timestamps, "May 2007", "YYYYMMDD", ranges, etc.
// Deliberately conservative: implausible values (outside ~1860..2100) yield 0.
int normalizeYear(const QString &raw);
