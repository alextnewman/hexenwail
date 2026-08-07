#!/usr/bin/env bash
# Configures and builds the Emscripten/WebAssembly hexenwail client.
#
# Single source of truth for the CMake configure + build invocation used by
# both the PR "PWA / WebAssembly" check and the GitHub Pages deployment
# workflow, so the two never drift apart.
#
# Requires: emcmake/emmake on PATH (i.e. `source "$EMSDK/emsdk_env.sh"` first).
set -euo pipefail

cd "$(dirname "$0")/.."

mkdir -p engine/build
cd engine/build

emcmake cmake \
	-DCMAKE_BUILD_TYPE=Release \
	-DUSE_CODEC_VORBIS=OFF \
	-DUSE_ALSA=OFF \
	-DUSE_SDL3_STATIC=ON \
	..

emmake make -j"$(nproc)"
