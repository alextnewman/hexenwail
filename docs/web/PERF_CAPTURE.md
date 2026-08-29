# Raw web performance capture

**Status:** shipping, off by default. Shared by both renderers: software and
WebGlideNitro. The `renderer` field says which produced the rows (`software` or
`webglidenitro`).

## Purpose

Installed iOS PWAs have no accessible profiler. The web build therefore
captures a bounded window of raw frame measurements that can be copied from
the launcher and supplied directly to an analysis agent.

The capture deliberately draws nothing in-game. The former overlay generated
hundreds of UI draws while measuring the renderer and made its own
results misleading.

## Using it

Enable **Performance capture** in the launcher and start the game. Every 128
frames the engine sends one report to the launcher. Use **Show launcher** from
the in-game menu, then **Copy latest report**. The text includes:

- renderer, render resolution, browser identity, DPR and canvas size;
- one CSV row per frame;
- the launcher's bounded runtime log.

The latest engine report is retained in session storage so it survives an exit
back to the launcher.

Compare a capture with **another capture from the same renderer**. The two
renderers draw different work in different ways, so cross-renderer numbers are
not a scoreboard. WebGlideNitro is judged on the target iPad against its own
earlier captures — change one thing, capture again.

## Columns

| Column | Meaning |
| --- | --- |
| `interval_ms` | Time between browser frame callbacks. |
| `host_ms` | CPU time spent in `Host_Frame`. |
| `callback_wait_ms` | Interval minus host CPU time: browser scheduling and deferred GPU back-pressure. |
| `view_ms` | `V_RenderView`, including scene submission or software rasterisation. |
| `ui_ms` | HUD, menus and console. |
| `present_ms` | `VID_Update`, including software framebuffer upload. |
| `engine_other_ms` | Host CPU time outside those three measured stages. |
| `draws`, `tris` | GPU submissions counted by WebGlideNitro. One `draws` is one `drawIndexed` for world and brush-entity surfaces or one `draw` for a model batch, so it is directly the batch count the design aims to keep low. |
| `uploads`, `upload_kb` | Software presenter uploads; under WebGlideNitro, load-time GPU uploads plus the per-frame index, entity and model arenas. |

Intervals of one second or more are treated as loading/background stalls and
excluded. Collection and integer counters are the only per-frame overhead;
formatting and DOM updates happen once per completed 128-frame window.
Rows describe host callbacks; a callback that intentionally skips drawing has
zero rendering-stage times and its work appears in `engine_other_ms`. Report
delivery is excluded from the following frame's timing baseline.

## Implementation

`engine/h2shared/web_perf.{c,h}` owns timing and report generation, including
`WebPerf_RendererName()`, which is the only place a renderer's reported name is
decided.
`engine/hexen2/sys_web.c` brackets the complete host callback, while
`engine/h2shared/screen.c` brackets the rendering stages. The engine dispatches
one `hexenwailperf` browser event per window; `web/app.js` stores and displays
the latest report.
