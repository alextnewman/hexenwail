# The performance overlay

**Status:** shipping, off by default. Shared by **both** web renderers — the
default software rasteriser ([`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md))
and WebGlide ([`WEBGLIDE.md`](WEBGLIDE.md)).

## Why it exists

There is no external profiler on the deployment target. An installed iOS PWA
has no dev tools, no `about:tracing`, and no console the player can read; the
launcher's runtime log only sees what the engine prints. When the game runs
at 41 fps on an iPad and at 60 fps on a desktop browser, the only question
worth asking is *where the milliseconds went*, and the only place that
question can be answered is inside the frame itself.

The engine runs on one WASM thread, so the frame decomposes into four
numbers and no more:

| Stage | What it covers |
| --- | --- |
| **view** | `V_RenderView` — the 3D scene. Software: the rasteriser. WebGlide: scene submission. |
| **2d** | HUD, menus, console, plus this overlay. |
| **pre** | `VID_Update` — framebuffer upload and scan-out. |
| **oth** | Everything else: server/physics ticks, sound mixing, input, and the browser's own time between frame callbacks. |

`oth` is the honest one. It is derived — frame interval minus the three
measured stages — so anything the engine does not time (including the
browser deciding not to call us back) lands there. A large `oth` with small
stage times means the frame is *not* renderer-bound, which is exactly the
mistake this overlay exists to prevent.

## Using it

```
scr_perf 0   off (default)
scr_perf 1   one line: renderer, resolution, fps, frame time
scr_perf 2   + stage breakdown, renderer counters, worst frame and 1% low
scr_perf 3   + the frame-time graph
```

`scr_perf` is archived, and both renderer bundles share one `config.cfg`
under `-basedir /persistent`, so the setting follows you across the launcher's
renderer toggle.

Reading the display:

```
software 640x480   60.2 fps  16.0 ms
view  9.0 2d 1.0 pre 2.0 oth  4.0
uploads  1      300 KB
worst  36.0 ms  1% low 27.8 fps
[ graph ]
```

* The **fps** figure is the average over the history window (128 frames), not
  an instantaneous reciprocal, so it does not flicker. The **ms** figure next
  to it is the last measured frame interval.
* **worst** and **1% low** are the numbers a player actually feels. A frame
  rate that averages 60 and drops one frame to 36 ms reads as a stutter, and
  the average will never show it.
* The counter line is renderer-dependent: WebGlide reports **draws** and
  **tris**, the software renderer reports framebuffer **uploads** and their
  size. Whichever renderer left its counters at zero shows the other line.
* The graph plots one column per frame, oldest on the left, scaled so that the
  centre line is the 60 Hz budget (16.7 ms) and the full height is twice it.
  Green is inside budget, amber is over 16.7 ms, red is over 25 ms — a missed
  frame at any refresh rate the target panel offers. Bars are clamped, so a
  wall of full-height red means "worse than the graph can show"; read the
  `worst` figure for the size of the hitch.

The overlay draws in `CANVAS_DEFAULT` at panel resolution, like the console,
so it stays legible at every rung of the software renderer's resolution
ladder.

## Timing model

`SCR_UpdateScreen` owns the whole measurement. `WebPerf_BeginFrame` runs once
all the early-out paths are past — a frame the engine refuses to draw is not
a frame — and `WebPerf_EndFrame` runs after presentation.

The frame *interval* is measured between successive frame starts, so on the
frame it is reported it describes the previous frame's wall time, while the
stage times describe the frame that just ended. At a steady frame rate these
are the same frame; where they diverge, the divergence is the hitch, and it
shows up in `oth` and in the graph. Intervals longer than one second are
treated as load stalls rather than frames and are kept out of the history so
that a level change does not poison the window.

Stage timing only calls `Sys_DoubleTime` while the overlay is on, latched at
frame start so that toggling the cvar mid-frame cannot leave a stage
half-timed. The counters (`WebPerf_CountDraw`, `WebPerf_CountUpload`) are a
couple of integer adds and are always live, so the first frame after you
enable the overlay already carries correct numbers.

## Files

| File | Role |
| --- | --- |
| `engine/h2shared/web_perf.h` | Public interface, the counter macros, the stage enum. |
| `engine/h2shared/web_perf.c` | `scr_perf`, the timers, the history window and the drawing. |
| `engine/h2shared/screen.c` | The only caller of the frame/stage hooks; guarded by `WEBQUAKE`. |

Counter call sites: `gl2_world.c`, `gl2_alias.c`, `gl2_glide.c` and
`draw_webgl2.c` for WebGlide; `vid_soft_web.c` for the software presenter.

## Deliberate limits

* **No GPU timing.** WebGL2 in Safari exposes no timer queries, so `pre` is
  the CPU cost of issuing the upload and scan-out, not the GPU's cost of
  executing them. On a GPU-bound frame the time surfaces as back-pressure in
  `oth` on a later frame instead. Treat the overlay as a CPU profiler with a
  presentation column.
* **No automatic quality response.** Nothing reads these numbers to change
  resolution or effects; the software renderer's resolution ladder still picks
  purely from canvas geometry. Closing that loop is a separate decision.
* **No history beyond the window.** 128 frames is about two seconds. This is a
  live instrument, not a recorder; there is no capture-to-file.

## Related documents

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — layering, the macro contract, non-goals.
* [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md) — the default renderer and its resolution ladder.
* [`WEBGLIDE.md`](WEBGLIDE.md) — the experimental GPU renderer.
* [`WEBGLIDE_NITRO.md`](WEBGLIDE_NITRO.md) — the gated follow-on concept the overlay is meant to keep honest.
