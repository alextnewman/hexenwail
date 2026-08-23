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

# The WebGlide experimental GPU bundle is optional here for the same
# reason it is optional in wasm-assemble-artifact.sh: a local `make dist`
# only builds the shipping software renderer, so a fresh clone must be
# able to assemble and validate a software-only artifact without failure.
# CI builds both renderers, and .github/actions/wasm-build enforces that
# both bundles reached the artifact -- that is where the CI-side "must
# ship both" contract lives, not here. When one half of the pair is
# present without the other, though, that is always a broken build.
gl_js="$DIST_DIR/hexenwail-webglide.js"
gl_wasm="$DIST_DIR/hexenwail-webglide.wasm"
if [ -s "$gl_js" ] && [ -s "$gl_wasm" ]; then
	echo "OK (optional present): $gl_js"
	echo "OK (optional present): $gl_wasm"
	for optional in hexenwail-webglide.data hexenwail-webglide.worker.js engine-shell-debug-webglide.html; do
		if [ -s "$DIST_DIR/$optional" ]; then
			echo "OK (optional present): $DIST_DIR/$optional"
		fi
	done
elif [ -s "$gl_js" ] || [ -s "$gl_wasm" ]; then
	echo "INVALID: only one half of the WebGlide bundle is present" \
		"(hexenwail-webglide.js and hexenwail-webglide.wasm must ship together)" >&2
	missing=1
else
	echo "info: optional artifact not present: $DIST_DIR/hexenwail-webglide.js" \
		"(WebGlide GPU bundle; software-only artifact)"
fi

# The WebGPU presenter preview follows the same optional-pair rule.
gpu_js="$DIST_DIR/hexenwail-webgpu.js"
gpu_wasm="$DIST_DIR/hexenwail-webgpu.wasm"
if [ -s "$gpu_js" ] && [ -s "$gpu_wasm" ]; then
	echo "OK (optional present): $gpu_js"
	echo "OK (optional present): $gpu_wasm"
	for optional in hexenwail-webgpu.data hexenwail-webgpu.worker.js engine-shell-debug-webgpu.html; do
		if [ -s "$DIST_DIR/$optional" ]; then
			echo "OK (optional present): $DIST_DIR/$optional"
		fi
	done
elif [ -s "$gpu_js" ] || [ -s "$gpu_wasm" ]; then
	echo "INVALID: only one half of the WebGPU presenter bundle is present" \
		"(hexenwail-webgpu.js and hexenwail-webgpu.wasm must ship together)" >&2
	missing=1
else
	echo "info: optional artifact not present: $DIST_DIR/hexenwail-webgpu.js" \
		"(WebGPU presenter feasibility bundle)"
fi

# WebGlideNitro, the native WebGPU renderer, follows the same rule again.
nitro_js="$DIST_DIR/hexenwail-nitro.js"
nitro_wasm="$DIST_DIR/hexenwail-nitro.wasm"
if [ -s "$nitro_js" ] && [ -s "$nitro_wasm" ]; then
	echo "OK (optional present): $nitro_js"
	echo "OK (optional present): $nitro_wasm"
	for optional in hexenwail-nitro.data hexenwail-nitro.worker.js engine-shell-debug-nitro.html; do
		if [ -s "$DIST_DIR/$optional" ]; then
			echo "OK (optional present): $DIST_DIR/$optional"
		fi
	done
elif [ -s "$nitro_js" ] || [ -s "$nitro_wasm" ]; then
	echo "INVALID: only one half of the WebGlideNitro bundle is present" \
		"(hexenwail-nitro.js and hexenwail-nitro.wasm must ship together)" >&2
	missing=1
else
	echo "info: optional artifact not present: $DIST_DIR/hexenwail-nitro.js" \
		"(WebGlideNitro native WebGPU bundle)"
fi

if [ "$missing" -ne 0 ]; then
	echo "PWA artifact validation FAILED: required files are missing." >&2
	exit 1
fi

echo "PWA artifact validation passed."
