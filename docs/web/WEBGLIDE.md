# WebGlide — the experimental GPU renderer

**Status:** experimental, **not** the default. The shipping renderer is the
classic 8bpp software rasteriser on an accelerated canvas — see
[`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md). WebGlide is retained,
built by CI, and must keep compiling; it may still render incorrectly or
fail to start.

## The brief

WebGlide is the mid-90s *dream* rather than mid-90s silicon: Hexen II the
way it looked on the back of a 3Dfx box — filtered textures, coloured
light, translucent water, fog you can see the world through — with as much
of the actual GPU under it as the browser will give us.

Put another way: **what if Raven had targeted Glide and shipped a reference
renderer for it?** That framing has two consequences worth stating plainly,
because they are what keeps this from drifting back into a generic GL port:

* The **specification is Hexen II's own software renderer**
  (`engine/h2shared/r_surf.c`, `d_*.c`), not GLQuake and not the desktop
  `gl_*.c` tree. Where the two disagree about what the game should look like,
  the software renderer wins. A GL renderer answers to the card; the software
  renderer answers to the art.
* The period effects are the **product**, not a nostalgia filter bolted on
  afterwards, so they ship enabled. See [Cvars](#cvars).

The rules that follow from that brief:

* The CPU does all transform, lighting and clipping for models, sprites and
  particles and hands the GPU nothing but textured, pre-lit triangles. That
  is what Glide wanted, and on one WASM thread it is also the cheapest thing
  that works.
* The world is the exception: its geometry never moves, so it lives in a
  static vertex buffer drawn as per-texture index batches.
* Dynamic world indices and CPU-transformed model vertices stream through
  non-overlapping buffer ranges within a frame. Reusing offset zero after
  every draw can serialize WebGL's Metal backend and starve the game loop.
* Everything expensive that depends only on the assets — mip chains, alpha
  fringe repair, lightmap atlases, `.lit` colour — happens once at load time,
  never per frame.
* Alias model skin coordinates are converted from the loader's 16.16 fixed
  point to normalized GPU coordinates before batching. Passing the stored
  integers through directly collapses every repeating sample onto the same
  texel, producing lit but apparently untextured hands and objects.
* The scene is rendered into an offscreen buffer sized as a fraction of the
  view (`gl_glide_scenescale`, default quarter resolution) and resolved on
  scan-out. The 2D HUD, menus and console are drawn on the canvas afterwards,
  at panel resolution, so text stays crisp.
* The period look is a set of scan-out *choices*, not a limitation: the 16bpp
  ordered dither, the 2×2 "22-bit" postfilter, the T-buffer blur and the CRT
  are all cvars, and all default to on.

## Naming

The build option is still spelled `webgl2` — that is the CMake value and the
`WEBGL2QUAKE` macro — but everything user-facing is WebGlide: the launcher
toggle, the console messages and the shipped bundle basename
(`hexenwail-webglide.*`). WebGlide is deliberately distinct from the older
"maximum GL2" profile this configuration used to carry.

## Files

| File | Role |
| --- | --- |
| `engine/hexen2/r_webgl2.c` | Renderer entry points, cvars, the per-entity PimpModel override table. |
| `common/gl_fog.c` | Shared fog state and `Fog_ParseServerMessage`; WebGlide drives it from `Fog_SetupFrame`. |
| `engine/h2shared/gl2_glide.c` | Matrices, the offscreen scene buffer, T-buffer accumulation and scan-out. |
| `engine/h2shared/gl2_world.c` | World geometry, lightmap atlases (128 × 128 RGBA), `.lit` colour, sky. |
| `engine/h2shared/gl2_alias.c` | Alias models, sprites and particles — CPU transformed and lit. |
| `engine/h2shared/gl2_texture.c` | Texture manager: indexed game textures, palette/colormap tables, palette-aware mip generation, RGBA utility textures. |
| `engine/h2shared/gl2_shader.c` | The four shader programs. Sources are extracted and compiled for real by the smoke test, so keep them as `static const char <name>[] = ...` literals. |
| `engine/h2shared/gl2_glide.h` | Internal interface and the `gl_glide_*` cvar declarations. |
| `engine/h2shared/vid_webgl2.c` | VID backend: WebGL2 context creation, canvas sizing, presentation. |
| `engine/h2shared/draw_webgl2.c` | The extended 2D API (`Draw_Quad`, `GL_SetCanvas`, glyph batching). |

`gl_fog.c` and `gl_refrag.c` are the only files shared with the old desktop
GL renderer. The rest of the `gl_*.c` desktop tree is still in-tree but is
**not compiled** by any configuration.

Sprite payloads are hunk-owned even though the model structure stores their
pointer in `cache.data`. Only alias models use the cache allocator;
`Mod_Extradata` must therefore never be used to retrieve a sprite.

## Shader programs

Four programs, which is all a renderer modelled on fixed-function hardware
needs:

| Program | Covers |
| --- | --- |
| `world` | Lightmapped world and brush surfaces plus warped liquids — one combine unit with a different address generator, as on a Voodoo. |
| `sky` | The two scrolling sky layers. |
| `model` | Everything the CPU transformed and lit: alias models, sprites, particles. |
| `post` | Scan-out: T-buffer accumulation, the 2×2 postfilter, the gamma ramp and the palette blend. |

Dither and fog are shared GLSL fragments rather than separate passes, because
that is where the hardware did them: the Voodoo dithered at pixel write time
and fogged in the same combine stage as texturing.

Every varying and uniform that carries a *coordinate* — texture coordinates,
lightmap coordinates, the sky's world position, fog depth, the scan-out's
source size — is declared `highp` in the fragment stage. The colour maths is
left at the default `mediump`, which is what the hardware had. This matters
on Apple GPUs, where `mediump` really is fp16: Quake's world texture
coordinates run to thousands of texels, and quantising them to fp16 makes the
sampled texel and the implicit mip derivative jitter from pixel to pixel,
which reads as static crawling over every wall.

## Texture filtering

Textures are filtered and mipmapped by default. Indexed world textures and
alias skins are uploaded as `R8UI`; WebGlide resolves each sample through
`colormap[row][index]` and then the palette before manually filtering the RGB
results. Palette indices are never interpolated. Mips are generated at load
time by averaging palette colours and selecting the nearest palette entry.
`gl_texturemode` still selects nearest/bilinear and nearest/trilinear mip
behaviour. RGBA utility textures retain hardware filtering and anisotropy.

Set `gl_texturemode GL_NEAREST` if you want the unfiltered look back; it is a
console command away and the mip chains are already there.

Because `gl_texturemode` is archived, a `config.cfg` written by a build from
before this was wired up pins the old `GL_NEAREST` default. If the walls still
crawl, set the cvar once — it will stick.

## How the software renderer lights the world

WebGlide's specification for lighting is `engine/h2shared/r_surf.c` plus the
`d_*.c` rasterisers, so it is worth writing the transfer function down. It is
**not** a multiply, and that is the single biggest reason a faithful GL port
of Hexen II reads as crushed to black next to the software renderer.

### World surfaces

`R_BuildLightMap` (`r_surf.c:167`) produces one value per luxel:

1. **Seed with ambient** — `blocklights[i] = r_refdef.ambientlight << 8`, from
   the `r_ambient` cvar (`r_main.c:156`, default `0`).
2. **Accumulate styles** — `blocklights[i] += lightmap[i] * scale`, where
   `scale` is `r_drawsurf.lightadj[maps]`, an 8.8 fraction derived from
   `d_lightstylevalue`.
3. **Add dynamic lights** — `R_AddDynamicLights` (`r_surf.c:68`), greyscale,
   with Quake's `max + min/2` distance approximation.
4. **Invert to a darkness index** —
   `t = (255*256 - blocklights) >> (8 - VID_CBITS)`, with `VID_CBITS 6`, so
   `t` runs `0` (fully lit) … `16320` (fully dark).
5. **Clamp** — `if (t < (1 << 6)) t = (1 << 6)`. This is *not* a light floor:
   it is headroom so the interpolator below cannot drive the index negative
   and read outside the colormap.

`R_DrawSurfaceBlock8_mip0` (`r_surf.c:367`) then interpolates `t` bilinearly
across each 16×16 block and shades every texel with one table lookup:

```c
prowdest[b] = ((unsigned char *)vid.colormap)[(light & 0xFF00) + pix];
```

`light & 0xFF00` selects one of `VID_GRADES` (64) colormap rows; `pix` is the
texture's **palette index**. The result is another palette index, resolved to
RGB only at scan-out.

### Models

`R_AliasSetupLighting` (`r_alias.c:845`) lands in the same index space:

* `r_ambientlight` is floored at `LIGHT_MIN` (5) **before** inversion, then
  `(255 - ambient) << VID_CBITS`. Because the floor is applied pre-inversion,
  a model's darkness index can never exceed `250 << 6 = 16000` — row 62.
  **Models in the software renderer never reach the darkest colormap row.**
* `r_shadelight *= VID_GRADES`, and per vertex
  `temp = r_ambientlight + r_shadelight * lightcos` for `lightcos < 0`. Since
  `temp` is *darkness*, a face turned toward the light gets a smaller index.

### What this means for WebGlide

WebGlide keeps indexed world textures and alias skins on the GPU. The world
shader converts lightmap intensity into the same 0…63 darkness row as
`R_BuildLightMap`, looks up `colormap[row][index]`, then resolves that result
through the palette. Alias vertices carry the software darkness value derived
from `ambientlight`, `shadelight`, the fixed light vector and the model normal.

| | software | WebGlide |
| --- | --- | --- |
| shading operation | table lookup `colormap[row][index]` | the same table lookup |
| light steps | 64, quantised | 64, quantised |
| output space | snapped to the 256-entry palette | palette RGB, filtered after lookup |
| interpolation | linear in **darkness** | linear in darkness, then RGB filtering |

Consequences, in the order they matter:

* **The colormap is the art direction.** Whatever curve Raven baked into
  `gfx/colormap.lmp` is applied to every software texel and to none of
  WebGlide's. It does not need reverse-engineering — the table is already
  resident as `vid.colormap` (`vid_webgl2.c:180`), alongside `d_8to24table`
  and `vid.fullbright`.
* **Palette snapping is a feature.** Colours hop between palette ramps as
  they darken instead of sliding smoothly to zero, which is most of what
  reads as period rather than muddy.
* Fullbright source indices bypass the darkness lookup. They replace the lit
  result rather than being added to it, so bright texture detail cannot clip
  into repeated white patches.
* `.lit` and dynamic-light RGB is reduced to a neutral scalar for the darkness
  row, then reapplied as luminance-preserving chroma. Stock greyscale BSP
  lighting therefore follows the software reference while coloured light
  remains a WebGlide feature.

## Light floors

The colormap's darkest rows still carry hue, so an unlit surface reads as
dark material rather than as a black silhouette. WebGlide now gets that
property from the same table instead of approximating it with a linear-light
floor.

**World surfaces — `r_ambient`.** `GL2_BuildLightmapBlock` seeds every
lightmap block with `r_ambient * 256` before accumulating light styles,
the same value in the same order as `r_surf.c:202`. Its default is `0`, as
in software; the colormap makes an artificial linear-light floor unnecessary.
It remains `CVAR_NONE` because both builds share `config.cfg`.

**Models — `LIGHT_MIN`.** `GL2_ApplyAliasLightFloor` floors both
`gl2_ambientlight` and `gl2_lightcolor` at `5`, matching `r_alias.c:26`.
It runs at `GL2_SetupEntityLighting`'s single call site rather than inside
it, because the GL rule set returns early for `EF_ROTATE` and the `MLS`
light styles, and the software renderer applies its floor after *all* of
those paths have resolved.

`LIGHT_MIN` is a backstop in colormap-row space. `(255 - 5) << VID_CBITS`
is row 62 of 64, and the difference between row 62 and row 63 is authored
hue rather than an arbitrary linear brightness floor.

**Models — `r_ambient`.** The real model floor is the same cvar the world
uses, applied where the software renderer applies it. `R_LightPoint`
(`r_light.c:307`) ends with

```c
if (r < r_refdef.ambientlight)
    r = r_refdef.ambientlight;
```

so a software model bottoms out at exactly the value `R_BuildLightMap`
seeds the surrounding walls with. `GL2_LightPoint` now floors its returned
intensity and all three colour channels the same way. The floor is applied
to the sample, *before* dynamic lights, so a dark dlight can still pull a
model under it — again as in software, where `LIGHT_MIN` then catches the
bottom.

Models and walls do not land on identical rows and are not meant to: alias
models use the software renderer's per-vertex normal-dot darkness calculation,
while world surfaces interpolate darkness across lightmap luxels.

**The view model — 24.** `R_DrawViewModel` in both renderers
(`r_main.c:806`, `gl_rmain.c:3837`) samples the world at the entity origin
rather than the model's mid-point, and floors the result at `24` —
"always give some light on gun" is the comment in each. WebGlide had
neither, so the player's hands were floored at `LIGHT_MIN` like anything
else and went black in any room that was not brightly lit.
`GL2_SetupEntityLighting` now special-cases `cl.viewent` for both.

## Coloured light

Colour comes from two places, both already present in the game data:

* **`.lit` files** — the 1997 LordHavoc coloured-lighting standard, loaded
  alongside the map and used in place of the BSP's greyscale samples.
* **`cl_dlights[].color`** — which Hexen II fills in for torches, missiles,
  ice and anything the QC gave `glow_settings`, plus `entity_t::colorshade`
  for the tinted spell and artifact effects.

The 8bpp software renderer collapses both to a grey ramp. This is the payoff
for running WebGlide at all.

## Light that is not the lightmap

Two things in Hexen II are lit without reference to a lightmap sample, and
both have to be reproduced or the world reads as far darker than the
software renderer draws it.

**Self-lit texels.** Palette indices at or above `vid.fullbright` (224 in the
stock colormap) bypass the darkness row and resolve directly through the
palette. There is no companion additive mask, so a fullbright texel replaces
the lit result instead of being added to it. `gl_fullbrights 0` disables the
explicit bypass for diagnosis; stock colormaps may still leave those authored
columns unchanged.

**Model light styles.** A brush entity's `drawflags & MLS_MASKIN` carries
either an absolute light value (`MLS_ABSLIGHT`, from `entity_t::abslight`) or
the index of a pre-baked light style — `MLS_FULLBRIGHT`, `MLS_POWERMODE`,
`MLS_TORCH`, `MLS_TOTALDARK`. Those surfaces normally have **no baked
lightmap samples at all**, so multiplying the atlas by the flat light gives
zero: bit pillars, lifts and similar brushwork render pure black. They take a
single-pass unlit draw with the flat light in `u_light` instead, which is
what vanilla Hexen II did and what the desktop GL renderer still does.

## Cvars

The visual-effect `gl_glide_*` cvars are archived and **default to on**. The
diagnostic cvar is deliberately transient. The shipped configuration is the
period look, not a clean GL image with optional garnish:
16bpp output through the ordered dither, the 2×2 scan-out filter over it, a
sharpening LOD bias with Voodoo Graphics mip dithering, a T-buffer trail and a
CRT — at a resolution a phone GPU is happy to sustain.

| Name | Default | Meaning |
| --- | --- | --- |
| `gl_glide_dither` | `1` | 16bpp ordered dither. Requires `gl_glide_colordepth 16` to have any effect. |
| `gl_glide_postfilter` | `1` | The 2×2 "22-bit" scan-out filter. |
| `gl_glide_lodbias` | `-0.5` | `grTexLodBiasValue()`, i.e. mip selection bias. Negative sharpens and sparkles. |
| `gl_glide_gamma` | `1` | The Voodoo gamma ramp, multiplied into `v_gamma`. |
| `gl_glide_tbuffer` | `1` | VSA-100 T-buffer accumulation. Disabled automatically if the buffer is unavailable. |
| `gl_glide_motionblur` | `0.25` | T-buffer temporal blend, 0…0.9. Values under `0.02` skip the pass. |
| `gl_glide_fogtable` | `1` | `GR_FOG_WITH_TABLE` emulation. |
| `gl_glide_colordepth` | `16` | `16` = dithered, `32` = straight RGBA8. This is the gate on `gl_glide_dither`. |
| `gl_glide_mipmapdither` | `1` | Voodoo Graphics mip dithering. |
| `gl_glide_scenescale` | `0.5` | Scene buffer scale per axis, 0.25…2. `0.5` is quarter resolution; above `1` supersamples. |
| `gl_glide_anisotropy` | `8` | Max anisotropy; `1` = off. |
| `gl_glide_crt` | `0.35` | Scanline strength; `0` = off. One scanline per line of `vid.height`, i.e. per UI-ladder row rather than per device pixel. |
| `gl_glide_crt_mask` | `0.35` | Aperture grille strength, in output pixels. |
| `gl_glide_crt_curve` | `0.15` | Barrel distortion. |
| `gl_glide_crt_vignette` | `0.2` | Corner falloff. |
| `gl_glide_debug` | `0` | `1` source index, `2` fullbright classification, `3` light intensity, `4` darkness row; `0` is final shading. Not archived. |

The CRT group is a *display* simulation rather than a renderer simulation, so
it is the first thing to turn off if it fights the panel:
`gl_glide_crt 0; gl_glide_crt_mask 0; gl_glide_crt_curve 0; gl_glide_crt_vignette 0`.

Because these are archived, a `config.cfg` written by an **older build** wins
over the defaults above — the engine writes every archived cvar on shutdown
(`Host_WriteConfiguration`), and the launcher syncs `/persistent` to browser
storage. A returning install therefore keeps whatever it last saved, and has
to set the period cvars explicitly once (or clear its stored data) to pick up
the new look.

Shared client cvars that WebGlide actually honours include `gl_fullbrights`,
`gl_overbright_models`, `gl_texturemode`, `gl_coloredlight`,
`v_gamma`, `v_contrast`, the liquid alphas (`r_wateralpha`, `r_lavaalpha`,
`r_slimealpha`, `r_telealpha`, `r_turbalpha`) and the light policy cvars
(`gl_missile_glows`, `gl_torch_dlight`, `gl_flashintensity`,
`gl_extra_dynamic_lights`).

`r_ambient` is registered by the WebGlide build with the software default of
`0`, and it is not archived. It
floors both the world lightmap and the model light samples. See
[Light floors](#light-floors).

`gl_coloredlight` defaults to `1`, matching the software renderer and the
desktop GL renderer. In WebGlide it currently only gates `.lit` loading
(`GL2_LoadLitFile`), so with stock Hexen II data — which ships no `.lit`
files — it changes nothing on its own.

`v_gamma` is an exponent, not a multiplier, exactly as in the rest of the
engine: the software renderer bakes `pow(c, v_gamma)` into the palette, so
values below `1` brighten. `gl_glide_gamma` multiplies into it, and the
product is clamped to 0.3…3. Both layers of the frame apply it — the scene
in the scan-out pass, the 2D layer (HUD, menus, console) in its own shader —
because the 2D layer is drawn straight to the canvas after scan-out and would
otherwise ignore the brightness slider entirely.

Which of the liquid alphas a turbulent surface gets is decided per texture,
as in the desktop GL renderer. The model loader this configuration builds
against carries no per-texture content class, so the classification uses the
vanilla `SURF_TRANSLUCENT` flag (`*lowlight`, `*rtex078`) plus the texture
name; anything unrecognised stays opaque under `r_turbalpha`, which is what
vanilla Hexen II did. Opaque liquids are drawn with depth writes on before
the blended ones, and the world's liquid pass is deferred until after the
opaque entities so that entities behind water are not drawn over it.

A second set is registered only so the shared client links and the menus have
something to bind to; they currently do nothing here: `r_scale`, `r_softemu`,
`gl_overbright`,
`r_dither`, `r_hdr`, `r_hdr_exposure`, `gl_fxaa`, `r_motionblur`,
`r_lightmap_bicubic`, `gl_flashblend`, `gl_texture_anisotropy` (use
`gl_glide_anisotropy`), `r_waterwarp`, `r_texture_external`,
`r_texture_external_hud`, `gl_glows`, `gl_other_glows`, `gl_glow_intensity`.

One known gap behind that last group: the model path does not consume
`qmodel_t::glow_settings`, so the QC-driven glow orbs and trails are absent.

## Diagnostics

WebGlide reports through the console and the launcher's **Runtime log** card.
`gl_glide_debug` exposes the indexed shading stages in both world and alias
draws. There are no `renderer_status` / `renderer_safe` commands or desktop
`r_world_debug`; those belong to the desktop GL renderer, which is not built.

| Message | Meaning |
| --- | --- |
| `WebGlide: shader compilation failed; the 3D view will be blank.` | A mandatory program did not build. The preceding lines carry the driver log. |
| `WebGlide: <name> <stage> shader failed:` / `... program failed to link:` | Per-program compile/link failure with the driver log. |
| `WebGlide: WxH scene buffer is incomplete` | The offscreen scene FBO was rejected at that size. |
| `WebGlide: T-buffer unavailable, disabling` | Accumulation is off for this session; `gl_glide_tbuffer` is ignored. |
| `Unable to create an iOS WebGL2 context` | Fatal: no WebGL2 context at all. |

## Building and validating

```bash
./scripts/wasm-build.sh webgl2 engine/build-webgl2   # -> hexenwail-webglide.*
./scripts/wasm-assemble-artifact.sh dist             # ships both bundles if both were built
```

`scripts/webgl-smoke-test.sh` runs Chrome/Chromium with software WebGL2 and
`scripts/webgl-engine-shader-smoke.mjs`, which extracts the engine's actual
shader strings from `gl2_shader.c` (the `webglide_world`, `webglide_sky`,
`webglide_model` and `webglide_post` programs) and `draw_webgl2.c` (the
`webglide_ui` 2D program) plus the legacy web-profile families still kept in
`gl_shader.c` / `gl_postprocess.c`, compiles and links
every one of them in WebGL2, draws generated textures through the world
contract into an RGBA8 FBO, and rejects GL errors, unexpected pixels and
predominantly black output. It runs in CI before the WASM build.

CI builds both renderer configurations on every run, so a deployed artifact
always ships both bundles. Locally, `make dist` builds only the software
renderer, and `scripts/wasm-validate-artifact.sh` accepts an artifact with
both WebGlide files or with neither — one half alone is an error, because the
launcher toggle would then load a bundle that cannot fetch its `.wasm`.

`npm test` covers the launcher side, including the renderer toggle and the
failure gate that names the toggle when the WebGlide bundle is missing.

Visual acceptance still requires legally supplied game data: check world
lighting and `.lit` colour, brush and alias models, sprites, particles, sky,
liquids, fog and the scan-out effects. Desktop parity is not a validation
goal — there is no desktop build.

## Related documents

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — layering, the macro contract, non-goals.
* [`SOFTWARE_RENDERER.md`](SOFTWARE_RENDERER.md) — the default renderer.
* [`PERF_CAPTURE.md`](PERF_CAPTURE.md) — copyable raw capture used to measure this renderer.
* [`WEBGLIDE_NITRO.md`](WEBGLIDE_NITRO.md) — an idea for what comes *after* this
  renderer is correct. It is gated; do not start it early.
* [`../PWA.md`](../PWA.md) — the launcher's renderer toggle and deployment.
