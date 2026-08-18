# Software rendering on an accelerated canvas

**Status:** canonical, and the **default** web renderer. Read
[`ARCHITECTURE.md`](ARCHITECTURE.md) first for the macro contract and the
non-goals.

## The decision

Hexen II ships as an 8bpp paletted game. uHexen2's original software
rasteriser draws it exactly the way it was authored: chunky pixels, palette
animation, colormap lighting, stipple translucency. That look is the
*intended* look, and reproducing it faithfully on a GPU turns out to be more
work — and more Safari-specific risk — than simply running the original
rasteriser.

So the design is:

1. **Render** with the classic 8bpp software rasteriser, restored verbatim
   from uHexen2. One CPU core, no GPU state, no driver surprises.
2. **Present** by handing the indexed framebuffer to the GPU and expanding it
   through a palette lookup in a fragment shader — a single fullscreen
   triangle. This is the only GPU work in the frame.

The rasteriser cost scales with the *software* resolution, which we choose;
the presenter cost scales with the *panel* resolution, which we do not, but
it is one textured triangle, so it is free in practice.

The WebGL2 renderer remains in-tree and buildable
(`-DWEB_RENDERER=webgl2`). It is not deprecated.

## Files

| File | Role |
| --- | --- |
| `engine/h2shared/d_*.c`, `r_*.c`, `engine/hexen2/r_{main,misc,alias}.c` | The restored rasteriser. Verbatim uHexen2 apart from one documented fix (see [Deviations](#deviations-from-upstream)). |
| `engine/h2shared/vid_soft_web.c` | VID backend: owns the framebuffer, z-buffer and surface cache; the resolution ladder; aspect/letterbox policy; palette upload; `Web_ResizeCanvas`; the video menu. |
| `engine/h2shared/web_canvas.h` | Backend-agnostic presenter interface. |
| `engine/h2shared/web_canvas_gl2.c` | WebGL2 presenter backend. |
| `engine/h2shared/draw_soft_web.c` | Extended 2D API (alpha pics/fills, glyph batching, UI canvases, intermission art) implemented on the 8bpp framebuffer. |
| `engine/hexen2/r_soft_web.c` | Renderer-policy cvars and the per-entity PimpModel override table, mirroring `r_webgl2.c`. |
| `engine/h2shared/soft_web.h` | The symbols shared client code expects from a web renderer; pulled in by `quakeinc.h`. |

## Data flow

```
  R_RenderView()                    software rasteriser
        |                             writes 8bpp indices
        v
  vid.buffer  (W x H, 1 byte/pixel)
        |
        |  VID_Update()
        v
  WebCanvas_Present()
        |
        |  glTexSubImage2D  -> R8UI texture   (W*H bytes/frame)
        |  palette shifts   -> 256x1 RGBA8 LUT (1 KB, only when it changes)
        v
  fragment shader: idx = texelFetch(indexed); rgb = texelFetch(palette, idx)
        |
        v
  one fullscreen triangle -> <canvas>
```

### Why this is fast

* **One byte per pixel of CPU→GPU traffic**, not four. At 640×480 that is
  300 KB/frame; at 60 Hz, ~18 MB/s. Trivial.
* **Palette effects are free.** `V_UpdatePalette` shifts the palette every
  frame when you take damage or pick up an item. Under an RGBA readback that
  would mean re-expanding the whole framebuffer on the CPU. Here it is a 1 KB
  LUT upload.
* **No vertex work.** The fullscreen triangle is generated from
  `gl_VertexID`; there is no VBO, no attribute fetch, no index buffer.
* **Exact index lookup.** The index texture is `R8UI` sampled with
  `usampler2D` + `texelFetch`, so there is no filtering of *indices* — which
  would blend unrelated palette entries and produce garbage colours.
* **Context attributes are tuned for a blit:** alpha, depth, stencil and
  antialias off, `preserveDrawingBuffer` off,
  `renderViaOffscreenBackBuffer` off, high-performance power preference.
* **Single upload when possible.** If `rowbytes == width` the whole
  framebuffer goes up in one `glTexSubImage2D` with
  `GL_UNPACK_ALIGNMENT 1`.

### Filtering

`vid_soft_filter 0` (default) is plain nearest-neighbour: hard, classic
pixels. `vid_soft_filter 1` selects a "sharp bilinear" path — a bilinear tap
whose weights are sharpened toward the texel centre, which removes the uneven
pixel widths you get at non-integer scale factors without turning the image
to mush. Filtering always happens **after** the palette lookup, on RGB, never
on indices.

### Deferred, deliberately

* **PBO ring upload.** Emscripten's GL binding copies the JS-side heap view
  anyway, so a PBO ring adds complexity without removing the copy. Revisit
  only if profiling shows upload stalls.
* **WebGPU backend.** `web_canvas.h` exists precisely so this can be added as
  `web_canvas_gpu.c` with a runtime probe and a WebGL2 fallback. Nothing in
  the rasteriser or VID layer needs to change. Not worth doing until Safari's
  WebGPU is broadly shipped on iPadOS.

## Resolution ladder

The software rasteriser's cost is roughly linear in pixel count, so the
render resolution is the single most important knob. All rungs are 4:3 (the
aspect Hexen II's art, HUD and FOV were designed for):

| # | Mode | Pixels | Notes |
| --- | --- | --- | --- |
| 1 | 320 × 240 | 77 k | Original console-era look. |
| 2 | 400 × 300 | 120 k | |
| 3 | 512 × 384 | 197 k | |
| 4 | 640 × 480 | 307 k | The classic "good" software mode. |
| 5 | **683 × 512** | 350 k | **Exactly ×4 → 2732 × 2048** on the 12.9″ iPad Pro. |
| 6 | 800 × 600 | 480 k | |
| 7 | 960 × 720 | 691 k | |
| 8 | 1024 × 768 | 786 k | Auto mode will not go above this. |
| 9 | 1152 × 864 | 995 k | Manual only. |
| 10 | 1280 × 960 | 1.23 M | Manual only; at the rasteriser's ceiling. |

### Hard limits

`r_shared.h` fixes `MAXWIDTH 1280` and `MAXHEIGHT 1024`. These are not
arbitrary: `R_ScanEdges()` uses 12.20 fixed point, which overflows past 2048.
**Do not add a rung above 1280 × 1024.**

This is why 1366 × 1024 is *not* in the ladder even though it would be a
perfect ×2 for the 12.9″ panel — it exceeds `MAXWIDTH`.

### Panel math

The tuning target is an **M1 iPad Pro**.

**12.9″ (5th gen)** — 2732 × 2048 device pixels, 1366 × 1024 CSS at DPR 2.
That is exactly 4:3, so 4:3 content fills the panel with no letterboxing.

| Rung | Scale to 2732 × 2048 | |
| --- | --- | --- |
| 683 × 512 | **×4.000** | pixel-perfect |
| 640 × 480 | ×4.27 | |
| 1024 × 768 | ×2.67 | |
| 512 × 384 | ×5.33 | |

683 × 512 is the sweet spot and the one auto mode picks: an exact integer
upscale means every software pixel becomes an identical 4 × 4 block, which is
what makes nearest-neighbour look crisp instead of shimmery.

**11″ (3rd gen)** — 2388 × 1668 device pixels, 1194 × 834 CSS at DPR 2;
aspect ≈ 1.432. 4:3 content pillarboxes to 2224 × 1668. No ladder rung gives
an integer scale there, so auto mode falls back to the largest affordable
rung (1024 × 768, ≈ ×2.17).

### Auto mode

`vid_soft_mode 0` picks a rung from the live canvas size:

1. Discard rungs above `AUTO_MAX_PIXELS` (1024 × 768). Beyond that the
   rasteriser, not the panel, becomes the bottleneck.
2. Discard rungs that would upscale by less than `AUTO_MIN_SCALE` (2.0).
   Below that you pay a lot of CPU for an image barely sharper than the rung
   underneath.
3. Among the survivors, prefer the **largest** rung whose destination scale
   is within 1 % of an integer.
4. If none is near-integer, take the largest survivor.
5. If nothing survives, fall back to rung 1.

Auto mode re-evaluates on canvas resize (orientation change, Stage Manager
window resize, Slide Over).

### Aspect policy

By default the 4:3 image keeps its aspect and is centred, with the remaining
canvas cleared to black — pillarboxing on the 11″, nothing at all on the
12.9″. `vid_soft_stretch 1` stretches to fill instead, which distorts
geometry; it exists for people who prefer no bars.

## Cvars and commands

| Name | Default | Meaning |
| --- | --- | --- |
| `vid_soft_mode` | `0` | `0` = auto, `1`…`10` = ladder rung. Archived. |
| `vid_soft_filter` | `0` | `0` = nearest neighbour, `1` = sharp bilinear. Archived. |
| `vid_soft_stretch` | `0` | `0` = keep 4:3 and letterbox, `1` = stretch to fill. Archived. |
| `vid_softmode` | — | Console command: `vid_softmode` lists the ladder, `vid_softmode <n>` selects a rung. |

The video menu drives the same three settings, so nothing here is
console-only.

### GPU-only cvars

Shared client code references a set of GPU-facing cvars (`r_scale`,
`r_softemu`, `r_dither`, `gl_fxaa`, `r_hdr`, …). Under the software renderer
`r_soft_web.c` defines them so the client links, but they do nothing, and
`M_Rendering_IsSkip()` hides their menu rows. The light-policy cvars
(`gl_missile_glows`, `gl_torch_dlight`, `gl_flashintensity`,
`gl_extra_dynamic_lights`) are *real* — the software renderer has dynamic
lights and honours them.

## Extended 2D API on 8bpp

`console.c`, `sbar.c`, `menu.c` and `pr_csqc.c` call the extended 2D API
unconditionally. `draw_soft_web.c` implements it against the indexed
framebuffer:

| API | Software behaviour |
| --- | --- |
| `Draw_AlphaPic` | 4 × 4 ordered stipple — the classic software translucency. Fully opaque delegates to `Draw_TransPic`. |
| `Draw_FillAlpha` | Nearest palette index for the requested RGB (memoised in a small direct-mapped cache), then stipple. Rects are clamped, not asserted, because callers derive them from cvars. |
| `Draw_MenuBackdrop` | Full-screen conback. |
| `Draw_IntermissionPic` | Nearest-neighbour stretch to the full framebuffer — point sampling on purpose, so intermission art matches everything else. |
| `Draw_CachePicNoTrans` | Same as `Draw_CachePic`; 8bpp transparency is a palette index resolved at blit time, so there is nothing to suppress at load. |
| `GL_SetCanvas` | Selects the 2D drawing origin. There is no projection matrix, so a canvas is a translation: every primitive in `draw.c` adds `draw_canvas_x/y`. |
| `Draw_FlushCharBatch` | No-op. Characters go straight to the framebuffer, so there is nothing to batch. |
| `SCR_CalcUIScale` | Always `1.0`. The framebuffer is already low-resolution and the presenter does the upscaling; returning anything else would move hit-testing away from where glyphs actually land. |
| CSQC `drawsetcliparea` / `drawresetcliparea` | No-ops. The software 2D layer has no scissor. |

### UI canvases

`sbar.c` and `menu.c` draw in *canvas* coordinates, not screen coordinates —
that is the Ironwail-parity 2D model the shared client code was written
against. The canvases are fixed-size logical rectangles (`UI_CANVAS_WIDTH`,
`UI_SBAR_CANVAS_HEIGHT` in `draw.h`) that `GL_SetCanvas` places on the
screen at scale 1:

| Canvas | Placement |
| --- | --- |
| `CANVAS_SBAR` | 320 × 167 (23 top bumps + 46 main bar + 98 lower info bar), anchored to the **bottom** of the screen, horizontally centred. |
| `CANVAS_MENU` | 320 wide, anchored to the **top** of the screen, horizontally centred. |
| everything else | the framebuffer itself, origin (0, 0). |

`screen.c` restores `CANVAS_DEFAULT` after `Sbar_Draw()` — the same place
`gl_screen.c` does — so the console, plaques and menus that follow are not
drawn inside the status bar canvas.

`draw_webgl2.c` places the canvases identically (`Draw_Quad` translates by
the canvas origin), so the two renderers agree on 2D coordinates.

## Renderer-owned server messages

`cl_parse.c` dispatches `svc_fog` unconditionally, so every renderer has to
publish a `Fog_ParseServerMessage()`. The software rasteriser has no fog;
`r_soft_web.c` supplies a handler that drains the payload ([byte] density,
[byte] × 3 colour, [short] fade time) and discards it. Draining is not
optional — skipping the read would leave the rest of the server message
being parsed at the wrong offset.

## Deviations from upstream

The restored rasteriser is verbatim uHexen2 with one exception:
`R_NewMap()` in `engine/hexen2/r_main.c` now calls
`Mod_RestoreAliasModelDefaults()` and `R_ClearPimpOverrides()`, matching
`r_webgl2.c`. Without it, hexenwail's per-entity PimpModel overrides bleed
across map changes.

## Building and validating

```bash
emcmake cmake -S engine -B build-soft                        # software (default)
emmake  make  -C build-soft -j"$(nproc)"

emcmake cmake -S engine -B build-gl -DWEB_RENDERER=webgl2    # WebGL2
emmake  make  -C build-gl -j"$(nproc)"

npm test                                                     # PWA shell tests
```

Both configurations must build. CI pins emsdk `4.0.23`; older emsdk releases
reject `-sSTACK_SIZE`.

## Known gaps

* The Display/Rendering menus were written around the GPU renderer. The
  GPU-only rows are hidden under the software renderer, but the menus have
  not been reorganised around the software knobs.
* Auto mode has no runtime performance feedback; it picks purely from canvas
  geometry.

Track these in `bd`, not here.
