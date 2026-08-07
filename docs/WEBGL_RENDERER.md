# WebGL2 renderer

The browser build uses an explicit **WebGL2 / OpenGL ES 3.0** renderer profile.
Startup prints the selected profile, drawable dimensions, supported extensions,
fallbacks, shader status, and the result of an RGBA8 texture/FBO self-test to the
PWA runtime log.

## Browser baseline

| Feature | WebGL2 behavior |
| --- | --- |
| World and brush entities | GLSL ES 3.00, RGBA8 lightmaps |
| Alias models | CPU submission path |
| Skeletal SSBO path | Disabled; CPU model fallback |
| Particles | CPU-built triangles/points |
| Sky, HUD, liquids | GLSL ES 3.00 |
| Transparency | Sorted blending |
| OIT | Disabled unless indexed blending is implemented and detected |
| Gamma, contrast, render scale, water warp, FXAA | GLSL ES 3.00 post-process |
| Palette effects and bloom | GLSL ES 3.00 post-process |
| HDR targets | Enabled only with `EXT_color_buffer_float` |
| Compute, indirect drawing, bindless textures | Disabled |

Legacy `gl_lightmapfmt GL_LUMINANCE` settings and `-lm_1` are ignored in the
browser build and migrated to `GL_RGBA`/RGBA8. Unavailable HDR and OIT settings
are reset during renderer initialization.

## Diagnostics

- `renderer_status` prints the profile, dimensions, capabilities, shader
  programs, lightmap format, and atlas state.
- `renderer_safe` disables optional effects and restores the RGBA8 atlas
  baseline.
- `r_world_debug 1` shows world albedo only.
- `r_world_debug 2` shows the lightmap only.
- `r_world_debug 3` shows the world fullbright.

Mandatory shader or framebuffer failures stop renderer startup and appear in
the PWA runtime log. Unsupported optional shaders and resources are skipped
without being compiled or initialized.

## Regression gates

`scripts/webgl-smoke-test.sh` runs Chrome/Chromium with software WebGL2 and:

1. Compiles and links browser-profile contracts for every supported shader
   family.
2. Draws generated albedo, lightmap, and fullbright textures through the world
   contract into an RGBA8 FBO without game data.
3. Rejects GL errors, unexpected pixels, and predominantly black output.

The same test runs in the shared PWA/WebAssembly CI action before the WASM
build. `npm test` covers the black-frame detector and launcher failure gate.

Blackmarsh visual acceptance still requires legally supplied game data: verify
world lighting, brush and alias models, HUD, liquids, particles, sky,
gamma/contrast, and supported post-processing with clean and legacy configs.
Desktop capability detection remains in place, but desktop parity is not a
validation goal for this WebGL2 overhaul.
