{
  description = "YouHexen2 - a web-native uHexen2 port for installed iOS PWAs";

  nixConfig = {
    extra-substituters = [ "https://hexenwail.cachix.org" ];
    extra-trusted-public-keys = [ "hexenwail.cachix.org-1:8p4Jk7hUQz7PC4eqiqBl0RtorLGO9QosIaKfRa2EgPE=" ];
  };

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  # This engine targets Emscripten only.  engine/CMakeLists.txt fails the
  # configure step outright on any other toolchain, so the former `nixos`,
  # `linux-fhs`, `win64` and `release` outputs -- all of which built the
  # native `glhexen2` binary -- cannot work and have been removed along
  # with `apps.default`.
  #
  # CI, the Pages deployment and the release workflow do NOT use this
  # flake: they build with a pinned emsdk via .github/actions/wasm-build
  # and scripts/wasm-*.sh, which is the source of truth for a shipping
  # build.  What follows is developer convenience only.
  # See docs/web/ARCHITECTURE.md.
  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachSystem [ "x86_64-linux" ] (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Version: extracted from engine/hexen2/quakedef.h HW_BASE_VERSION
        version = let
          lines = builtins.split "\n" (builtins.readFile ./engine/hexen2/quakedef.h);
          matches = builtins.filter (x: builtins.isString x &&
            builtins.match ".*HW_BASE_VERSION.*" x != null) lines;
          line = builtins.head matches;
          parts = builtins.split "\"" line;
          strs = builtins.filter builtins.isString parts;
        in builtins.elemAt strs 1;

        # Source filter: exclude non-build files to improve cache hits
        filteredSrc = pkgs.lib.cleanSourceWith {
          src = ./.;
          filter = path: type:
            let baseName = baseNameOf (toString path);
            in !(
              baseName == ".beads" ||
              baseName == ".github" ||
              baseName == "history" ||
              baseName == "docs" ||
              baseName == "flatpak" ||
              baseName == "gamecode" ||
              (type == "regular" && (
                pkgs.lib.hasSuffix ".md" baseName ||
                baseName == "release.sh" ||
                baseName == ".gitignore"
              ))
            );
        };

      in
      {
        packages = {
          # WebAssembly / Emscripten build.
          #
          # Best effort: nixpkgs' emscripten is not the pinned emsdk that
          # CI uses, and a pure `nix build` has no network, so it cannot
          # fetch an Emscripten port.  The shipping build uses exactly one
          # (ogg/vorbis, for OGG music), so this derivation configures with
          # -DUSE_CODEC_VORBIS=OFF and produces a client that plays
          # wav/mp3/flac music but not ogg.  It is a build-health check,
          # not a shipping artifact: for that, use `nix develop` (below) or
          # the emsdk directly and run scripts/wasm-build.sh, which is what
          # CI does.
          wasm = pkgs.stdenv.mkDerivation {
            pname = "hexenwail-wasm";
            inherit version;

            src = filteredSrc;

            nativeBuildInputs = with pkgs; [
              emscripten
              cmake
              nodejs
            ];

            preConfigure = ''
              export EM_CACHE="''${EM_CACHE:-$TMPDIR/.emcache}"
              export EM_CONFIG="''${EM_CONFIG:-$TMPDIR/.emscripten}"
            '';

            # Use Emscripten's CMake toolchain. This package explicitly builds
            # the software renderer with WebGPU presentation.
            # USE_CODEC_VORBIS is off here because a
            # pure build cannot fetch the Emscripten vorbis port.
            configurePhase = ''
              runHook preConfigure

              mkdir -p engine/build
              cd engine/build
              emcmake cmake \
                -DCMAKE_BUILD_TYPE=Release \
                -DWEB_RENDERER=software \
                -DUSE_CODEC_VORBIS=OFF \
                ..

              runHook postConfigure
            '';

            buildPhase = ''
              runHook preBuild
              emmake make -j''${NIX_BUILD_CORES:-1}
              runHook postBuild
            '';

            # Delegate to the same assembler CI and Pages use, so the flake
            # cannot drift from the artifact that actually ships.  It expects
            # to run from the repo root and reads engine/build/bin.
            installPhase = ''
              runHook preInstall

              cd ../..
              bash scripts/wasm-assemble-artifact.sh "$out" "${version}"

              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "YouHexen2 - installable WebAssembly PWA build";
              longDescription = ''
                uHexen2 built for the browser as an installable PWA, using the
                classic 8bpp software renderer presented on an accelerated
                canvas. Requires users to provide their own game data files
                (pak0.pak, pak1.pak), imported in-app.
              '';
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.linux;
            };
          };

          default = self.packages.${system}.wasm;
        };

        # Development shell for building and testing the web target.
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            emscripten
            cmake
            nodejs
            python3
          ];

          shellHook = ''
            echo "YouHexen2 web development environment"
            echo ""
            echo "Build the software renderer with WebGPU presentation:"
            echo "  ./scripts/wasm-build.sh software"
            echo ""
            echo "Assemble and validate the static PWA artifact:"
            echo "  ./scripts/wasm-assemble-artifact.sh dist"
            echo "  ./scripts/wasm-validate-artifact.sh dist"
            echo ""
            echo "PWA shell tests:"
            echo "  npm test"
            echo ""
            echo "Note: CI pins emsdk (see .github/workflows/ci.yml); the"
            echo "nixpkgs emscripten here may differ."
          '';
        };
      }
    );
}
