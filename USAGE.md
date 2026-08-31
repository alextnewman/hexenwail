# Using YouHexen2

YouHexen2 is used through its HTML5 launcher, not through a native executable.

## Install and import

1. Open the [YouHexen2 launcher](https://alextnewman.github.io/hexenwail/) in Safari.
2. Choose **Share → Add to Home Screen** and open the installed app.
3. Import `pak0.pak` and `pak1.pak` from a legally owned copy of Hexen II. You can select loose PAKs, a directory, or a ZIP containing the game tree.
4. Import `portals/pak3.pak` if you own Portal of Praevus.
5. Press **Start game**. Assets and saves are retained in browser storage for offline play.

There is no Flatpak data directory or `glhexen2` command in the current product. The old Linux, Windows, SDL, OpenGL, and Steam Deck instructions belonged to the Hexenwail desktop ancestor and no longer apply.

## Renderer

The launcher’s **Renderer** card selects the engine bundle:

- **WebGlideNitro** is the default native WebGPU renderer.
- **Software + WebGPU presentation** is the classic 8-bit correctness reference.

Changing the renderer reloads the launcher before the next game starts. Both choices use the same imported data and saves.

## Controls

Touch controls appear automatically on a touch-only device and can be forced on or off in the launcher. Gyro aim is optional and has its own sensitivity and Y-inversion setting.

A standard browser gamepad can drive gameplay and every menu. On an iPad keyboard, `` ` `` acts as Escape and `Shift`+`` ` `` opens the console. Mouse input works where Safari exposes it, but iPadOS does not currently provide Pointer Lock.

## Saves

The engine periodically synchronizes saves to persistent browser storage and also syncs when the app leaves the foreground. Use **Export saves** to make a portable ZIP and **Import saves** to restore it. Save bundles never include commercial PAK or music data.

## Music

The HTML5 build decodes OGG Vorbis, MP3, FLAC, and WAV. It does not synthesize the MIDI files inside the original PAKs, so import external music named for the MIDI track requested by the map—for example, `casa1.ogg`, not `track02.ogg`.

See [the complete PWA guide](docs/PWA.md) for storage behavior, file mapping, save compatibility, controls, offline updates, performance capture, and troubleshooting. See [the music guide](docs/README.music) for the CD-track-to-MIDI-name table.
