#!/usr/bin/env bash
# Configures and builds the Emscripten/WebAssembly hexenwail client.
#
# Single source of truth for the CMake configure + build invocation used by
# both the PR "PWA / WebAssembly" check and the GitHub Pages deployment
# workflow, so the two never drift apart.
#
# Usage: wasm-build.sh [renderer] [build-dir]
#   renderer   software (default) | webgl2 | webgpu | nitro
#              webgpu is the software rasterizer with its WebGPU presenter;
#              nitro is WebGlideNitro, the native WebGPU renderer.
#   build-dir  defaults to engine/build
#
# Requires: emcmake/emmake on PATH (i.e. `source "$EMSDK/emsdk_env.sh"` first).
set -euo pipefail

cd "$(dirname "$0")/.."
repo_root="$PWD"

renderer="${1:-software}"
build_dir="${2:-engine/build}"
presenter="webgl2"

case "$renderer" in
webgpu)
	# The software rasterizer scanned out through a WebGPU presenter.
	renderer="software"
	presenter="webgpu"
	;;
nitro)
	# WebGlideNitro: a native WebGPU renderer, no software framebuffer.
	renderer="webgpu"
	;;
esac

mkdir -p "$build_dir"
cd "$build_dir"

emcmake cmake \
	-DCMAKE_BUILD_TYPE=Release \
	-DWEB_RENDERER="$renderer" \
	-DWEB_PRESENTER="$presenter" \
	"$repo_root/engine"

emmake make -j"$(nproc)"
