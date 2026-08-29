#!/usr/bin/env bash
# Validates that the assembled PWA "dist" artifact contains everything the
# offline-installable client needs: the Emscripten-generated runtime plus
# the PWA shell (manifest + service worker). Used by the PR "PWA /
# WebAssembly" check to catch artifact-assembly regressions before merge,
# without actually deploying anything.
#
# Usage: scripts/wasm-validate-artifact.sh [dist-dir]
set -euo pipefail

cd "$(dirname "$0")/.."

DIST_DIR="${1:-dist}"
missing=0

require() {
	local path="$1"
	if [ ! -s "$DIST_DIR/$path" ]; then
		echo "MISSING (or empty): $DIST_DIR/$path" >&2
		missing=1
	else
		echo "OK: $DIST_DIR/$path"
	fi

}

require "hexenwail.js"
require "hexenwail.wasm"
require "hexenwail-nitro.js"
require "hexenwail-nitro.wasm"
require "engine-shell-debug.html"
require "index.html"
require "manifest.webmanifest"
require "sw.js"

if grep -q '__HEXENWAIL_BUILD_VERSION__' "$DIST_DIR/sw.js"; then
	echo "INVALID: $DIST_DIR/sw.js still contains the build-version placeholder" >&2
	missing=1
fi

# .data / .worker.js are optional depending on build options (e.g. preload
# file packaging), so only report their presence informationally.
for optional in hexenwail.data hexenwail.worker.js; do
	if [ -s "$DIST_DIR/$optional" ]; then
		echo "OK (optional present): $DIST_DIR/$optional"
	else
		echo "info: optional artifact not present: $DIST_DIR/$optional"
	fi
done

# WebGlideNitro is required above; this section reports its optional sidecars.
nitro_js="$DIST_DIR/hexenwail-nitro.js"
nitro_wasm="$DIST_DIR/hexenwail-nitro.wasm"
if [ -s "$nitro_js" ] && [ -s "$nitro_wasm" ]; then
	echo "OK (primary renderer): $nitro_js"
	echo "OK (primary renderer): $nitro_wasm"
	for optional in hexenwail-nitro.data hexenwail-nitro.worker.js engine-shell-debug-nitro.html; do
		if [ -s "$DIST_DIR/$optional" ]; then
			echo "OK (optional present): $DIST_DIR/$optional"
		fi
	done
elif [ -s "$nitro_js" ] || [ -s "$nitro_wasm" ]; then
	echo "INVALID: only one half of the WebGlideNitro bundle is present" \
		"(hexenwail-nitro.js and hexenwail-nitro.wasm must ship together)" >&2
	missing=1
fi

if [ "$missing" -ne 0 ]; then
	echo "PWA artifact validation FAILED: required files are missing." >&2
	exit 1
fi

echo "PWA artifact validation passed."
