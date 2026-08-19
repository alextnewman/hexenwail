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

The rules that follow from that brief:

* The CPU does all transform, lighting and clipping for models, sprites and
  particles and hands the GPU nothing but textured, pre-lit triangles. That
  is what Glide wanted, and on one WASM thread it is also the cheapest thing
  that works.
* The world is the exception: its geometry never moves, so it lives in a
  static vertex buffer drawn as per-texture index batches.
* Everything expensive that depends only on the assets — mip chains, alpha
  fringe repair, lightmap atlases, `.lit` colour — happens once at load time,
  never per frame.
* The scene is rendered into an offscreen buffer that may be larger than the
  display (`gl_glide_supersample`) and resolved on scan-out.
* The period look is a set of scan-out *choices*, not a limitation: the 16bpp
  ordered dither, the 2×2 "22-bit" postfilter, the T-buffer blur and the
  optional CRT are all cvars.

## Naming

The build option is still spelled `webgl2` — that is the CMake value and the
`WEBGL2QUAKE` macro — but everything user-facing is WebGlide: the launcher
toggle, the console messages and the shipped bundle basename
(`hexenwail-webglide.*`). WebGlide is deliberately distinct from the older
"maximum GL2" profile this configuration used to carry.

## Files

| File | Role |
| --- | --- |
| `engine/hexen2/r_webgl2.c` | Renderer entry points, cvars, the per-entity PimpModel override table, `Fog_ParseServerMessage`. |
| `engine/h2shared/gl2_glide.c` | Matrices, the offscreen scene buffer, T-buffer accumulation and scan-out. |
| `engine/h2shared/gl2_world.c` | World geometry, lightmap atlases (128 × 128 RGBA), `.lit` colour, sky. |
| `engine/h2shared/gl2_alias.c` | Alias models, sprites and particles — CPU transformed and lit. |
| `engine/h2shared/gl2_texture.c` | Texture manager: palette expansion, alpha fringe repair, mip generation. |
| `engine/h2shared/gl2_shader.c` | The four shader programs. Sources are extracted and compiled for real by the smoke test, so keep them as `static const char <name>[] = ...` literals. |
| `engine/h2shared/gl2_glide.h` | Internal interface and the `gl_glide_*` cvar declarations. |
| `engine/h2shared/vid_webgl2.c` | VID backend: WebGL2 context creation, canvas sizing, presentation. |
| `engine/h2shared/draw_webgl2.c` | The extended 2D API (`Draw_Quad`, `GL_SetCanvas`, glyph batching). |

`gl_fog.c` and `gl_refrag.c` are the only files shared with the old desktop
GL renderer. The rest of the `gl_*.c` desktop tree is still in-tree but is
**not compiled** by any configuration.

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

## Coloured light

Colour comes from two places, both already present in the game data:

* **`.lit` files** — the 1997 LordHavoc coloured-lighting standard, loaded
  alongside the map and used in place of the BSP's greyscale samples.
* **`cl_dlights[].color`** — which Hexen II fills in for torches, missiles,
  ice and anything the QC gave `glow_settings`, plus `entity_t::colorshade`
  for the tinted spell and artifact effects.

The 8bpp software renderer collapses both to a grey ramp. This is the payoff
for running WebGlide at all.

## Cvars

All `gl_glide_*` cvars are archived. The defaults are the look the brochure
promised — full colour, a supersampled scene, no scanlines — not the
hardware's limits.

| Name | Default | Meaning |
| --- | --- | --- |
| `gl_glide_dither` | `1` | 16bpp ordered dither. |
| `gl_glide_postfilter` | `0` | The 2×2 "22-bit" scan-out filter. |
| `gl_glide_lodbias` | `-0.5` | `grTexLodBiasValue()`, i.e. mip selection bias. |
| `gl_glide_gamma` | `1` | The Voodoo gamma ramp. |
| `gl_glide_tbuffer` | `1` | VSA-100 T-buffer accumulation. Disabled automatically if the buffer is unavailable. |
| `gl_glide_motionblur` | `0` | T-buffer temporal blend, 0…0.9. |
| `gl_glide_fogtable` | `1` | `GR_FOG_WITH_TABLE` emulation. |
| `gl_glide_colordepth` | `32` | `16` = dithered, `32` = straight RGBA8. |
| `gl_glide_mipmapdither` | `0` | Voodoo Graphics mip dithering. |
| `gl_glide_supersample` | `1` | Scene buffer scale, 1…2. |
| `gl_glide_anisotropy` | `8` | Max anisotropy; `1` = off. |
| `gl_glide_crt` | `0` | Scanline strength; `0` = off. |
| `gl_glide_crt_mask` | `0` | Aperture grille strength. |
| `gl_glide_crt_curve` | `0` | Barrel distortion. |
| `gl_glide_crt_vignette` | `0` | Corner falloff. |

Shared client cvars that WebGlide actually honours include `gl_overbright`,
`gl_texturemode`, `gl_coloredlight`, the liquid alphas (`r_wateralpha`,
`r_lavaalpha`, `r_slimealpha`, `r_telealpha`, `r_turbalpha`) and the light
policy cvars (`gl_missile_glows`, `gl_torch_dlight`, `gl_flashintensity`,
`gl_extra_dynamic_lights`).

A second set is registered only so the shared client links and the menus have
something to bind to; they currently do nothing here: `r_scale`, `r_softemu`,
`r_dither`, `r_hdr`, `r_hdr_exposure`, `gl_fxaa`, `r_motionblur`,
`r_lightmap_bicubic`, `gl_flashblend`, `gl_texture_anisotropy` (use
`gl_glide_anisotropy`), `r_waterwarp`, `r_texture_external`,
`r_texture_external_hud`, `gl_fullbrights`, `gl_glows`, `gl_other_glows`,
`gl_glow_intensity`.

Two known gaps behind that last group: the texture manager builds no
fullbright mask, so palette fullbright pixels are lit like any other texel
rather than staying at full brightness; and the model path does not consume
`qmodel_t::glow_settings`, so the QC-driven glow orbs and trails are absent.

## Diagnostics

WebGlide reports through the console and the launcher's **Runtime log** card.
There are no `renderer_status` / `renderer_safe` commands and no
`r_world_debug` — those belong to the desktop GL renderer, which is not built.

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
`webglide_model` and `webglide_post` programs) plus the legacy web-profile
families still kept in `gl_shader.c` / `gl_postprocess.c`, compiles and links
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
* [`../PWA.md`](../PWA.md) — the launcher's renderer toggle and deployment.
