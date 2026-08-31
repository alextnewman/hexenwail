# YouHexen2

**Hexen II as an installable iOS game.**

![Wheel of Karma running in YouHexen2](docs/screenshot1.png)

*Wheel of Karma by Inky.*

[Launch the PWA](https://alextnewman.github.io/hexenwail/) · [Report a bug](https://github.com/alextnewman/hexenwail/issues)

YouHexen2 is a hard port of [uHexen2 / Hammer of Thyrion](https://github.com/sezero/uhexen2) to an installed iOS PWA. It is a WebAssembly game client with its own browser platform layer, native WebGPU rendering, local asset storage, touch controls, and offline play. The browser is treated like a console operating system rather than another desktop windowing backend.

This repository began with the Hexenwail tree because it provided useful modern build scaffolding around uHexen2. That ancestry no longer defines the product: YouHexen2 targets iPadOS and iOS only, does not build native Linux or Windows clients, and is not pursuing a generic SDL port or an Ironwail-style desktop renderer.

Some internal source paths, persistent storage keys, and build artifacts still use the historical `hexenwail` identifier. Those are implementation names, not a separate edition of the game.

## Play

YouHexen2 does not include Hexen II game data. You need a legally owned copy of the game; [GOG](https://www.gog.com/en/game/hexen_ii) and Steam both sell it.

1. Open the PWA in Safari and choose **Share → Add to Home Screen**.
2. Launch it from the Home Screen.
3. Import `data1/pak0.pak` and `data1/pak1.pak`, or import a directory or ZIP containing them.
4. Start the game. The launcher stores the assets and saves locally so subsequent sessions work offline.

For Portal of Praevus, also import `portals/pak3.pak`. Loose OGG, MP3, FLAC, and WAV music files can be imported alongside the PAKs. The web build has no MIDI synthesizer, so a PAK-only installation has no music.

The launcher supports touch controls and optional gyro aim. Keyboard, mouse, and standard browser gamepads are also supported; on a tablet, a physical controller is the primary non-touch input.

See [the PWA guide](docs/PWA.md) for asset layouts, music naming, save export and restore, controls, offline behavior, and troubleshooting.

## Rendering

The launcher ships two renderer configurations:

- **WebGlideNitro** is the default and the project’s primary renderer. It is a native WebGPU backend built around indexed textures, palette and colormap lighting, authored light styles, dither, stipple, and deliberately finite effects budgets. It extends that 1990s visual language with dynamic lights, fog, translucent and warped liquids, glows, projected shadows, and spell-specific effects instead of replacing it with a conventional modern remaster.
- **Software + WebGPU presentation** runs uHexen2’s classic 8-bit software rasterizer and expands its indexed framebuffer through a WebGPU scan-out pass. It remains available as the authored-pixel correctness reference.

WebGlideNitro draws the complete playable scene, but correctness work is still in progress. The software renderer is the reference when the two disagree. See [the Nitro design](docs/web/WEBGLIDE_NITRO.md) and [software renderer design](docs/web/SOFTWARE_RENDERER.md) for the precise contracts and current status.

## Product scope

- **Primary target:** an installed PWA on iPadOS, tuned and tested on iPad Pro hardware.
- **Runtime:** Emscripten/WebAssembly, one main thread, WebGPU, WebAudio, and browser-owned persistent storage.
- **Input:** touch, gyro, keyboard, mouse, and the browser Gamepad API.
- **Content:** Hexen II, Portal of Praevus, and compatible uHexen2 community content imported by the player.
- **Not targets:** native desktop builds, SDL/POSIX parity, Android or desktop browsers as first-class platforms, multiplayer, or a dedicated server.

Other browsers may work, but they are not the compatibility or performance target.

## Build

The build requires the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) version `4.0.23`. Node.js 22 or newer runs the launcher tests.

```bash
source "$EMSDK/emsdk_env.sh"

make build          # WebGlideNitro
make build-software # classic software renderer with WebGPU presentation
make dist           # build both renderers and assemble the static PWA in dist/
make test           # run the PWA shell tests
```

The Make targets call the same scripts used by CI and GitHub Pages. `WEB_RENDERER=webgpu` selects WebGlideNitro; `WEB_RENDERER=software` selects the software reference. Both configurations must continue to build.

For the architecture, renderer macro contract, deployment pipeline, and performance capture format, read:

- [Web port architecture](docs/web/ARCHITECTURE.md)
- [WebGlideNitro](docs/web/WEBGLIDE_NITRO.md)
- [Software renderer](docs/web/SOFTWARE_RENDERER.md)
- [PWA shell and local storage](docs/PWA.md)
- [Performance capture](docs/web/PERF_CAPTURE.md)

## Lineage and license

Hexen II was created by Raven Software and published by id Software. This project descends from the open-source Hexen II engine through Anvil of Thyrion, Hammer of Thyrion / uHexen2, Shanjaq’s work, and Hexenwail. It also retains code and ideas from the broader Quake source-port community. See [the full authors list](docs/AUTHORS) for attribution.

The engine is distributed under the GNU General Public License, version 2 or later. See [LICENSE](LICENSE) and [docs/COPYING](docs/COPYING). Bundled third-party components retain their own licenses.

YouHexen2 is an unaffiliated fan project. It does not include copyrighted game data. Hexen and Quake are trademarks of their respective owners.
