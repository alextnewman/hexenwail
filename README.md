# Hexenwail

**uHexen2 rebuilt for the iPad.**

![Wheel of Karma running in Hexenwail](docs/screenshot1.png)

*Wheel of Karma by Inky.*

[Play or install](https://alextnewman.github.io/hexenwail/) · [Report a bug](https://github.com/alextnewman/hexenwail/issues)

Hexenwail is a web port of [Hammer of Thyrion / uHexen2](https://github.com/sezero/uhexen2) made for an installed iOS PWA. The engine runs as WebAssembly, renders through WebGPU, keeps the player’s game data and saves in browser storage, and works offline after installation.

This is not the desktop source port the old README described. There is no current Linux, Windows, SDL, or OpenGL build. The browser is its own platform here, much like a console port, and the project is designed around the hardware and constraints of an iPad rather than around cross-platform parity.

## Playing

Hexenwail does not include the copyrighted Hexen II data. You need `data1/pak0.pak` and `data1/pak1.pak` from a legally owned copy of the game; [GOG](https://www.gog.com/en/game/hexen_ii) and Steam both sell it.

1. Open the [launcher](https://alextnewman.github.io/hexenwail/) in Safari.
2. Choose **Share → Add to Home Screen**, then open the installed app.
3. Import the two PAK files, a directory containing them, or a ZIP of the game directory.
4. Start the game. Imported assets and saves remain local to the device and are available offline.

Import `portals/pak3.pak` to play Portal of Praevus. The launcher also accepts compatible mods and loose OGG, MP3, FLAC, and WAV music. Hexen II’s PAK files contain MIDI music, but the web build has no MIDI synthesizer, so external music files are required for music playback.

The PWA includes:

- touch controls with optional gyro aiming;
- direct browser gamepad support, including complete controller-driven menus;
- keyboard and mouse input where the browser exposes them;
- automatic save synchronization plus save export and restore;
- edge-to-edge play, orientation and resize handling, and offline updates.

See [the PWA guide](docs/PWA.md) for data layouts, music filenames, controls, saves, storage behavior, and troubleshooting.

## Rendering

Hexenwail ships two renderers and lets the player choose between them in the launcher.

**WebGlideNitro** is the default. It is a native WebGPU renderer that keeps Hexen II’s indexed textures, palette ramps, colormap lighting, authored light styles, dither, stipple, and hard-edged 1990s character. It uses the GPU to extend that language with dynamic colored lighting, fog, warped and translucent liquids, glows, projected shadows, and spell-specific effects rather than turning the game into a conventional modern remaster.

**Software + WebGPU presentation** runs uHexen2’s classic 8-bit software rasterizer, then presents its indexed framebuffer through one WebGPU scan-out pass. It is the exact authored-pixel reference and the correctness baseline for Nitro.

Nitro draws the complete playable scene, but its correctness work is still in progress. The details and current boundaries live in [the WebGlideNitro design](docs/web/WEBGLIDE_NITRO.md). The reference path is documented in [the software renderer design](docs/web/SOFTWARE_RENDERER.md).

## Scope

The supported target is an installed PWA on iPadOS, tuned and measured on iPad Pro hardware. iPhone is supported by the same platform layer and touch interface. Other browsers may work, but Android, desktop browsers, native desktop builds, multiplayer, and dedicated servers are not development targets.

The port is deliberately single-threaded. Its platform layer directly owns WebAudio, browser input, OPFS/IndexedDB storage, fullscreen and canvas sizing, and the Emscripten main loop instead of preserving desktop abstractions that do not help the iOS build.

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

Hexenwail descends from Raven Software’s Hexen II source release through Anvil of Thyrion, Hammer of Thyrion / uHexen2, Shanjaq’s work, and the earlier desktop Hexenwail fork. The current web port retains code and ideas from those projects and from the wider Quake source-port community. See [the full authors list](docs/AUTHORS).

The engine is distributed under the GNU General Public License, version 2 or later. See [LICENSE](LICENSE) and [docs/COPYING](docs/COPYING). Bundled third-party components retain their own licenses.

Hexenwail is an unaffiliated fan project and does not include game data. Hexen and Quake are trademarks of their respective owners.
