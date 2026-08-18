{
  description = "Hexenwail - uHexen2 (Hammer of Thyrion) as an installable iOS PWA";

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
          # CI uses, and a pure `nix build` has no network, so anything
          # that needs an Emscripten port would fail here.  The engine no
          # longer links any port (no SDL), which makes this far more
          # likely to work than it used to -- but if it does not, use
          # `nix develop` (below) or the emsdk directly and run
          # scripts/wasm-build.sh, which is what CI does.
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

            # Use Emscripten's CMake toolchain.  WEB_RENDERER defaults to
            # "software"; pass -DWEB_RENDERER=webgl2 to build the retained
            # GPU renderer instead.
            configurePhase = ''
              runHook preConfigure

              mkdir -p build
              cd build
              emcmake cmake \
                -DCMAKE_BUILD_TYPE=Release \
                -DWEB_RENDERER=software \
                ../engine

              runHook postConfigure
            '';

            buildPhase = ''
              runHook preBuild
              emmake make -j''${NIX_BUILD_CORES:-1}
              runHook postBuild
            '';

            # Mirrors scripts/wasm-assemble-artifact.sh: the PWA shell from
            # web/ plus the Emscripten runtime.  The emcc-generated HTML is
            # debug-only; web/index.html is the real entry point.
            installPhase = ''
              runHook preInstall

              mkdir -p $out
              cp -r ../web/. $out/
              substituteInPlace $out/sw.js \
                --replace-fail __HEXENWAIL_BUILD_VERSION__ "${version}"

              cp bin/hexenwail.js $out/
              cp bin/hexenwail.wasm $out/
              cp bin/hexenwail.html $out/engine-shell-debug.html
              if [ -f bin/hexenwail.data ]; then cp bin/hexenwail.data $out/; fi

              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "Hexenwail - installable WebAssembly PWA build";
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
            echo "Hexenwail web development environment"
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
