# WASM/Emscripten development shell
# Usage: nix-shell shell-wasm.nix
#
# Superseded by the flake's `nix develop`, which provides the same toolchain.
# Kept for non-flake users.
#
# Note: the nixpkgs Emscripten is not the pinned emsdk CI uses
# (see .github/workflows/ci.yml). For a shipping-equivalent build, install
# that emsdk release directly and run scripts/wasm-build.sh.

{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    emscripten
    cmake
    nodejs
    python3
  ];

  shellHook = ''
    echo "Emscripten development environment for Hexenwail"
    echo "=========================================="
    echo ""
    echo "Build (software renderer, the shipping default):"
    echo "  ./scripts/wasm-build.sh software"
    echo ""
    echo "Build the retained WebGL2 renderer:"
    echo "  ./scripts/wasm-build.sh webgl2 engine/build-webgl2"
    echo ""
    echo "Assemble and validate the static PWA artifact:"
    echo "  ./scripts/wasm-assemble-artifact.sh dist"
    echo "  ./scripts/wasm-validate-artifact.sh dist"
    echo ""
  '';
}
