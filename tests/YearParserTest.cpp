#include <QtTest>

#include "YearParser.h"

// Exercises normalizeYear() against the rogue date/year formats found in real
// tags. Run with: ctest --test-dir build  (or ./build/year_test)
class YearParserTest : public QObject
{
    Q_OBJECT
private slots:
    void normalizes_data();
    void normalizes();
};

void YearParserTest::normalizes_data()
{
    QTest::addColumn<QString>("raw");
    QTest::addColumn<int>("expected");

    // Plain years
    QTest::newRow("plain")            << "2007"                  << 2007;
    QTest::newRow("whitespace")       << "  1985  "             << 1985;

    // Dates with separators
    QTest::newRow("iso-date")         << "2007-05-01"            << 2007;
    QTest::newRow("iso-timestamp")    << "1985-03-12T00:00:00Z"  << 1985;
    QTest::newRow("slash-ymd")        << "2007/05/01"            << 2007;
    QTest::newRow("dot-ym")           << "2007.05"               << 2007;
    QTest::newRow("month-first")      << "05/2007"               << 2007;

    // Words around the year
    QTest::newRow("month-name")       << "May 2007"              << 2007;
    QTest::newRow("remaster-suffix")  << "1999 (Remastered)"     << 1999;

    // Ranges -> first plausible
    QTest::newRow("range")            << "2007-2009"             << 2007;

    // Compact YYYYMMDD
    QTest::newRow("yyyymmdd")         << "20070501"              << 2007;

    // Unknown / junk -> 0
    QTest::newRow("empty")            << ""                      << 0;
    QTest::newRow("whitespace-only")  << "   "                  << 0;
    QTest::newRow("zeros")            << "0000"                  << 0;
    QTest::newRow("two-digit")        << "07"                    << 0;
    QTest::newRow("text")             << "Unknown"               << 0;
    QTest::newRow("partial-decade")   << "199x"                  << 0;

    // Implausible numbers -> 0
    QTest::newRow("too-old")          << "1234"                  << 0;
    QTest::newRow("far-future")       << "3001"                  << 0;
    QTest::newRow("catalogue-id")     << "12345"                 << 0;
}

void YearParserTest::normalizes()
{
    QFETCH(QString, raw);
    QFETCH(int, expected);
    QCOMPARE(normalizeYear(raw), expected);
}

QTEST_APPLESS_MAIN(YearParserTest)
#include "YearParserTest.moc"
