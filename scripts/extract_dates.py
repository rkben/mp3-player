#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = [
#     "pytaglib>=3.0",
#     "tqdm>=4.66",
# ]
# ///
"""Walk a directory and dump each track's raw date/year tags via TagLib.

Reads the date-bearing tags (DATE, ORIGINALDATE, YEAR, ORIGINALYEAR, plus a few
container-specific aliases) straight off every audio file and writes one TSV row
per file. Intended as an offline probe of what date strings actually live in a
library "in the wild" — the messy inputs the player's year normaliser has to cope
with (ISO timestamps, ranges, "May 2007", YYYYMMDD, …).

Run with uv (no install needed; deps are declared inline above):

    uv run scripts/extract_dates.py ~/Music
    uv run scripts/extract_dates.py ~/Music -o dates.tsv

Or make it executable and run directly: ./scripts/extract_dates.py ~/Music
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import taglib
from tqdm import tqdm

# Same set the player scans (MusicLibrary / AudioFormats.h), lower-cased.
AUDIO_EXTS = {
    ".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".m4b", ".aac",
    ".wav", ".aiff", ".aif", ".wma", ".ape", ".wv", ".mpc", ".mp2",
    ".spx", ".tta", ".dsf", ".ac3",
}

# Tag keys that carry a date or year, in priority order. TagLib normalises tag
# names to upper-case across formats, so these match ID3, Vorbis comments, MP4…
DATE_KEYS = ["DATE", "ORIGINALDATE", "YEAR", "ORIGINALYEAR", "RELEASEDATE", "TDRC"]


def collect_audio(root: Path) -> list[Path]:
    """Return audio files under root, sorted, case-insensitive on extension."""
    return [p for p in sorted(root.rglob("*"))
            if p.is_file() and p.suffix.lower() in AUDIO_EXTS]


def date_fields(path: Path) -> dict[str, str]:
    """Return the date-bearing tags found on one file ({} if unreadable)."""
    try:
        with taglib.File(str(path)) as f:
            tags = f.tags
    except Exception as exc:  # noqa: BLE001 - report and keep walking
        print(f"warning: could not read {path}: {exc}", file=sys.stderr)
        return {}
    out: dict[str, str] = {}
    for key in DATE_KEYS:
        values = tags.get(key)
        if values:
            # Tags are lists; join multi-valued frames with ';'.
            out[key] = ";".join(values)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("directory", type=Path, help="directory to walk recursively")
    ap.add_argument("-o", "--output", type=Path, default=Path("dates.tsv"),
                    help="output TSV file (default: dates.tsv; '-' for stdout)")
    args = ap.parse_args()

    if not args.directory.is_dir():
        print(f"error: not a directory: {args.directory}", file=sys.stderr)
        return 2

    to_stdout = str(args.output) == "-"
    out = sys.stdout if to_stdout else open(args.output, "w", encoding="utf-8")
    columns = ["path", *DATE_KEYS]
    paths = collect_audio(args.directory)
    found = 0
    try:
        out.write("\t".join(columns) + "\n")
        # Progress to stderr so it never mixes into a piped TSV on stdout.
        for path in tqdm(paths, unit="file", desc="reading tags", file=sys.stderr):
            fields = date_fields(path)
            if fields:
                found += 1
            row = [str(path)] + [fields.get(k, "") for k in DATE_KEYS]
            out.write("\t".join(row) + "\n")
    finally:
        if not to_stdout:
            out.close()

    dest = "stdout" if to_stdout else str(args.output)
    print(f"scanned {len(paths)} file(s), {found} with date tags -> {dest}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
