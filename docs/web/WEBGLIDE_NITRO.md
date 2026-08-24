# WebGlideNitro — WebGPU renderer design

**Status: primary renderer; correctness completion is under way.** Nitro is no longer gated
on WebGlide in any way, and WebGlide is
explicitly *not* its performance baseline (see "How Nitro is measured").
The renderer now exists as a real build configuration
(`-DWEB_RENDERER=webgpu`, macro `WEBGPUQUAKE`, bundle `hexenwail-nitro`) that
draws the world, its brush entities, alias models, sprites, particles and the
view weapon as batched engine polygons through native WebGPU. Palette-quantized
`.lit` files and coloured world/actor dynamic lights, authored light styles,
fog, liquid warp and alpha, fullbright skin pixels, authored glows and projected
model shadows are implemented. See "Where it stops" below.

The WebGPU *presenter* preview remains a separate thing: it is part of the
software-renderer configuration (`-DWEB_PRESENTER=webgpu`, macro
`WEBGPU_PRESENT`, bundle `hexenwail-webgpu`) and is **not** Nitro. The two
never share a build.

This document exists so the idea stops being re-derived from scratch in every
session. It is not a third rendering plan. The settled decisions in
[`ARCHITECTURE.md`](ARCHITECTURE.md) still hold: Nitro is primary, the software
renderer is parked as a reference, WebGlide stays in-tree and buildable, and an
Ironwail-class modern renderer is a non-goal.

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
ownership clearer, but backend objects never cross that boundary. Nitro is the
primary renderer, the software renderer remains the correctness reference and
WebGlide remains buildable.

### The visual north star

Nitro completes Raven's artistic vision without treating 1990s hardware limits
as sacred. Its vocabulary remains palette ramps, colormap rows, lightmaps,
ordered dither, stipple, sprites and deliberately finite budgets, but modern GPU
techniques are welcome when they make that authored world more coherent.

This is not hardware retro-ism and not a conventional modern remaster. Compute,
light fields and temporal accumulation may be used where they preserve the
assets' chunky lighting language and remain efficient on Apple and
Snapdragon X-era GPUs. PBR is not the destination. The software renderer remains
the art-direction anchor: enhancements preserve its silhouette, contrast,
palette membership and readability while extending its world-level vision to
actors, weapons and effects.

The intended signature is:

* **living palette lighting** — authored light styles, palette-quantized
  dynamic light and deliberate transitions between neighbouring colours;
* **infernal atmosphere** — depth-quantized fog, underwater palette shifts and
  localized haze rather than generic smooth volumetrics;
* **supernatural liquids** — period sine warping, dithered translucency and
  retained-frame refraction for water, lava and portals;
* **spell-specific motion** — dense indexed particles, stippled trails, glow
  orbs and restrained T-buffer afterimages whose rhythm distinguishes fire,
  ice, poison and necromancy;
* **grounded entities** — fullbright pixels, authored glows and cheap planar
  shadows, with any pose interpolation restrained enough to retain the
  stop-motion character;
* **Nitro scan-out** — a crisp panel-resolution HUD over a scalable scene with
  optional 16-bit ordered dither, a 2x2 “22-bit” resolve and restrained
  persistence.

Correctness comes first: complete the effects already authored by the game
before inventing new ones. Performance work then protects the look through
adaptive scene scale and measured fixed budgets; it must not erase the chunky
pixels and hard lighting transitions that define it.

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
* Nitro's own remaining correctness work is listed in "Where it stops"
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
* a batched, full-resolution UI pass, with legacy pre-view clears kept before
  scan-out and overlays kept after it.

The pass order is lightmap updates, opaque world/entities, ordered translucent
work, optional history accumulation, pre-view 2D clears, scan-out, then
full-resolution UI overlays. Frame and pass constants are packed blocks;
pipeline variants are immutable.

Keep the implementation behind a positive `WEBGPUQUAKE` build macro. Do not
make `WEBQUAKE` mean GPU and do not spread WebGPU conditionals through the
shared client.

## As built

The renderer implements the backend shape above as far as the world, the
entities in it and the UI pass need. Nothing is stubbed out behind a flag that
pretends to work; unimplemented content is simply not drawn, and the renderer
says so.

| Piece | File | Owns |
| --- | --- | --- |
| GPU backend | `engine/web/webgpu_nitro.js` | The device, pipelines, bind groups, buffers, textures, the two render passes and the single submit. The only file that touches WebGPU. |
| Renderer front end | `engine/hexen2/r_webgpu.c` | Cvars, view/frustum setup, the `[0,1]`-depth projection, culling, gamma/contrast, polyblend, frame entry points, `R_*`/`D_*` shared-client surface. |
| Static world | `engine/h2shared/wgpu_world.c` | Load-time texture and lightmap upload, the immutable vertex/index buffers, BSP/PVS traversal, texture chains, batch emission, brush entities, the per-entity uniform arena, the world light sample, the single scene submit. |
| Entities | `engine/h2shared/wgpu_entity.c` | Alias models and sprites: skin cache and player-colour translation, entity lighting, pose and transform, sprite orientations, the visedict walk and the view weapon. |
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
* sky keeps the authored indexed solid and punched-out cloud layers in one
  cached binding, projects them from the eye direction, and scrolls them at
  independent speeds in one draw;
* the 2D layer coalesces consecutive same-texture quads into runs and flushes
  the whole HUD, console and menu from a single vertex buffer;
* the whole frame is one command encoder, two passes (scene, then scan-out
  plus UI at panel resolution) and one `queue.submit`.

**Entities.** Brush entities reuse the world's immutable vertex buffer: only
their indices are gathered per frame, and their transform, alpha and flat
light level live in a 256-byte block of a per-entity uniform arena addressed
by dynamic offset. A door therefore costs an offset, not a buffer, and not a
copy of the map. That arena is a uniform buffer rather than a vertex-stage
storage buffer on purpose: WebGPU's compatibility limits allow zero storage
buffers in the vertex stage, and the target device is an iPad. Block 0 is the
world itself.

Alias models and sprites are posed, transformed and lit on the CPU into one
per-frame vertex arena, so the model vertex stage is a single matrix multiply
and a whole texture's worth of models is one `draw`. Model frames step rather
than interpolate, matching the software renderer. Player skins are translated
through the entity's colormap and cached by `(skin, top, bottom)`, so a colour
change re-specifies one texture instead of leaking a new one per frame.

**Entity lighting stays indexed.** `d_polyse.c`'s shading is reproduced:
`darkness = (255 - ambient) * 64`, plus `shade * 64 * lightcos` when
the normal faces the light, clamped to `[0, 255 * 64]`, divided by 256 to give
the colormap row. Hexen II's `colorshade` stays an index remap too — the
256x256 `gfx/tinttab.lmp` is a `r8uint` texture applied *after* the colormap,
which is what `R_AliasDrawModel` does when it rebuilds `globalcolormap`. A
data set with no tint table gets an identity table, so a missing file is a
visual no-op rather than a black item.

**One authored atmosphere feeds every solid actor.** Nitro builds a bounded,
lazy light volume over the BSP. A cell resolves six axis traces through the
same authored samples and animated style values used by world lightmaps,
producing an ambient value, a palette-light colour and a dominant incoming
direction. Alias models and
the view weapon trilinearly sample that field; active dynamic lights are folded
into the same sample, so a torch changes both brightness and direction rather
than merely raising a model's flat ambient term. Their engine-authored RGB
colour is luminance-normalised into that sample as well, so the wall, actor and
view weapon receive the same flickering hue.

Interpolation is BSP-aware: a probe contributes only when it shares the
receiver's convex leaf or the segment between them remains outside solid map
contents. Dynamic lights use that same visibility test for world lightmaps,
actors and the view weapon, and back-facing surfaces terminate authored-light
traces instead of allowing a sample from the far side of a wall. This keeps a
closed wall authoritative across every kind of receiver rather than correcting
leaks separately in each draw path.

The volume's direction chooses the lit side of an alias model while the sampled
ambient supplies the authored software-style shade range. This preserves
pronounced silhouettes instead of weakening them in proportion to the coarse
probe gradient. The view weapon receives the same directional range, including
its existing minimum readability floor.

The old projected depth/stencil silhouette is deliberately suppressed whenever
the shared volume is enabled. It describes neither the authored ambient field
nor a real emitter and would place a second, contradictory lighting composition
over the new one. `r_dynamic 1` and `r_nitro_lightvol 1` are the shipped
defaults. Setting `r_nitro_lightvol 0` restores the legacy point-lighting path
and permits `r_shadows` again for controlled A/B diagnosis.

World lightmaps remain the detailed surface authority: replacing 16-unit
authored luxels with a coarse volume would erase composition and leak across
walls. The volume instead carries that composition into objects that never had
surface lightmaps. It is limited to 2 MiB, defaults to 64-unit cells, resolves
at most 32 cells per frame and falls back to the previous point sample whenever
its budget is exhausted. This deliberately spends CPU cache instead of adding a
GPU pass or a per-fragment 3D fetch on Apple and Adreno. A GPU mirror, compute
injection and temporal propagation remain valid later stages, but only after
target-device captures show that they improve either expression or cost.
Resolved corners are retained across budget pressure; a partially warm sample
renormalizes its available corners, and only an entirely cold sample falls back
to the previous point-light path.

| Cvar | Default | Meaning |
| --- | --- | --- |
| `r_nitro_lightvol` | `1` | Shared BSP-derived ambient and direction for solid actors; `0` restores the point-sample path for A/B captures. |
| `r_nitro_lightvol_cell` | `64` | Requested map-space cell size, power-of-two snapped and automatically coarsened to the fixed memory budget. |
| `r_nitro_lightvol_budget` | `32` | Maximum lazy cell resolves per frame. |

Sprites are unlit, exactly as `d_sprite.c` leaves them, and carry all five
Hexen II orientations. The view weapon is drawn last, into the near 30% of the
depth range: WebGPU has no `glDepthRange`, so the range belongs to the
viewport, and the viewport is restored afterwards. Model pipelines do not
cull: alias winding flips with the sign of the entity's scale and sprites are
built facing the eye, so the depth test decides visibility, as it does in the
software rasteriser's own model path.

**Indexed colour is preserved end to end.** Diffuse textures are `r8uint`
palette indices. The world shader reconstructs the software rasteriser's
lighting exactly: an 8-bit lightmap sample `L` selects colormap row
`clamp((255 - L) >> 2, 0, 63)`, that row is indexed by the texel to get a
shaded palette index, and only then does the palette produce RGB. Nothing is
filtered in RGB space before that lookup, so the neutral result stays inside
the authored palette by construction — the same contract as the software
renderer, which is the specification here.

Coloured lighting does not weaken that contract. World lightmaps are
`rgba8unorm`: alpha retains the software renderer's scalar intensity while RGB
contains only luminance-normalised chroma from standard `.lit` samples and
`cl_dlights[].color`. Aliases carry the same chroma from the shared light
volume. After the ordinary colormap lookup, Nitro applies that chroma and
snaps the result through a precomputed 32x32x32 nearest-palette cube. White
lights therefore take the exact old colormap path, mixed coloured lights remain
bounded, and every coloured result is still one of the game's 256 palette
entries. `gl_coloredlight 0` restores neutral lighting without disabling the
underlying scalar light or its authored flicker timing.

Atmosphere follows the same rule. Authored exponential fog is rounded into a
small number of depth bands before it is mixed, and the result is snapped back
through the nearest-palette cube. Optional localized haze modulates that fog
with slow world-space pockets rather than adding a volumetric pass. Contents
shifts and full-screen flashes use the existing `cl.cshifts` timing and colour,
then return the shifted scene to the palette in scan-out; this includes the
game's white lightning flashes. The panel-resolution UI remains outside every
one of these operations.

| Cvar | Default | Meaning |
| --- | --- | --- |
| `r_nitro_fogbands` | `8` | Number of authored-fog depth steps; values below 2 restore smooth fog. |
| `r_nitro_haze` | `0` | Strength of localized palette haze; disabled pending target-device aesthetic validation. |
| `r_nitro_paletteshifts` | `1` | Palette-snap contents, damage, powerup and lightning/white-flash shifts; `0` restores continuous RGB scan-out tinting. |

### Where it stops

The renderer prints its own gaps once per map (`r_nitro_report 0` silences
it) so a running build never quietly implies more than it does.

Model frames still step rather than interpolate. In the legacy light-volume-off
fallback, projected shadows use the sampled world floor and a two-sided
depth/stencil receiver mask that clips the
silhouette both where no receiver exists and where nearer geometry occludes it,
so raised platforms do not leave a shadow floating beyond their edge; luminous
glow models do not cast one. The result remains a deliberately cheap planar
silhouette rather than a shadow map. Model fullbright pixels remain available
through `gl_fullbrights`, but only models marked as luminous may bypass the
colormap: incidental high palette indices in ordinary dim skins no longer
become bright speckles.
Sky uses both authored palette-indexed layers, projected from the eye direction
and scrolled independently. Authored light styles animate world lightmaps and
model-light-style entities; touched atlas pages are rebuilt and uploaded only
when a style value changes.
Texture *animation*, entity animation, player skin translation and particles
are implemented, so they are deliberately absent from those lists.

Ordering is opaque then translucent, as the software renderer's edge list
produces; within the translucent half, brush entities are drawn before alias
models and sprites rather than interleaved in visedict order. That is a
deliberate batching choice, not an oversight: it keeps a translucent door to
one `drawIndexed` and it is invisible unless two translucent things overlap.

Core authored transparency is already active: `DRF_TRANSLUCENT`, entity alpha,
transparent/special-trans alias models and sprites, teleporter surfaces and
non-default liquid alpha values select blend pipelines with depth writes off.
Ordinary water remains opaque at the compatibility default
`r_wateralpha 1`; changing that cvar exercises the existing path. The later
“supernatural liquids” horizon adds distinctive dithered translucency and
retained-frame refraction—it does not introduce basic alpha blending. If an
authored transparent entity remains opaque with an alpha value below one, that
is a regression rather than deferred work.

The launcher labels Nitro as the default and the service worker precaches its
core bundle.

## Delivery order

1. Prove device acquisition, canvas presentation, indexed textures, texture
   arrays, resize, suspension and device loss on the installed iOS PWA.
2. Establish offscreen scene scan-out and failure reporting.
3. Batch the complete 2D layer.
4. Render the static world from immutable geometry with one lightmap texture
   array, retaining CPU BSP/PVS traversal.
5. Add brush entities, aliases, sprites, sky, liquids, particles, fog and
   dynamic lights in correctness order. The view weapon belongs here too: it
   is an alias model with its own depth range.
6. Consolidate frame uniforms, upload arenas and dirty lightmap regions.
7. Add adaptive scene scale, then GPU-side particles. Indirect submission,
   checkerboarding and decoupled scan-out require measured evidence.

The first renderer milestone was static world plus lightmap array,
Glide-style scan-out and complete batched UI; the second is everything that
moves in a map. The bar for both is the software renderer's output, and
GPU-driven visibility or model transforms come after content correctness, not
after a comparison with WebGlide.

Progress: steps 1–4 and most of step 5 are delivered by what "As built"
describes — device acquisition and resize via the launcher handoff, offscreen
scene scan-out with explicit failure reporting, a fully batched 2D layer, the
static world drawn from immutable geometry with one lightmap texture array
behind CPU BSP/PVS traversal, and then brush entities, alias models, sprites,
particles, the view weapon and the moving two-layer sky. The authored effects
in step 5 are now complete. Steps 6–7 remain gated on measured evidence from
Nitro captures on the iPad.

## Visual horizon order

This is the locked direction, ordered so later expression rests on correct game
content rather than disguising omissions:

1. **Finish the authored world (delivered):** animated light styles, world dynamic lights,
   warped/translucent liquids, fullbright pixels, model glows, fog and cheap
   projected shadows.
2. **Establish Nitro's scan-out (implemented, target validation pending):**
   optional ordered 16-bit dither, the restrained 2x2 resolve and
   colour-selective retained-frame persistence, always beneath panel-resolution
   UI. `r_nitro_dither`, `r_nitro_resolve` and `r_nitro_persistence` independently
   disable the invented effects; persistence defaults to a light 0.06 and is
   clamped to 0.25. The resolve backs away at local contrast edges, the dither is
   weighted toward fog-like low-contrast gradients and near-black colour, and
   history retains fading saturated light rather than uniformly blurring motion.
   Target-iPad Nitro-before/Nitro-after capture and aesthetic review remain the
   acceptance gate.
3. **Deepen palette lighting and atmosphere (implemented, target validation
   pending):** quantized coloured `.lit` and dynamic light reaches world
   surfaces, actors and the view weapon. Authored fog is depth-banded,
   underwater and full-screen lightning shifts return through the palette, and
   optional world-space haze provides localized pockets. Each invented
   operation has an independent off switch; target-iPad Nitro-before/Nitro-after
   capture and aesthetic review remain the acceptance gate.
4. **Give liquids supernatural material identity:** distinct water, slime, lava
   and portal movement, translucency and retained-frame refraction without
   physically based material simulation.
5. **Make magic spectacular:** GPU-pulled indexed particles, stippled trails,
   sprite ribbons, glow orbs and spell-family temporal signatures.
6. **Protect the result under load:** measured adaptive scene scale first;
   checkerboarding, decoupled scan-out and more aggressive retained history only
   where target-iPad captures justify them.

Each horizon ships in small reversible slices. Defaults may be expressive, but
every newly invented effect needs an off switch; authored correctness does not.
Cross-renderer screenshots establish meaning, not performance. Performance is
always a Nitro-before/Nitro-after capture on the target iPad.

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

* Not a conventional modern remaster. Modern GPU techniques serve the authored
  lighting language; PBR and generic feature-checklist rendering do not define
  the destination.
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
* [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md) — the parked renderer, and the
  specification Nitro's output answers to.
