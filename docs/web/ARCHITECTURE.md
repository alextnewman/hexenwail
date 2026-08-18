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
   |  software renderer  |  WebGL2 renderer    |   picked at build time
   |  (default)          |  (kept buildable)   |
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

# the experimental GPU renderer, unchanged
emcmake cmake -S engine -B build -DWEB_RENDERER=webgl2
```

Both configurations must keep compiling. The WebGL2 renderer is **not**
deprecated and must not be deleted.

### Macro contract

`WEBQUAKE` used to mean both "the web platform" and "the WebGL2 renderer".
Those are now separate:

| Macro | Meaning | Defined for |
| --- | --- | --- |
| `PLATFORM_WEB` | Emscripten host: no SDL, browser event loop, OPFS filesystem | both configurations |
| `WEBQUAKE` | web *platform* client: web VID/input/sound, extended 2D API surface | both configurations |
| `WEBGL2QUAKE` | the WebGL2 *renderer* | `-DWEB_RENDERER=webgl2` only |
| `WEBSOFT` | the software *renderer* | default configuration only |
| `GLQUAKE` | the desktop OpenGL renderer | never (no desktop target is built) |

Rules of thumb when adding a guard:

* Guarding *browser vs. native* behaviour → `WEBQUAKE` / `PLATFORM_WEB`.
* Guarding *GPU-specific* behaviour → `WEBGL2QUAKE`.
* Never add a new `#if defined(WEBQUAKE)` that really means "GPU".

## Ownership boundaries

| Area | Owner | Notes |
| --- | --- | --- |
| PWA shell, asset import, save sync, touch UI | `web/` | Plain JS + `node --test`; no engine knowledge beyond the exported C entry points. |
| Engine ↔ JS entry points | `engine/CMakeLists.txt` `EXPORTED_FUNCTIONS` | Names are `Web_*`. `web/app.js` must match exactly; a mismatch fails **silently** at runtime. |
| Platform backends | `engine/hexen2/sys_web.c`, `engine/h2shared/in_web.c`, `snd_web.c` | |
| VID / presentation | `vid_soft_web.c` + `web_canvas*.c` (software), `vid_webgl2.c` (WebGL2) | |
| Renderer | restored `d_*.c` / `r_*.c` (software), `r_webgl2.c` (WebGL2) | |
| Shared client (menu, sbar, console, screen) | `engine/hexen2`, `engine/h2shared` | Written against one API; renderer-specific gaps are filled by shim files, not by `#ifdef` sprinkling. |

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
* [`../WEBGL_RENDERER.md`](../WEBGL_RENDERER.md) — the retained WebGL2
  renderer profile.
* [`../PWA.md`](../PWA.md) — PWA shell, asset import, deployment.
