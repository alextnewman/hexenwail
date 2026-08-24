# Web port architecture

**Status:** canonical. This document, together with
[`WEBGLIDE_NITRO.md`](WEBGLIDE_NITRO.md), is the agreed design for the
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
in modern GL?" but "what is the most direct way to get correct Hexen II pixels
onto this panel?". WebGlideNitro is the primary renderer; it retains indexed
textures and colormap lighting while owning the native WebGPU path end to end.
The classic software rasteriser is parked as a correctness reference.

## Layering

```
   engine/hexen2, engine/h2shared      game + shared client code
             |
             |  renderer interface (R_*, Draw_*, VID_*)
             v
   +---------------------+-----------------+-------------------+
   |  software renderer  |  WebGlide       |  WebGlideNitro    |  picked at
   |  (parked reference) |  (WebGL2, exp.) |  (WebGPU, primary)|  build time
   +---------------------+-----------------+-------------------+
             |                    |                  |
             |  WebCanvas_*       |  direct GL       |  Nitro_* -> WebGPU
             v                    v                  v
   +-----------------------------------+             |
   |  presenter backends               |             |
   |  (web_canvas.h)                   |             |
   |   web_canvas_gl2.c                |             |
   |   web_canvas_wgpu.c               |             |
   +-----------------------------------+             |
             |                    |                  |
             v                    v                  v
              <canvas> in the PWA shell (web/)
```

The **presenter** is deliberately separated from the **renderer**. The
software renderer produces an 8bpp indexed framebuffer and knows nothing
about how it reaches the screen; the presenter knows nothing about Hexen II.
That boundary is what let us add a WebGPU presenter without touching a line
of rasteriser code.

The presenter layer belongs to the software renderer alone. WebGlide and
WebGlideNitro each own their whole path to the canvas, which is why they do
not appear above `web_canvas.h`.

## Build-time renderer selection

```bash
# default: WebGlideNitro, the native WebGPU renderer
emcmake cmake -S engine -B build

# parked classic software rasteriser on an accelerated canvas
emcmake cmake -S engine -B build -DWEB_RENDERER=software

# WebGlide, the experimental GPU renderer
emcmake cmake -S engine -B build -DWEB_RENDERER=webgl2

# explicit WebGlideNitro selection
emcmake cmake -S engine -B build -DWEB_RENDERER=webgpu
```

`WEB_RENDERER` is the *only* renderer switch, it takes exactly one of
`software`, `webgl2` or `webgpu`, and all three configurations must keep
compiling. WebGlide is an **abortive experiment**: deprecated in the sense that
no further work is owed to it, but it stays in-tree, keeps compiling and must
not be deleted. It is not a performance baseline for anything — the owner's
instruction is explicit that WebGlide performance is not a criterion — and at
most an optional visual and behavioural reference.

| `WEB_RENDERER` | Macro | Bundle | Build helper | Status |
| --- | --- | --- | --- | --- |
| `software` | `WEBSOFT` | `hexenwail.*` | `make build-software` | Parked correctness reference |
| `webgl2` | `WEBGL2QUAKE` | `hexenwail-webglide.*` | `make build-webgl2` | Abortive experiment, kept buildable |
| `webgpu` (default) | `WEBGPUQUAKE` | `hexenwail-nitro.*` | `make build` / `make build-nitro` | Primary renderer |

The option value is `webgl2` and the macro is `WEBGL2QUAKE`, but the shipped
bundle basename and every user-facing name are WebGlide — see
[`WEBGLIDE.md`](WEBGLIDE.md). Likewise `webgpu`/`WEBGPUQUAKE` ships as
WebGlideNitro — see [`WEBGLIDE_NITRO.md`](WEBGLIDE_NITRO.md).

The software renderer additionally has an opt-in WebGPU *presenter*
feasibility build, which is a different axis entirely:

```bash
./scripts/wasm-build.sh webgpu engine/build-webgpu
```

`WEB_PRESENTER` only applies to `WEB_RENDERER=software`. It changes only
indexed framebuffer scan-out, retains `WEBSOFT`, and is **not** the
WebGlideNitro renderer despite both using WebGPU. The two never share a build:
the presenter is `hexenwail-webgpu` with `WEBGPU_PRESENT`, Nitro is
`hexenwail-nitro` with `WEBGPUQUAKE`.

What they do share is the launcher device handoff. The launcher acquires the
asynchronous WebGPU device before `callMain` and hands it over through
`Module.hexenwailWebGPU`; both the presenter and Nitro adopt that device rather
than requesting a second adapter for a canvas that already has one.

### Macro contract

`WEBQUAKE` used to mean both "the web platform" and "the WebGL2 renderer".
Those are now separate:

| Macro | Meaning | Defined for |
| --- | --- | --- |
| `PLATFORM_WEB` | Emscripten host: no SDL, browser event loop, OPFS filesystem | every configuration |
| `WEBQUAKE` | web *platform* client: web VID/input/sound, extended 2D API surface | every configuration |
| `WEBGL2QUAKE` | the WebGlide GPU *renderer* | `-DWEB_RENDERER=webgl2` only |
| `WEBGPUQUAKE` | the WebGlideNitro native WebGPU *renderer* | `-DWEB_RENDERER=webgpu` only |
| `WEBSOFT` | the software *renderer* | `-DWEB_RENDERER=software` only |
| `WEBGPU_PRESENT` | WebGPU presenter under the software renderer | `WEB_PRESENTER=webgpu` only |
| `GLQUAKE` | the desktop OpenGL renderer | never (no desktop target is built) |

Rules of thumb when adding a guard:

* Guarding *browser vs. native* behaviour → `WEBQUAKE` / `PLATFORM_WEB`.
* Guarding *WebGlide-specific* behaviour → `WEBGL2QUAKE`.
* Guarding *WebGlideNitro-specific* behaviour → `WEBGPUQUAKE`.
* Guarding *software-rasteriser-specific* behaviour → `WEBSOFT`.
* Exactly one of `WEBSOFT`, `WEBGL2QUAKE` and `WEBGPUQUAKE` is ever defined,
  so "any GPU renderer" is spelled `defined(WEBGL2QUAKE) || defined(WEBGPUQUAKE)`
  rather than by negating the software case.
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
| VID / presentation | `vid_soft_web.c` + `web_canvas*.c` (software), `vid_webgl2.c` (WebGlide), `vid_webgpu.c` (Nitro) | |
| Renderer | restored `d_*.c` / `r_*.c` (software), `r_webgl2.c` + `gl2_*.c` (WebGlide), `r_webgpu.c` + `wgpu_world.c` + `wgpu_entity.c` + `draw_webgpu.c` (Nitro) | The three renderers share no backend code. Nitro treats WebGlide as a visual and scene-preparation reference only; it contains no GL call and no `gl2_*` source. |
| WebGPU JS libraries | `engine/web/webgpu_present.js` (presenter), `engine/web/webgpu_nitro.js` (Nitro) | Linked with `--js-library`. WebGlide needs no such file because Emscripten already maps GL to WebGL2. `webgpu_nitro.js` is the only file in the Nitro build that touches the WebGPU API. |
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

* **Small, reversible steps.** Build after every meaningful change — all three
  renderer configurations.
* **No history rewriting.** Fix forward with new commits.
* **Issues live in `bd`**, not in markdown TODO lists.
* **Docs are part of the change.** If you alter the resolution ladder, the
  macro contract, or the presenter interface, update these documents in the
  same commit.

## Related documents

* [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md) — the parked reference
  renderer and presenter design, resolution ladder, and cvars.
* [`WEBGLIDE.md`](WEBGLIDE.md) — WebGlide, the abortive WebGL2 experiment that
  stays buildable.
* [`PERF_CAPTURE.md`](PERF_CAPTURE.md) — copyable raw web performance capture.
* [`WEBGLIDE_NITRO.md`](WEBGLIDE_NITRO.md) — the primary native WebGPU
  renderer. Not gated on WebGlide; measured on the target iPad against its own
  captures.
* [`../PWA.md`](../PWA.md) — PWA shell, asset import, deployment.
