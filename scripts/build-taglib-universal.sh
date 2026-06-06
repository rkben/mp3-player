#!/usr/bin/env bash
# Build a universal (arm64+x86_64) static TagLib for the macOS universal app build.
#
#   scripts/build-taglib-universal.sh           # -> <repo>/.deps/taglib-universal
#   TAGLIB_PREFIX=/path scripts/build-taglib-universal.sh
#
# Official Qt is already universal and bundles FFmpeg, so TagLib is the only dep that
# isn't universal out of the box (Homebrew installs host-arch only). We build it
# static so it links straight into the app — nothing extra to bundle. Header-only
# utf8cpp comes from Homebrew (arch-independent). Re-run only when bumping TagLib.
#
# Artifacts stay inside the repo (.deps/, gitignored) — never in $HOME — so builds
# are self-contained and reproducible on CI.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${DEPS_DIR:-$ROOT/.deps}"

TAGLIB_VERSION="${TAGLIB_VERSION:-2.3}"            # keep in step with Homebrew's taglib
PREFIX="${TAGLIB_PREFIX:-$DEPS/taglib-universal}"
SRC="$DEPS/src/taglib-$TAGLIB_VERSION"

mkdir -p "$DEPS/src"
if [[ ! -d "$SRC" ]]; then
    echo "==> Fetching TagLib $TAGLIB_VERSION"
    curl -fsSL "https://github.com/taglib/taglib/releases/download/v$TAGLIB_VERSION/taglib-$TAGLIB_VERSION.tar.gz" \
        | tar xz -C "$DEPS/src"
fi

echo "==> Building universal static TagLib -> $PREFIX"
cmake -B "$SRC/build-uni" -S "$SRC" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH=/opt/homebrew \
    -DUTF8CPP_INCLUDE_DIR=/opt/homebrew/include
cmake --build "$SRC/build-uni" -j
cmake --install "$SRC/build-uni"

echo "==> Done: $(lipo -archs "$PREFIX/lib/libtag.a") in $PREFIX/lib/libtag.a"
