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
# The optional WebGlide GPU bundle. engine/CMakeLists.txt renames the
# Emscripten outputs by renderer (see docs/web/ARCHITECTURE.md and the
# WEB_RENDERER contract), so a webgl2 build lands next to the software
# one under the distinct basename "hexenwail-webglide" -- the CMake
# option is still spelled "webgl2" but the user-facing renderer is
# WebGlide, an experimental GPU pipeline inspired by mid-90s 3Dfx Glide
# hardware, deliberately distinct from the old "maximum GL2" profile.
# Do NOT rename these after the fact: the Emscripten-generated .js looks
# up its own .wasm sibling by basename, and a rename here breaks the
# runtime fetch.
GL_BUILD_BIN="${GL_BUILD_BIN:-engine/build-webgl2/bin}"
# Optional WebGPU presenter feasibility bundle. This still contains the
# software rasterizer; it is not the WebGlideNitro renderer.
GPU_BUILD_BIN="${GPU_BUILD_BIN:-engine/build-webgpu/bin}"
# Primary WebGlideNitro bundle: the native WebGPU renderer, which has no
# software framebuffer and no GL context at all. Built with
# `./scripts/wasm-build.sh nitro engine/build-nitro`.
NITRO_BUILD_BIN="${NITRO_BUILD_BIN:-engine/build-nitro/bin}"

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

# The WebGlide experimental GPU bundle ships alongside the software one
# when the second CMake configuration has been built. It is optional here
# because it is retained only as an experimental reference. CI builds every
# configuration, while a local artifact may omit this one.
if [ -d "$GL_BUILD_BIN" ]; then
	cp "$GL_BUILD_BIN/hexenwail-webglide.js" "$DIST_DIR/"
	cp "$GL_BUILD_BIN/hexenwail-webglide.wasm" "$DIST_DIR/"
	if [ -f "$GL_BUILD_BIN/hexenwail-webglide.data" ]; then
		cp "$GL_BUILD_BIN/hexenwail-webglide.data" "$DIST_DIR/"
	fi
	if [ -f "$GL_BUILD_BIN/hexenwail-webglide.worker.js" ]; then
		cp "$GL_BUILD_BIN/hexenwail-webglide.worker.js" "$DIST_DIR/"
	fi
	if [ -f "$GL_BUILD_BIN/hexenwail-webglide.html" ]; then
		cp "$GL_BUILD_BIN/hexenwail-webglide.html" "$DIST_DIR/engine-shell-debug-webglide.html"
	fi
else
	echo "notice: WebGlide GPU build directory not found at $GL_BUILD_BIN;" \
		"assembling the PWA artifact without WebGlide." \
		"Run './scripts/wasm-build.sh webgl2 engine/build-webgl2' before" \
		"re-running this script to ship the experimental WebGlide bundle too."
fi

if [ -d "$GPU_BUILD_BIN" ]; then
	cp "$GPU_BUILD_BIN/hexenwail-webgpu.js" "$DIST_DIR/"
	cp "$GPU_BUILD_BIN/hexenwail-webgpu.wasm" "$DIST_DIR/"
	if [ -f "$GPU_BUILD_BIN/hexenwail-webgpu.data" ]; then
		cp "$GPU_BUILD_BIN/hexenwail-webgpu.data" "$DIST_DIR/"
	fi
	if [ -f "$GPU_BUILD_BIN/hexenwail-webgpu.worker.js" ]; then
		cp "$GPU_BUILD_BIN/hexenwail-webgpu.worker.js" "$DIST_DIR/"
	fi
	if [ -f "$GPU_BUILD_BIN/hexenwail-webgpu.html" ]; then
		cp "$GPU_BUILD_BIN/hexenwail-webgpu.html" "$DIST_DIR/engine-shell-debug-webgpu.html"
	fi
else
	echo "notice: WebGPU presenter build directory not found at $GPU_BUILD_BIN;" \
		"continuing without the optional feasibility bundle."
fi

if [ -d "$NITRO_BUILD_BIN" ]; then
	cp "$NITRO_BUILD_BIN/hexenwail-nitro.js" "$DIST_DIR/"
	cp "$NITRO_BUILD_BIN/hexenwail-nitro.wasm" "$DIST_DIR/"
	if [ -f "$NITRO_BUILD_BIN/hexenwail-nitro.data" ]; then
		cp "$NITRO_BUILD_BIN/hexenwail-nitro.data" "$DIST_DIR/"
	fi
	if [ -f "$NITRO_BUILD_BIN/hexenwail-nitro.worker.js" ]; then
		cp "$NITRO_BUILD_BIN/hexenwail-nitro.worker.js" "$DIST_DIR/"
	fi
	if [ -f "$NITRO_BUILD_BIN/hexenwail-nitro.html" ]; then
		cp "$NITRO_BUILD_BIN/hexenwail-nitro.html" "$DIST_DIR/engine-shell-debug-nitro.html"
	fi
else
	echo "error: primary WebGlideNitro build directory not found at $NITRO_BUILD_BIN." >&2
	exit 1
fi

echo "Assembled PWA artifact in $DIST_DIR:"
ls -la "$DIST_DIR"
