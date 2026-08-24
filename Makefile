# Hexenwail Build System - Convenience Makefile
#
# This engine targets Emscripten only: engine/CMakeLists.txt fails the
# configure step outright on any other toolchain. There is no native
# Linux/Windows binary, so the former nix-linux/nix-win64/nix-release
# targets have been removed. See docs/web/ARCHITECTURE.md.

.PHONY: help build build-software build-webgl2 build-webgpu build-nitro dist test clean

help:
	@echo "Hexenwail Build Targets"
	@echo "======================"
	@echo ""
	@echo "Requires emcmake/emmake on PATH:"
	@echo "  source \"\$$EMSDK/emsdk_env.sh\""
	@echo ""
	@echo "  make build          - Build WebGlideNitro, the primary renderer"
	@echo "  make build-software - Build the parked software reference"
	@echo "  make build-webgl2   - Build the retained WebGL2 renderer"
	@echo "  make build-webgpu   - Build the WebGPU software-presenter preview"
	@echo "  make build-nitro    - Build WebGlideNitro, the native WebGPU renderer"
	@echo "  make dist           - Assemble and validate the static PWA artifact"
	@echo "  make test           - Run the PWA shell tests"
	@echo "  make clean          - Clean all build artifacts"
	@echo ""

# Default target
all: help

# The primary WebGlideNitro renderer.
build: build-nitro

# Parked classic 8bpp software renderer presented on an accelerated canvas.
build-software:
	./scripts/wasm-build.sh software

# The WebGL2 renderer is retained and must keep compiling.
build-webgl2:
	./scripts/wasm-build.sh webgl2 engine/build-webgl2

# Target-feasibility path: unchanged software rasterizer, WebGPU scan-out.
# This is intentionally distinct from WebGlideNitro below.
build-webgpu:
	./scripts/wasm-build.sh webgpu engine/build-webgpu

# WebGlideNitro: a native WebGPU renderer that builds its own scene geometry.
# See docs/web/WEBGLIDE_NITRO.md.
build-nitro:
	./scripts/wasm-build.sh nitro engine/build-nitro

# Static PWA artifact, the same one CI validates and Pages deploys.
dist: build build-software
	./scripts/wasm-assemble-artifact.sh dist
	./scripts/wasm-validate-artifact.sh dist

test:
	npm test

clean:
	rm -rf engine/build engine/build-* dist result
	@echo "Clean complete"
