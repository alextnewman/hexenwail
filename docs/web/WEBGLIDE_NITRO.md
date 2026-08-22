# WebGlideNitro — the idea

**Status: idea. Not scheduled, not started, nothing in-tree implements it.**

This document exists so the idea stops being re-derived from scratch in every
session, and so that it stays *behind its gate*. It is not a third rendering
plan. The settled decisions in [`ARCHITECTURE.md`](ARCHITECTURE.md) still
hold: the software renderer is the default, WebGlide is experimental, and an
Ironwail-class modern renderer is a non-goal.

## The idea

Once WebGlide is *finished* — not "renders something", but correct — the port
has a second, entirely different opportunity. WebGlide's brief is what Raven
would have shipped for Glide in 1997. **Nitro** is what a console developer
would do with that same renderer three years into the hardware's life, once
correctness is behind them and the only thing left is showing off: the
off-the-wall tricks that come from owning the whole machine and knowing
exactly what it will do every frame.

The web platform is a fixed console here (see the "browser as a console"
framing in [`ARCHITECTURE.md`](ARCHITECTURE.md)), so the same style of trick
applies: spend fixed per-frame budgets, amortise work across frames, exploit
the fact that the scene buffer, the palette and the colormap are all ours.

Nitro is a *mode on top of WebGlide*, not a fork of it. It would be cvar-gated
(`gl_glide_nitro*`), default off, and it must never be a precondition for
WebGlide being correct.

## The gate

Do not start Nitro until all of these are true:

1. WebGlide starts, runs and renders the game correctly with legally supplied
   data: world, brush entities, alias models, sprites, particles, sky,
   liquids, fog, HUD, menus, console.
2. Its known gaps are closed or explicitly accepted, in particular the ones
   listed in [`WEBGLIDE.md`](WEBGLIDE.md) (glow settings, the cvars registered
   only so the client links).
3. Frame cost is *measured*, not guessed, with the raw performance capture
   ([`PERF_CAPTURE.md`](PERF_CAPTURE.md)) on the actual iPad target, and a
   baseline is written down.

Point 3 is why the overlay was built first. Every idea below is a trade — it
buys frame time and spends correctness, or the reverse — and a trade cannot be
evaluated without a number. Nitro without the overlay would be exactly the
kind of context-free renderer churn this repository's design docs exist to
stop.

## Candidate tricks

Unordered, unscheduled, and deliberately biased towards things that are cheap
on one WASM thread and that stay inside the palette/colormap look:

* **Adaptive scene scale.** `gl_glide_scenescale` is already a fraction of the
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
* Not a reason to delay WebGlide correctness work. If a Nitro idea makes
  WebGlide's baseline harder to reason about, the baseline wins.

## Related documents

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — layering, the macro contract, non-goals.
* [`WEBGLIDE.md`](WEBGLIDE.md) — the renderer Nitro would sit on top of.
* [`PERF_CAPTURE.md`](PERF_CAPTURE.md) — the instrument that gates it.
* [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md) — the default renderer.
