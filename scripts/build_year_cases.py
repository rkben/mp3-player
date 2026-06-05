#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Filter extracted date tags down to a small, diverse parser-test fixture.

Takes the TSV produced by extract_dates.py and reduces a whole library's worth of
date strings to a representative sample for testing normalizeYear():

  1. Collect every date value (splitting multi-valued ';' cells), dedup, count.
  2. Bucket by structural "shape" — digits->D, letters->L, punctuation kept
     literally (so "2007-05-01" -> "DDDD-DD-DD", "May 2007" -> "LLL DDDD").
  3. Keep up to N examples per shape, drop the trivial bare-year shape down to a
     couple, and order rarest-shape-first so the odd formats surface at the top.
  4. Pre-fill each case's expected year by piping it through the `year_normalize`
     CLI (the real parser), then flag *suspected misses*: rows where the parser
     returned 0 but a plausible year is still visible in the string (e.g. a year
     embedded in a longer run like "rec20070501", a 2-digit year, or "199x").
     Suspects sort to the top — these are the parser gaps worth fixing.

Writes raw \t expected \t suspect \t shape \t count; the test reads the first two
columns. Run after building the CLI:

    cmake --build build                       # builds year_normalize
    uv run scripts/extract_dates.py ~/Music -o - \
        | uv run scripts/build_year_cases.py -

Or from a saved dump:  uv run scripts/build_year_cases.py dates.tsv
Focus the review:       uv run scripts/build_year_cases.py dates.tsv --misses-only

Apply loop — turn real misses into permanent tests:
  1. `make misses DIR=~/Music`  (generates suspects, opens them in visidata)
  2. For each genuine suspect=1 row, note the year a human reads.
  3. Add a QTest::newRow("...") << "<raw>" << <year>; line to normalizes_data()
     in tests/YearParserTest.cpp — it fails until the parser is fixed.
  4. Fix normalizeYear in src/YearParser.cpp; rerun `make test` until green.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLI = REPO_ROOT / "build" / "year_normalize"
DEFAULT_OUT = REPO_ROOT / "tests" / "year_cases.tsv"
PLAIN_YEAR_SHAPE = "DDDD"

# Plausible release-year window — mirrors kMinYear/kMaxYear in src/YearParser.cpp.
MIN_YEAR, MAX_YEAR = 1860, 2100
_YEAR_RE = re.compile(r"(?:18|19|20)\d\d")


def has_plausible_year(s: str) -> bool:
    """True if a plausible 4-digit year appears anywhere in s (even embedded)."""
    return any(MIN_YEAR <= int(m) <= MAX_YEAR for m in _YEAR_RE.findall(s))


def shape(s: str) -> str:
    """Structural signature: digit->D, letter->L, everything else verbatim."""
    return "".join("D" if c.isdigit() else "L" if c.isalpha() else c for c in s)


def read_values(source) -> Counter:
    """Count distinct date strings across all non-path columns of a dates.tsv."""
    counts: Counter = Counter()
    handle = sys.stdin if source == "-" else open(source, encoding="utf-8")
    try:
        header = next(handle, "").rstrip("\n").split("\t")
        # Every column except the leading 'path' carries date values.
        for line in handle:
            cells = line.rstrip("\n").split("\t")[1:]
            for cell in cells:
                for value in cell.split(";"):   # multi-valued tags
                    if not value:
                        continue
                    if "\t" in value or "\n" in value:
                        continue   # can't round-trip through the TSV/CLI protocol
                    counts[value] += 1
    finally:
        if handle is not sys.stdin:
            handle.close()
    return counts


def select(counts: Counter, per_bucket: int, plain_keep: int):
    """Sample examples per shape, rarest shape first. Returns [(raw, shape, count)]."""
    buckets: dict[str, list[tuple[str, int]]] = {}
    for raw, n in counts.items():
        buckets.setdefault(shape(raw), []).append((raw, n))

    chosen: list[tuple[str, str, int]] = []
    for sig, items in buckets.items():
        items.sort(key=lambda rc: (-rc[1], rc[0]))   # most common example first
        keep = plain_keep if sig == PLAIN_YEAR_SHAPE else per_bucket
        for raw, n in items[:keep]:
            chosen.append((raw, sig, n))

    # Rarest shapes first (edge cases up top), then by shape, then by value.
    bucket_total = {sig: sum(n for _, n in items) for sig, items in buckets.items()}
    chosen.sort(key=lambda c: (bucket_total[c[1]], c[1], c[0]))
    return chosen


def prefill(raws: list[str], cli: Path) -> dict[str, int]:
    """Run each raw value through the parser CLI; map raw -> year."""
    proc = subprocess.run([str(cli)], input="\n".join(raws),
                          capture_output=True, text=True, check=True)
    out: dict[str, int] = {}
    for line in proc.stdout.splitlines():
        raw, _, year = line.rpartition("\t")
        out[raw] = int(year) if year.lstrip("-").isdigit() else 0
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dates", help="dates.tsv from extract_dates.py ('-' for stdin)")
    ap.add_argument("-o", "--output", type=Path, default=DEFAULT_OUT,
                    help=f"fixture to write (default: {DEFAULT_OUT})")
    ap.add_argument("--cli", type=Path, default=DEFAULT_CLI,
                    help=f"year_normalize binary (default: {DEFAULT_CLI})")
    ap.add_argument("--per-bucket", type=int, default=4,
                    help="max examples per shape (default: 4)")
    ap.add_argument("--plain-keep", type=int, default=2,
                    help="examples to keep of the bare-year shape (default: 2)")
    ap.add_argument("--misses-only", action="store_true",
                    help="emit only suspected misses (suspect == 1)")
    args = ap.parse_args()

    if not args.cli.exists():
        print(f"error: {args.cli} not found — build it first: cmake --build build",
              file=sys.stderr)
        return 2

    counts = read_values(args.dates)
    if not counts:
        print("error: no date values found in input", file=sys.stderr)
        return 1

    chosen = select(counts, args.per_bucket, args.plain_keep)
    years = prefill([raw for raw, _, _ in chosen], args.cli)

    # A suspected miss: the parser gave up (0) yet a plausible year is visible —
    # exactly the rows worth hardcoding as test cases and fixing in normalizeYear.
    rows = []
    for raw, sig, n in chosen:
        expected = years.get(raw, 0)
        suspect = int(expected == 0 and has_plausible_year(raw))
        rows.append((raw, expected, suspect, sig, n))
    if args.misses_only:
        rows = [r for r in rows if r[2]]
    # Suspects first; select() already ordered rarest-shape-first, and Python's
    # sort is stable, so that ordering is preserved within each group.
    rows.sort(key=lambda r: not r[2])

    suspects = sum(r[2] for r in rows)
    with open(args.output, "w", encoding="utf-8") as out:
        out.write("# raw\texpected\tsuspect\tshape\tcount  "
                  "(expected pre-filled by year_normalize; suspect=1 means a year is "
                  "visible but was missed — audit & edit)\n")
        for raw, expected, suspect, sig, n in rows:
            out.write(f"{raw}\t{expected}\t{suspect}\t{sig}\t{n}\n")

    print(f"{len(counts)} distinct value(s) across {len(set(map(shape, counts)))} "
          f"shape(s) -> {len(rows)} case(s) ({suspects} suspected miss(es)) "
          f"written to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
