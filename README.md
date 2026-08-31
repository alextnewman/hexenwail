# YouHexen2

**Hexen II rebuilt as an HTML5 game for web-centric devices like iPad.**

[Play or install](https://alextnewman.github.io/youhexen2/) · [Report a bug](https://github.com/alextnewman/youhexen2/issues)

YouHexen2 is a web-native port of [Hammer of Thyrion / uHexen2](https://github.com/sezero/uhexen2). It runs the original game engine as WebAssembly inside an installable iOS PWA, renders through WebGPU, stores the player’s game data and saves locally, and works offline after installation.

This is not desktop Hexenwail with an HTML launcher. YouHexen2 treats the browser as its own target platform, much like a console port: it has first-party web input, audio, storage, lifecycle, and rendering backends designed around the hardware and constraints of an iPad. The source tree began with Hexenwail because its modern uHexen2 build bindings were a useful starting point, but Linux, Windows, SDL, OpenGL, and desktop parity are not goals.

The repository, generated bundles, storage formats, and some C/JavaScript interfaces retain the internal `hexenwail` name for compatibility. The game and PWA are YouHexen2.

## Playing

YouHexen2 does not include the copyrighted Hexen II data. You need `data1/pak0.pak` and `data1/pak1.pak` from a legally owned copy of the game; [GOG](https://www.gog.com/en/game/hexen_ii) and Steam both sell it.

1. Open the [launcher](https://alextnewman.github.io/youhexen2/) in Safari.
2. Choose **Share → Add to Home Screen**, then open the installed app.
3. Import the two PAK files, a directory containing them, or a ZIP of the game directory.
4. Start the game. Imported assets and saves remain local to the device and are available offline.

Import `portals/pak3.pak` to play Portal of Praevus. The launcher also accepts loose OGG, MP3, FLAC, and WAV music. Hexen II’s PAK files contain MIDI music, but the web build has no MIDI synthesizer, so external music files are required for music playback.

The PWA includes:

- touch controls with optional gyro aiming;
- direct browser gamepad support, including complete controller-driven menus;
- keyboard and mouse input where the browser exposes them;
- automatic save synchronization plus save export and restore;
- edge-to-edge play, orientation and resize handling, and offline updates.

See [the PWA guide](docs/PWA.md) for data layouts, music filenames, controls, saves, storage behavior, and troubleshooting.

## WebGlideNitro

WebGlideNitro is YouHexen2’s default renderer. It is a native WebGPU backend built around what the game actually is: indexed textures, palette ramps, colormap lighting, authored light styles, dither, stipple, sprites, and deliberately finite effects budgets.

Nitro uses the GPU to realize an impossible late-1990s version of Hexen II rather than turning it into a conventional modern remaster. Dynamic colored lighting, fog, warped and translucent liquids, glows, projected shadows, and spell-specific effects extend the visual language of the source art while preserving its silhouettes, contrast, palette relationships, and hard lighting transitions.

The launcher also ships **Software + WebGPU presentation**. That path runs uHexen2’s classic 8-bit software rasterizer, then presents its indexed framebuffer through one WebGPU scan-out pass. It is the exact authored-pixel reference and the correctness baseline for Nitro.

Nitro draws the complete playable scene, but its correctness work is still in progress. The details and current boundaries live in [the WebGlideNitro design](docs/web/WEBGLIDE_NITRO.md). The reference path is documented in [the software renderer design](docs/web/SOFTWARE_RENDERER.md).

## Platform

The supported target is an installed PWA on iPadOS, tuned and measured on iPad Pro hardware. iPhone is supported by the same platform layer and touch interface. Other browsers may work, but Android, desktop browsers, native desktop builds, multiplayer, and dedicated servers are not development targets. It runs well on Firefox for Windows-ARM64, but that result is omitted because I am the only person on Earth who uses that. 

The port is deliberately single-threaded. Its HTML5 platform layer directly owns WebAudio, browser input, OPFS/IndexedDB storage, fullscreen and canvas sizing, and the Emscripten main loop instead of preserving desktop abstractions that do not help the iOS build.

## Building

The build requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) version `4.0.23`. Node.js 22 or newer runs the PWA tests.

```bash
source "$EMSDK/emsdk_env.sh"

make build          # WebGlideNitro
make build-software # software renderer with WebGPU presentation
make dist           # build both renderers and assemble dist/
make test           # run the PWA tests
```

These targets call the same scripts used by CI, Pages, and releases. `WEB_RENDERER=webgpu` selects WebGlideNitro and `WEB_RENDERER=software` selects the software reference; both configurations must continue to build.

Developer documentation:

- [Web port architecture](docs/web/ARCHITECTURE.md)
- [WebGlideNitro renderer](docs/web/WEBGLIDE_NITRO.md)
- [Software renderer](docs/web/SOFTWARE_RENDERER.md)
- [PWA shell and storage](docs/PWA.md)
- [Performance capture](docs/web/PERF_CAPTURE.md)

## History, credits, and license

YouHexen2 descends from Raven Software’s Hexen II source release through Anvil of Thyrion, Hammer of Thyrion / uHexen2, Shanjaq’s work, and Hexenwail. It retains code and ideas from those projects and from the wider Quake source-port community. See [the full authors list](docs/AUTHORS).

The engine is distributed under the GNU General Public License, version 2 or later. See [LICENSE](LICENSE) and [docs/COPYING](docs/COPYING). Bundled third-party components retain their own licenses.

YouHexen2 is an unaffiliated fan project and does not include game data. Hexen and Quake are trademarks of their respective owners.
