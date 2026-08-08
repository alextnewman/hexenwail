#!/usr/bin/env bash
# Assembles the static PWA artifact (the "dist" directory) from the built
# Emscripten output plus the web/ PWA shell.
#
# Single source of truth shared by the PR "PWA / WebAssembly" check (which
# validates but does not publish it) and the GitHub Pages deployment
# workflow (which uploads it), so the two never drift apart.
#
# Usage: scripts/wasm-assemble-artifact.sh [dist-dir] [build-version]
set -euo pipefail

cd "$(dirname "$0")/.."

DIST_DIR="${1:-dist}"
BUILD_VERSION="${2:-${GITHUB_SHA:-$(git rev-parse --verify HEAD)}}"
BUILD_BIN="engine/build/bin"

if [[ ! "$BUILD_VERSION" =~ ^[A-Za-z0-9._-]+$ ]]; then
	echo "Invalid PWA build version: $BUILD_VERSION" >&2
	exit 1
fi

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

cp -r web/. "$DIST_DIR/"
sed -i.bak "s/__HEXENWAIL_BUILD_VERSION__/$BUILD_VERSION/g" "$DIST_DIR/sw.js"
rm -f "$DIST_DIR/sw.js.bak"

cp "$BUILD_BIN/hexenwail.js" "$DIST_DIR/"
cp "$BUILD_BIN/hexenwail.wasm" "$DIST_DIR/"
if [ -f "$BUILD_BIN/hexenwail.data" ]; then
	cp "$BUILD_BIN/hexenwail.data" "$DIST_DIR/"
fi
if [ -f "$BUILD_BIN/hexenwail.worker.js" ]; then
	cp "$BUILD_BIN/hexenwail.worker.js" "$DIST_DIR/"
fi
# The Emscripten-generated .html shell is only useful for local/debug
# testing directly against the emcc shell template; the PWA serves web/
# (index.html, manifest, service worker) as the real entry point.
cp "$BUILD_BIN/hexenwail.html" "$DIST_DIR/engine-shell-debug.html"

echo "Assembled PWA artifact in $DIST_DIR:"
ls -la "$DIST_DIR"
