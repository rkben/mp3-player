// Thin CLI around normalizeYear(): reads one raw date string per line on stdin
// and writes "<raw>\t<year>" per line. Used by scripts/build_year_cases.py to
// pre-fill the golden test fixture with the parser's actual output, so the
// fixture characterises current behaviour and flags any future drift.
//
// Lines containing a tab or carriage return are passed through verbatim except
// the trailing CR is stripped; callers must not feed embedded newlines.
#include "YearParser.h"

#include <QString>

#include <iostream>
#include <string>

int main()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r')   // tolerate CRLF input
            line.pop_back();
        const int year = normalizeYear(QString::fromStdString(line));
        std::cout << line << '\t' << year << '\n';
    }
    return 0;
}
