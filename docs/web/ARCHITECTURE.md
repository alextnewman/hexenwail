# Web port architecture

**Status:** canonical. This document, together with
[`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md), is the agreed design for the
hexenwail web target. Read both before changing anything under `engine/`,
`web/`, or `docs/web/`. If reality and these documents disagree, one of them
is a bug — say so explicitly rather than quietly re-deriving a new plan.

## What this project is

hexenwail is a hard port of **uHexen2 (Hammer of Thyrion)** to the web
platform, deployed as an **installed iOS PWA**. That is the whole goal: a
Hexen II you can install on an iPad and play offline.

hexenwail was forked from a modernised uHexen2 tree because its build
bindings were convenient, not because we wanted its renderer. The modern GPU
renderer is a means, never the end.

## Non-goals

These are settled. Do not reopen them without an explicit instruction.

| Non-goal | Why |
| --- | --- |
| An Ironwail-class modern renderer | We want a *working* Hexen II, not a showcase renderer. Chasing GPU features has repeatedly cost more than it returned. |
| POSIX / desktop / SDL parity | The web target is treated as an independent operating system, the way a console port would be. There is no shared "one-size-fits-all GL port" to protect. |
| Upstreamability to uHexen2 | We change shared code whenever it is convenient for the web target. |
| Android / desktop browsers as first-class targets | They may work. They are not what we test or tune for. |
| Multiplayer / dedicated server | Out of scope for the PWA. |

## The console-port mental model

Treat the browser as a game console with a fixed, known hardware profile:

* **CPU:** one WebAssembly thread. No SMP, no worker-parallel rendering.
* **GPU:** available, but reachable only through WebGL2 / WebGPU, with
  Safari's driver quirks and no compute in WebGL2.
* **Display:** a 4:3-ish, very high-DPI panel — see the M1 iPad Pro numbers in
  [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md#panel-math).
* **Input:** touch, plus optional keyboard/gamepad.
* **Storage:** OPFS/IndexedDB, populated by the user importing their own
  legally-owned Hexen II data.

Under that model the interesting question is not "how do we express Hexen II
in modern GL?" but "what is the cheapest, most predictable way to get correct
Hexen II pixels onto this panel?". The answer we settled on is the classic
software rasteriser presented through an accelerated canvas.

## Layering

```
   engine/hexen2, engine/h2shared      game + shared client code
             |
             |  renderer interface (R_*, Draw_*, VID_*)
             v
   +---------------------+---------------------+
   |  software renderer  |  WebGlide (GPU)     |   picked at build time
   |  (default)          |  (experimental)     |
   +---------------------+---------------------+
             |                       |
             |  WebCanvas_*          |  direct GL
             v                       v
   +---------------------------------------------+
   |  presenter backends (web_canvas.h)          |
   |   web_canvas_gl2.c   [ future: WebGPU ]     |
   +---------------------------------------------+
             |
             v
        <canvas> in the PWA shell (web/)
```

The **presenter** is deliberately separated from the **renderer**. The
software renderer produces an 8bpp indexed framebuffer and knows nothing
about how it reaches the screen; the presenter knows nothing about Hexen II.
That boundary is what lets us add a WebGPU backend later without touching a
line of rasteriser code.

## Build-time renderer selection

```bash
# default: classic software rasteriser on an accelerated canvas
emcmake cmake -S engine -B build

# WebGlide, the experimental GPU renderer
emcmake cmake -S engine -B build -DWEB_RENDERER=webgl2
```

Both configurations must keep compiling. WebGlide is **not** deprecated and
must not be deleted. The build option value is `webgl2` and the macro is
`WEBGL2QUAKE`, but the shipped bundle basename (`hexenwail-webglide.*`) and
every user-facing name are WebGlide — see [`WEBGLIDE.md`](WEBGLIDE.md).

### Macro contract

`WEBQUAKE` used to mean both "the web platform" and "the WebGL2 renderer".
Those are now separate:

| Macro | Meaning | Defined for |
| --- | --- | --- |
| `PLATFORM_WEB` | Emscripten host: no SDL, browser event loop, OPFS filesystem | both configurations |
| `WEBQUAKE` | web *platform* client: web VID/input/sound, extended 2D API surface | both configurations |
| `WEBGL2QUAKE` | the WebGlide GPU *renderer* | `-DWEB_RENDERER=webgl2` only |
| `WEBSOFT` | the software *renderer* | default configuration only |
| `GLQUAKE` | the desktop OpenGL renderer | never (no desktop target is built) |

Rules of thumb when adding a guard:

* Guarding *browser vs. native* behaviour → `WEBQUAKE` / `PLATFORM_WEB`.
* Guarding *GPU-specific* behaviour → `WEBGL2QUAKE`.
* Guarding *software-rasteriser-specific* behaviour → `WEBSOFT`.
* Never add a new `#if defined(WEBQUAKE)` that really means "GPU", and never
  spell the software case as `defined(WEBQUAKE) && !defined(WEBGL2QUAKE)` —
  `WEBSOFT` exists precisely so that renderer guards stay positive and
  self-describing. The only exception is the renderer include ladder in
  `engine/hexen2/quakeinc.h`, which is an `#if`/`#elif` chain over renderers and
  is correct by construction.

## Window state and input ownership

Fullscreen, cursor and keyboard responsibilities are split along the same line
as everything else — the shell owns the page, the engine owns the game:

| Concern | Owner | Notes |
| --- | --- | --- |
| Immersive layout and native fullscreen | `web/` | `data-immersive` hides the launcher chrome and always works; the Fullscreen API is a best-effort extra requested from the launch gesture. Both are entered when the game starts, so there is no "fullscreen with no game" state. |
| Canvas size | `web/` → `Web_ResizeCanvas` | Every resize, rotation and fullscreen transition re-measures the game surface and hands CSS pixels to the engine. |
| Cursor visibility | `engine/h2shared/in_web.c` | There is no window manager and no Pointer Lock on iPadOS Safari, so `IN_Commands` hides the cursor while `key_dest == key_game`. |
| Key mapping | `engine/h2shared/in_web.c` | Web-specific: `` ` `` is Escape (iPad keyboards have none) and `Shift`+`` ` `` toggles the console. `in_key_backquote_escape 0` restores classic behaviour. |
| Gamepad | `engine/h2shared/in_web.c` | First-party driver over the browser Gamepad API — the web port has no SDL joystick layer to translate. Poll-only, so `IN_Commands` samples it once per host frame and synthesises the engine's edge-triggered key events. |
| Touch controls | `web/lib/phone-controls.js` | Shown when the device is touch-only (pointer capability, not screen size). Phone mode is a size-only signal that forces the immersive layout. |

## Ownership boundaries

| Area | Owner | Notes |
| --- | --- | --- |
| PWA shell, asset import, save sync, touch UI | `web/` | Plain JS + `node --test`; no engine knowledge beyond the exported C entry points. |
| Engine ↔ JS entry points | `engine/CMakeLists.txt` `EXPORTED_FUNCTIONS` | Names are `Web_*`. `web/app.js` must match exactly; a mismatch fails **silently** at runtime. |
| Platform backends | `engine/hexen2/sys_web.c`, `engine/h2shared/in_web.c`, `snd_web.c` | |
| Music codec set | `engine/CMakeLists.txt` | `bgmusic.c` only offers a format whose codec registered itself in `S_CodecInit`, so the build file is what decides which formats exist at runtime — see below. |
| VID / presentation | `vid_soft_web.c` + `web_canvas*.c` (software), `vid_webgl2.c` (WebGlide) | |
| Renderer | restored `d_*.c` / `r_*.c` (software), `r_webgl2.c` + `gl2_*.c` (WebGlide) | |
| Shared client (menu, sbar, console, screen) | `engine/hexen2`, `engine/h2shared` | Written against one API; renderer-specific gaps are filled by shim files, not by `#ifdef` sprinkling. |

## Audio and music

Sound output is a single first-party backend, `engine/h2shared/snd_web.c`: one
WebAudio node fed from the engine's own mixer. Everything audible — sound
effects and music alike — goes through that mixer, so the browser never gets to
own playback state. Music in particular is decoded in WebAssembly and pushed
through `S_RawSamples`; it is deliberately *not* handed to an
`HTMLAudioElement`, which would fork volume, pause/resume and looping away from
the engine and make behaviour depend on the Safari version.

**This is settled.** Native browser playback was tested on the target and does
not work: Ogg Vorbis will not play in the installed iOS PWA (MP3 will), and
Vorbis-in-Ogg only reached Safari at all in 18.4. Decoding in-engine costs a few
percent of a core in `BGM_UpdateStream` and works on every device. Audio
glitching during long frames is a *separate* problem — the mixer runs in a
main-thread `ScriptProcessorNode`. The current backend uses a 2048-frame
callback to tolerate ordinary frame variance and catches every asynchronous
AudioContext transition so a closed context cannot flood the launcher with
unhandled promise rejections. Moving the callback off the main thread still
needs the AudioWorklet work, not a different music path.

Which music formats exist is a build-time decision, because `bgmusic.c` only
offers a format whose codec registered itself in `S_CodecInit`. A codec that is
not in `engine/CMakeLists.txt` is not "unsupported" — it is silently absent, and
the symptom is a track that quietly does nothing.

| Format | Decoder | Cost |
| --- | --- | --- |
| WAV, MP3, FLAC | vendored `dr_wav.h` / `dr_mp3.h` / `dr_flac.h` in `h2shared` | none — public-domain single headers already in-tree |
| Ogg Vorbis | Emscripten `vorbis` port (`-sUSE_OGG -sUSE_VORBIS`) | one port, fetched over the network on a cold `EM_CACHE` |
| Opus, MOD/S3M/XM/IT, UMX | — | excluded: `opusfile` and `libxmp` have no Emscripten port |
| MIDI | — | excluded: no synthesiser in the web build (`_NO_MIDIDRV`) |

The Vorbis port is the only third-party dependency the client links, so it is
also the only thing in the build that needs the network. Two consequences worth
keeping in mind when touching this:

* CI pins `EM_CACHE` outside the emsdk checkout and caches it separately, then
  prefetches the ports in their own step (`.github/actions/wasm-build`). Folding
  that cache back inside the emsdk cache would make it a hostage of a key that
  never changes, and the ports would be refetched on every run.
* `-DUSE_CODEC_VORBIS=OFF` drops the port for network-less builds (this is what
  the flake's `wasm` package does). The result still plays wav/mp3/flac music,
  so it is a legitimate build-health check but not a shipping artifact.

Because there is no MIDI synthesiser, a PAK-only install has **no music at all**
until external music files are imported. See [`../PWA.md`](../PWA.md#music) for
the user-facing side, including the filename rule that makes imported tracks
actually play.

## Working agreements

* **Small, reversible steps.** Build after every meaningful change — both
  renderer configurations.
* **No history rewriting.** Fix forward with new commits.
* **Issues live in `bd`**, not in markdown TODO lists.
* **Docs are part of the change.** If you alter the resolution ladder, the
  macro contract, or the presenter interface, update these documents in the
  same commit.

## Related documents

* [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md) — the default renderer and
  presenter design, resolution ladder, and cvars.
* [`WEBGLIDE.md`](WEBGLIDE.md) — the experimental WebGlide GPU renderer.
* [`PERF_CAPTURE.md`](PERF_CAPTURE.md) — copyable raw web performance capture.
* [`WEBGLIDE_NITRO.md`](WEBGLIDE_NITRO.md) — an idea, gated on a finished WebGlide.
* [`../PWA.md`](../PWA.md) — PWA shell, asset import, deployment.
