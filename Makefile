# YouHexen2 Build System - Convenience Makefile
#
# This engine targets Emscripten only: engine/CMakeLists.txt fails the
# configure step outright on any other toolchain. There is no native
# Linux/Windows binary, so the former nix-linux/nix-win64/nix-release
# targets have been removed. See docs/web/ARCHITECTURE.md.

.PHONY: help build build-software build-nitro dist test clean

help:
	@echo "YouHexen2 Build Targets"
	@echo "======================"
	@echo ""
	@echo "Requires emcmake/emmake on PATH:"
	@echo "  source \"\$$EMSDK/emsdk_env.sh\""
	@echo ""
	@echo "  make build          - Build WebGlideNitro, the primary renderer"
	@echo "  make build-software - Build the parked software reference on WebGPU"
	@echo "  make build-nitro    - Build WebGlideNitro, the native WebGPU renderer"
	@echo "  make dist           - Assemble and validate the static PWA artifact"
	@echo "  make test           - Run the PWA shell tests"
	@echo "  make clean          - Clean all build artifacts"
	@echo ""

# Default target
all: help

# The primary WebGlideNitro renderer.
build: build-nitro

# Parked classic 8bpp software renderer presented through WebGPU.
build-software:
	./scripts/wasm-build.sh software

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
