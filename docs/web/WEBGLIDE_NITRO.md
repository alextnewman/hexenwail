# WebGlideNitro — WebGPU renderer design

**Status: gate removed by explicit owner instruction; first vertical slice
landed.** Nitro is no longer gated on WebGlide in any way, and WebGlide is
explicitly *not* its performance baseline (see "How Nitro is measured").
The renderer now exists as a real build configuration
(`-DWEB_RENDERER=webgpu`, macro `WEBGPUQUAKE`, bundle `hexenwail-nitro`) that
draws the static world as batched engine polygons through native WebGPU. It is
a **technology preview**: only the static world and the 2D layer are
implemented, with GPU-expanded particles as the first dynamic scene content,
so it is not yet playable. See "Where the slice stops" below.

The WebGPU *presenter* preview remains a separate thing: it is part of the
software-renderer configuration (`-DWEB_PRESENTER=webgpu`, macro
`WEBGPU_PRESENT`, bundle `hexenwail-webgpu`) and is **not** Nitro. The two
never share a build.

This document exists so the idea stops being re-derived from scratch in every
session. It is not a third rendering plan. The settled decisions in
[`ARCHITECTURE.md`](ARCHITECTURE.md) still hold: the software renderer is the
default, WebGlide stays in-tree and buildable, and an Ironwail-class modern
renderer is a non-goal.

## Vision

WebGlide is an **abortive experiment**: a WebGL2 attempt at what Raven would
have shipped for Glide in 1997, kept in-tree and buildable but not finished,
not supported, and not something Nitro waits for. **Nitro** is what a console
developer would do with that same brief once correctness is behind them and the
only thing left is showing off: the off-the-wall tricks that come from owning
the whole machine and knowing exactly what it will do every frame.

The web platform is a fixed console here (see the "browser as a console"
framing in [`ARCHITECTURE.md`](ARCHITECTURE.md)), so the same style of trick
applies: spend fixed per-frame budgets, amortise work across frames, exploit
the fact that the scene buffer, the palette and the colormap are all ours.

Nitro is a **separate, native WebGPU renderer**, not a WebGL-to-WebGPU
translation layer and not a cvar mode inside the WebGL2 backend. WebGPU has its
own device, resources, immutable pipelines, bind groups and command submission
model; hiding those behind GL-shaped calls would keep the costs and constraints
we are trying to remove.

It may share renderer-neutral scene preparation with WebGlide where that makes
ownership clearer, but backend objects never cross that boundary. The software
renderer remains the default and WebGlide remains buildable.

## How Nitro is measured

There is no WebGlide gate. The owner's instruction is on the record and
explicit:

> I don't care about WebGlide performance. We already know that the software
> renderer has great perf. WebGlide is an abortive experiment. Just make a
> native renderer and disregard WebGlide perf.

So:

* **WebGlide is not a performance baseline, a gate, or an optimisation
  criterion.** Nitro is never required to beat it, match it, or be compared
  with it, and no Nitro work waits on WebGlide being finished. WebGlide stays
  in-tree and buildable, but as an abortive experiment.
* **Nitro's performance is measured directly on the target iPad, against
  Nitro's own captures.** Take a raw capture ([`PERF_CAPTURE.md`](PERF_CAPTURE.md))
  on the device, write the numbers down, change one thing, capture again. A
  Nitro capture before the change is the only baseline a Nitro change is judged
  against. Do not guess, and do not substitute another renderer's numbers for a
  measurement you have not taken.
* **The correctness bar is Hexen II's own software renderer** — the same
  specification WebGlide answers to (`engine/h2shared/r_surf.c`, `d_*.c`), not
  WebGlide's output. WebGlide remains available as an *optional* visual and
  behavioural reference: it can be a convenient second opinion on what a frame
  should contain, and where it and the software renderer disagree, the software
  renderer wins.
* Nitro's own remaining correctness work is listed in "Where the slice stops"
  and in delivery step 5; that list, not WebGlide, is what "finished" means.

The WebGPU presenter came first because it changes only how the
already-rendered software framebuffer reaches the canvas. It was the
feasibility exercise for device acquisition, loss handling, indexed textures and
WGSL scan-out. Nitro reuses the launcher device handoff that exercise
established, and nothing else from it.

## Structural notes carried over from WebGL2

These are design observations, not a scoreboard: nothing here says Nitro must
be faster than WebGlide, and none of it is measured against WebGlide.

A static world vertex buffer, load-time asset processing, cached bindings and
non-overlapping per-frame streaming ranges are sound however they are spelled.
Replacing them merely because WebGPU offers different names would be churn, not
Nitro.

The structural opportunities WebGPU opens up are at the GL-shaped submission
boundaries an OpenGL-era API forces:

* a 2D path that uploads and draws every glyph or quad independently;
* up to 128 same-sized lightmap textures splitting otherwise compatible world
  batches;
* dynamic lightmaps uploaded as many small surface rectangles;
* repeated uniforms and mutable state calls obscuring the actual frame data;
* visible world indices rebuilt and uploaded every frame;
* transient geometry CPU-expanded even where particles could be pulled
  directly by the GPU.

Whether removing any of them actually helps is a question for a Nitro capture
on the iPad, not for a comparison with WebGlide.

Indexed palette and colormap filtering, retained history for T-buffer effects,
and independently sized animated diffuse textures do not become simpler merely
because the API is WebGPU. Preserve those semantics first.

## Backend shape

The renderer-facing frame description contains frame/view constants, visible
world surfaces, brush and alias instances, sprites and particles, ordered
translucent work, and 2D quads. The backend owns:

* WebGPU device/context recovery and a resource registry;
* a frame-ring upload arena;
* static world geometry and a lightmap texture array;
* transient models and GPU-pulled particles;
* scene/history targets and scan-out;
* a batched, full-resolution UI pass.

The pass order is lightmap updates, opaque world/entities, ordered translucent
work, optional history accumulation, scan-out, then full-resolution UI. Frame
and pass constants are packed blocks; pipeline variants are immutable.

Keep the implementation behind a positive `WEBGPUQUAKE` build macro. Do not
make `WEBQUAKE` mean GPU and do not spread WebGPU conditionals through the
shared client.

## As built (first slice)

The slice implements the backend shape above only as far as the static world
and the UI pass need. Nothing is stubbed out behind a flag that pretends to
work; unimplemented content is simply not drawn, and the renderer says so.

| Piece | File | Owns |
| --- | --- | --- |
| GPU backend | `engine/web/webgpu_nitro.js` | The device, pipelines, bind groups, buffers, textures, the two render passes and the single submit. The only file that touches WebGPU. |
| Renderer front end | `engine/hexen2/r_webgpu.c` | Cvars, view/frustum setup, the `[0,1]`-depth projection, culling, gamma/contrast, polyblend, frame entry points, `R_*`/`D_*` shared-client surface. |
| Static world | `engine/h2shared/wgpu_world.c` | Load-time texture and lightmap upload, the immutable vertex/index buffers, BSP/PVS traversal, texture chains, batch emission. |
| 2D layer | `engine/h2shared/draw_webgpu.c` | The whole `Draw_*` API, accumulated into one CPU arena and flushed as coalesced runs. |
| Video layer | `engine/h2shared/vid_webgpu.c` | Palette, colormap and tint tables, the UI-height ladder, canvas resize, the `VID_*` menu surface. |

Build wiring: `-DWEB_RENDERER=webgpu` selects those sources, defines
`WEBGPUQUAKE`, links `webgpu_nitro.js` with `--js-library`, and names the
bundle `hexenwail-nitro`. `./scripts/wasm-build.sh nitro engine/build-nitro`
(or `make build-nitro`) builds it; the launcher's renderer selector exposes it
as `nitro`.

It is a native WebGPU path, not a translation layer. There is no GL call, no
GL-shaped state machine and no shared code with `gl2_*`. The device comes from
the launcher probe (`Module.hexenwailWebGPU`), so the engine never requests a
second adapter for a canvas that already has one.

**Real batched polygons, and how draw calls are conserved:**

* the entire world's vertices are written once into a buffer created with
  `mappedAtCreation` and **no** `COPY_DST`, so it is immutable for the life of
  the map;
* every surface's triangle-fan indices are also built once at load, so the
  per-frame index list is a `memcpy` of prebuilt ranges for the visible
  surfaces and a single `writeBuffer`, never a geometry rebuild. Removing that
  per-frame stream entirely is listed above as a later structural opportunity,
  not a slice requirement, and it is gated on a measurement rather than on a
  comparison;
* visible surfaces chain by texture alone, so a visible texture costs exactly
  one `drawIndexed` — sky, liquids and lit walls do not fragment the batch;
* lightmaps live in one 128-layer `r8unorm` texture array, bound once per
  frame rather than per batch;
* every texture's bind group is built once at load;
* the 2D layer coalesces consecutive same-texture quads into runs and flushes
  the whole HUD, console and menu from a single vertex buffer;
* the whole frame is one command encoder, two passes (scene, then scan-out
  plus UI at panel resolution) and one `queue.submit`.

**Indexed colour is preserved end to end.** Diffuse textures are `r8uint`
palette indices. The world shader reconstructs the software rasteriser's
lighting exactly: an 8-bit lightmap sample `L` selects colormap row
`clamp((255 - L) >> 2, 0, 63)`, that row is indexed by the texel to get a
shaded palette index, and only then does the palette produce RGB. Nothing is
filtered in RGB space, so the result stays inside the authored palette by
construction — the same contract as the software renderer, which is the
specification here.

### Where the slice stops

The renderer prints its own gaps once per map (`r_nitro_report 0` silences
it) so a running build never quietly implies more than it does.

Not drawn at all: brush entities, alias models and sprites. Particles are
uploaded as compact instances and expanded into camera-facing quads by the
WebGPU vertex shader. Not
applied: dynamic lights, animated light styles, fog. Approximated: sky is its
solid layer drawn unscrolled, liquids are opaque and unwarped. Texture
*animation* is implemented, so it is deliberately absent from that list.

This is why the launcher labels `nitro` a technology preview limited to the
static world, and why the bundle is optional in the service worker rather than
precached.

## Delivery order

1. Prove device acquisition, canvas presentation, indexed textures, texture
   arrays, resize, suspension and device loss on the installed iOS PWA.
2. Establish offscreen scene scan-out and failure reporting.
3. Batch the complete 2D layer.
4. Render the static world from immutable geometry with one lightmap texture
   array, retaining CPU BSP/PVS traversal.
5. Add brush entities, aliases, sprites, sky, liquids, particles, fog and
   dynamic lights in correctness order.
6. Consolidate frame uniforms, upload arenas and dirty lightmap regions.
7. Add adaptive scene scale, then GPU-side particles. Indirect submission,
   checkerboarding and decoupled scan-out require measured evidence.

The first renderer milestone is static world plus lightmap array, Glide-style
scan-out and complete batched UI. Its bar is the software renderer's output,
and GPU-driven visibility or model transforms come after content correctness,
not after a comparison with WebGlide.

Progress: steps 1–4 and the particle part of step 5 are delivered by the slice
described in "As built" —
device acquisition and resize via the launcher handoff, offscreen scene
scan-out with explicit failure reporting, a fully batched 2D layer, and the
static world drawn from immutable geometry with one lightmap texture array
behind CPU BSP/PVS traversal. Step 5 is the next work: it is the content
Nitro still does not draw, listed in "Where the slice stops". Steps 6–7 remain
gated on measured evidence from Nitro captures on the iPad.

## Candidate tricks

Unordered, unscheduled, and deliberately biased towards things that are cheap
on one WASM thread and that stay inside the palette/colormap look. Each is a
trade, so each needs a number: capture Nitro on the iPad before and after, and
judge it against *that*. WebGlide's frame cost is not an input to any of them.

* **Adaptive scene scale.** `r_nitro_scenescale` is already a fraction of the
  view. Drive it from measured frame time instead of a fixed cvar, with
  hysteresis, so the panel resolution HUD stays crisp while the scene breathes
  under load. The console-standard dynamic resolution, and the single largest
  win available.
* **Interlaced or checkerboard scene update.** Render half the scene pixels
  per frame and resolve the other half from the previous frame through the
  existing T-buffer accumulation. Mid-90s consoles shipped this; the artefact
  budget is generous because the target look is already a 16bpp dither.
* **Decoupled scan-out.** Present the last resolved scene buffer on a frame
  where the CPU could not finish a new one, so a slow simulation frame costs
  latency instead of a dropped presentation.
* **Palette-domain effects.** Damage flashes, water shifts and torch flicker
  are palette and colormap-row operations, not per-pixel shader work. Doing
  them in the LUT is nearly free and is period-correct by construction.
* **GPU-side particles.** Particles are the one CPU-transformed batch that is
  trivially parallel and never needs the CPU's answer back. Vertex-pulling
  them removes the largest per-frame CPU transform cost that is not models.
* **Lightmap delta uploads.** The world atlas is static; only dlight-touched
  blocks change. Upload changed rectangles instead of blocks.
* **Debug visualisations built on the overlay.** Overdraw heat, batch
  boundaries, atlas occupancy. Cheap to add once the overlay exists, and they
  are how the tricks above get validated.

## What Nitro is not

* Not a modern renderer. No PBR, no deferred shading, no shadow maps. Those
  belong to the desktop tree that this port does not build.
* Not a replacement for the software renderer, and not a reason to weaken it.
* Not a WebGlide successor, competitor or benchmark target. WebGlide is an
  abortive experiment that stays buildable; Nitro neither waits for it nor is
  scored against it.

## Related documents

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — layering, the macro contract, non-goals.
* [`WEBGLIDE.md`](WEBGLIDE.md) — the abortive WebGL2 experiment, kept buildable
  and usable as an optional visual reference. Not a performance baseline.
* [`PERF_CAPTURE.md`](PERF_CAPTURE.md) — the instrument used to measure Nitro
  on the target iPad, against Nitro's own earlier captures.
* [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md) — the default renderer, and
  the specification Nitro's output answers to.
