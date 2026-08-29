#!/usr/bin/env bash
# Configures and builds the Emscripten/WebAssembly hexenwail client.
#
# Single source of truth for the CMake configure + build invocation used by
# both the PR "PWA / WebAssembly" check and the GitHub Pages deployment
# workflow, so the two never drift apart.
#
# Usage: wasm-build.sh [renderer] [build-dir]
#   renderer   nitro (default) | software
#              software uses the WebGPU presenter; nitro is WebGlideNitro.
#   build-dir  defaults to engine/build
#
# Requires: emcmake/emmake on PATH (i.e. `source "$EMSDK/emsdk_env.sh"` first).
set -euo pipefail

cd "$(dirname "$0")/.."
repo_root="$PWD"

renderer="${1:-nitro}"
if [ -n "${2:-}" ]; then
	build_dir="$2"
elif [ "$renderer" = "nitro" ]; then
	build_dir="engine/build-nitro"
else
	build_dir="engine/build"
fi
case "$renderer" in
nitro)
	# WebGlideNitro: a native WebGPU renderer, no software framebuffer.
	renderer="webgpu"
	;;
software)
	;;
*)
	echo "Unknown renderer '$renderer' (expected 'nitro' or 'software')" >&2
	exit 2
	;;
esac

mkdir -p "$build_dir"
cd "$build_dir"

emcmake cmake \
	-DCMAKE_BUILD_TYPE=Release \
	-DWEB_RENDERER="$renderer" \
	"$repo_root/engine"

emmake make -j"$(nproc)"
