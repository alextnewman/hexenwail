# Hexenwail Build System - Convenience Makefile
#
# This engine targets Emscripten only: engine/CMakeLists.txt fails the
# configure step outright on any other toolchain. There is no native
# Linux/Windows binary, so the former nix-linux/nix-win64/nix-release
# targets have been removed. See docs/web/ARCHITECTURE.md.

.PHONY: help build build-webgl2 build-webgpu dist test clean

help:
	@echo "Hexenwail Build Targets"
	@echo "======================"
	@echo ""
	@echo "Requires emcmake/emmake on PATH:"
	@echo "  source \"\$$EMSDK/emsdk_env.sh\""
	@echo ""
	@echo "  make build          - Build the WebAssembly client (software renderer)"
	@echo "  make build-webgl2   - Build the retained WebGL2 renderer"
	@echo "  make build-webgpu   - Build the WebGPU software-presenter preview"
	@echo "  make dist           - Assemble and validate the static PWA artifact"
	@echo "  make test           - Run the PWA shell tests"
	@echo "  make clean          - Clean all build artifacts"
	@echo ""

# Default target
all: help

# The shipping configuration: classic 8bpp software renderer presented on
# an accelerated canvas.
build:
	./scripts/wasm-build.sh software

# The WebGL2 renderer is retained and must keep compiling.
build-webgl2:
	./scripts/wasm-build.sh webgl2 engine/build-webgl2

# Target-feasibility path: unchanged software rasterizer, WebGPU scan-out.
# This is intentionally distinct from the future WebGlideNitro renderer.
build-webgpu:
	./scripts/wasm-build.sh webgpu engine/build-webgpu

# Static PWA artifact, the same one CI validates and Pages deploys.
dist: build
	./scripts/wasm-assemble-artifact.sh dist
	./scripts/wasm-validate-artifact.sh dist

test:
	npm test

clean:
	rm -rf engine/build engine/build-* dist result
	@echo "Clean complete"
