/*
 * gl2_shader.c -- the WebGlide shader family.
 *
 * Four programs, which is all a renderer modelled on fixed-function
 * hardware needs:
 *
 *   world  -- lightmapped world and brush surfaces, plus the warped
 *             liquid surfaces, which on a Voodoo were the same texture
 *             combine unit with a different address generator.
 *   sky    -- the two scrolling sky layers.
 *   model  -- everything the CPU transformed and lit: alias models,
 *             sprites, particles, glow orbs.  Glide had no vertex
 *             pipeline, so neither does this.
 *   post   -- scan-out: T-buffer accumulation, the 2x2 postfilter, the
 *             gamma ramp and the palette blend.
 *
 * The dither and fog helpers are shared GLSL fragments rather than
 * separate passes because that is where the hardware did them: the
 * Voodoo dithered at pixel write time and fogged in the same combine
 * stage as texturing.
 *
 * These sources are extracted and compiled for real by
 * scripts/webgl-engine-shader-smoke.mjs, so keep them written as
 * `static const char <name>[] = ...` string literals built from the
 * GLIDE_* macros below.
 *
 * Copyright (C) 2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "gl2_glide.h"

#define GLIDE_VERSION \
	"#version 300 es\n"

#define GLIDE_VERT_PRECISION \
	"precision highp float;\n" \
	"precision highp int;\n"

#define GLIDE_FRAG_PRECISION \
	"precision mediump float;\n" \
	"precision highp int;\n"

/*
 * grFogTable / GR_FOG_WITH_TABLE.  The Voodoo looked fog up in a 64-entry
 * table indexed by the exponent of 1/w, which is an exponential-squared
 * curve in eye distance -- so that is what this is, evaluated per pixel
 * instead of per table entry.
 */
#define GLIDE_FOG_FN \
	"uniform vec3 u_fogcolor;\n" \
	"uniform float u_fogdensity;\n" \
	"in highp float v_fogdepth;\n" \
	"vec3 GlideFog (vec3 color)\n" \
	"{\n" \
	" if (u_fogdensity <= 0.0) return color;\n" \
	" float d = u_fogdensity * v_fogdepth;\n" \
	" return mix (u_fogcolor, color, clamp (exp (-d * d), 0.0, 1.0));\n" \
	"}\n"

/*
 * The 4x4 ordered dither matrix the Voodoo used, as a helper: both the
 * 16bpp pixel dither and the mipmap dither read from it.
 */
#define GLIDE_BAYER_FN \
	"float GlideBayer (void)\n" \
	"{\n" \
	" const float bayer[16] = float[16] (\n" \
	"  0.0, 8.0, 2.0, 10.0,\n" \
	"  12.0, 4.0, 14.0, 6.0,\n" \
	"  3.0, 11.0, 1.0, 9.0,\n" \
	"  15.0, 7.0, 13.0, 5.0);\n" \
	" ivec2 p = ivec2 (gl_FragCoord.xy) & 3;\n" \
	" return (bayer[p.y * 4 + p.x] + 0.5) / 16.0 - 0.5;\n" \
	"}\n"

/*
 * The Voodoo wrote 16bpp (RGB 5:6:5) pixels through that dither.
 * u_dither carries the quantisation steps per channel, so 0 means "leave
 * the frame at full RGBA8 precision" -- which is what a 32bpp card of the
 * same era, or gl_glide_colordepth 32, would give you.
 */
#define GLIDE_DITHER_FN \
	"uniform vec3 u_dither;\n" \
	"vec3 GlideDither (vec3 color)\n" \
	"{\n" \
	" if (u_dither.g <= 0.0) return color;\n" \
	" return floor (clamp (color, 0.0, 1.0) * u_dither + GlideBayer () + 0.5) / u_dither;\n" \
	"}\n"

/*
 * grTexLodBiasValue(), plus Voodoo Graphics mipmap dithering: the first
 * Voodoo had no LOD blending, so its drivers dithered the level selection
 * to hide the mip seam.  Both are a bias on the sampled level, which in
 * GLES3 is the only place a LOD bias exists at all.
 */
#define GLIDE_LOD_FN \
	"uniform float u_lodbias;\n" \
	"uniform float u_mipdither;\n" \
	"float GlideLod (void)\n" \
	"{\n" \
	" return u_lodbias + GlideBayer () * u_mipdither;\n" \
	"}\n"

/*
 * Indexed textures are integer textures and therefore cannot use hardware
 * filtering. Resolve each tap through colormap[row][index] and the palette,
 * then filter RGB results. This avoids interpolating unrelated palette indices.
 */
#define GLIDE_PALETTE_FN \
	"uniform highp usampler2D u_indices;\n" \
	"uniform sampler2D u_palette;\n" \
	"uniform highp usampler2D u_colormap;\n" \
	"uniform int u_fullbright;\n" \
	"uniform int u_filter;\n" \
	"uniform int u_debug;\n" \
	"ivec2 GlideWrap (ivec2 p, ivec2 size)\n" \
	"{\n" \
	" return ivec2 ((p.x % size.x + size.x) % size.x,\n" \
	"                (p.y % size.y + size.y) % size.y);\n" \
	"}\n" \
	"uint GlideIndex (highp vec2 uv, int level)\n" \
	"{\n" \
	" ivec2 size = textureSize (u_indices, level);\n" \
	" ivec2 p = ivec2 (floor (fract (uv) * vec2 (size)));\n" \
	" return texelFetch (u_indices, GlideWrap (p, size), level).r;\n" \
	"}\n" \
	"vec3 GlideChroma (vec3 color, vec3 chroma)\n" \
	"{\n" \
	" vec3 tinted = color * chroma;\n" \
	" float source_luma = dot (color, vec3 (0.299, 0.587, 0.114));\n" \
	" float tinted_luma = dot (tinted, vec3 (0.299, 0.587, 0.114));\n" \
	" return (tinted_luma > 0.0001) ? tinted * (source_luma / tinted_luma) : color;\n" \
	"}\n" \
	"vec4 GlideShadeIndex (uint index, float darkness, vec3 chroma, bool holey)\n" \
	"{\n" \
	" bool fullbright = int (index) >= u_fullbright && index != 255u;\n" \
	" int row = clamp (int (floor (darkness)), 0, 63);\n" \
	" uint shaded = fullbright ? index : texelFetch (u_colormap, ivec2 (int (index), row), 0).r;\n" \
	" vec4 source = texelFetch (u_palette, ivec2 (int (index), 0), 0);\n" \
	" vec3 color = texelFetch (u_palette, ivec2 (int (shaded), 0), 0).rgb;\n" \
	" if (!fullbright) color = GlideChroma (color, chroma);\n" \
	" if (holey && index == 0u) source.a = 0.0;\n" \
	" return vec4 (color, source.a);\n" \
	"}\n" \
	"vec4 GlideIndexedLevel (highp vec2 uv, int level, float darkness, vec3 chroma, bool holey, bool linear)\n" \
	"{\n" \
	" ivec2 size = textureSize (u_indices, level);\n" \
	" if (!linear) return GlideShadeIndex (GlideIndex (uv, level), darkness, chroma, holey);\n" \
	" highp vec2 p = fract (uv) * vec2 (size) - 0.5;\n" \
	" ivec2 base = ivec2 (floor (p));\n" \
	" vec2 f = fract (p);\n" \
	" uint i00 = texelFetch (u_indices, GlideWrap (base, size), level).r;\n" \
	" uint i10 = texelFetch (u_indices, GlideWrap (base + ivec2 (1, 0), size), level).r;\n" \
	" uint i01 = texelFetch (u_indices, GlideWrap (base + ivec2 (0, 1), size), level).r;\n" \
	" uint i11 = texelFetch (u_indices, GlideWrap (base + ivec2 (1, 1), size), level).r;\n" \
	" vec4 top = mix (GlideShadeIndex (i00, darkness, chroma, holey),\n" \
	"                 GlideShadeIndex (i10, darkness, chroma, holey), f.x);\n" \
	" vec4 bottom = mix (GlideShadeIndex (i01, darkness, chroma, holey),\n" \
	"                    GlideShadeIndex (i11, darkness, chroma, holey), f.x);\n" \
	" return mix (top, bottom, f.y);\n" \
	"}\n" \
	"vec4 GlideIndexed (highp vec2 uv, float darkness, vec3 chroma, bool holey)\n" \
	"{\n" \
	" ivec2 base_size = textureSize (u_indices, 0);\n" \
	" highp vec2 footprint = max (abs (dFdx (uv * vec2 (base_size))),\n" \
	"                             abs (dFdy (uv * vec2 (base_size))));\n" \
	" float max_lod = floor (log2 (float (max (base_size.x, base_size.y))));\n" \
	" float lod = clamp (log2 (max (max (footprint.x, footprint.y), 1.0)) + GlideLod (), 0.0, max_lod);\n" \
	" bool linear = u_filter >= 3;\n" \
	" if (u_filter == 0 || u_filter == 3) lod = 0.0;\n" \
	" if (u_filter == 1 || u_filter == 4)\n" \
	"  return GlideIndexedLevel (uv, int (floor (lod + 0.5)), darkness, chroma, holey, linear);\n" \
	" if (u_filter == 2 || u_filter == 5)\n" \
	" {\n" \
	"  int lo = int (floor (lod));\n" \
	"  int hi = min (lo + 1, int (max_lod));\n" \
	"  return mix (GlideIndexedLevel (uv, lo, darkness, chroma, holey, linear),\n" \
	"              GlideIndexedLevel (uv, hi, darkness, chroma, holey, linear), fract (lod));\n" \
	" }\n" \
	" return GlideIndexedLevel (uv, 0, darkness, chroma, holey, linear);\n" \
	"}\n"

/*
=============================================================================

	world

=============================================================================
*/

static const char glide_world_vert[] =
	GLIDE_VERSION
	GLIDE_VERT_PRECISION
	"layout(location=0) in vec3 a_position;\n"
	"layout(location=1) in vec2 a_texcoord;\n"
	"layout(location=2) in vec2 a_lmcoord;\n"
	"uniform mat4 u_mvp;\n"
	"uniform vec2 u_scroll;\n"
	"out vec2 v_texcoord;\n"
	"out vec2 v_lmcoord;\n"
	"out float v_fogdepth;\n"
	"void main ()\n"
	"{\n"
	" gl_Position = u_mvp * vec4 (a_position, 1.0);\n"
	" v_texcoord = a_texcoord + u_scroll;\n"
	" v_lmcoord = a_lmcoord;\n"
	" v_fogdepth = gl_Position.w;\n"
	"}\n";

static const char glide_world_frag[] =
	GLIDE_VERSION
	GLIDE_FRAG_PRECISION
	"in highp vec2 v_texcoord;\n"
	"in highp vec2 v_lmcoord;\n"
	GLIDE_BAYER_FN
	GLIDE_FOG_FN
	GLIDE_DITHER_FN
	GLIDE_LOD_FN
	GLIDE_PALETTE_FN
	"uniform sampler2D u_lightmap;\n"
	"uniform highp vec2 u_turbscale;\n"
	"uniform highp float u_turbtime;\n"
	"uniform float u_alpha;\n"
	"uniform float u_overbright;\n"
	"uniform float u_light;\n"
	"uniform int u_flags;\n"
	"out vec4 frag_color;\n"
	"void main ()\n"
	"{\n"
	" highp vec2 uv = v_texcoord;\n"
	/* The liquid warp is the classic turbsin table: the surface's
	 * texel coordinates displaced by a sine of the other axis. Liquid
	 * batches hand us texels, everything else hands us normalised
	 * coordinates, so u_turbscale is the only difference. */
	" if ((u_flags & 1) != 0)\n"
	"  uv = (uv + 8.0 * sin (uv.yx * 0.125 + u_turbtime)) * u_turbscale;\n"
	" vec4 lightmap = ((u_flags & 2) != 0) ? texture (u_lightmap, v_lmcoord) : vec4 (u_light);\n"
	" float intensity = ((u_flags & 2) != 0) ? lightmap.a : u_light;\n"
	" vec3 chroma = lightmap.rgb / max (max (lightmap.r, lightmap.g), max (lightmap.b, 0.0001));\n"
	" float darkness = floor (clamp ((255.0 - intensity * 255.0) * 0.25, 0.0, 63.0));\n"
	" uint source_index = GlideIndex (uv, 0);\n"
	" vec4 albedo = GlideIndexed (uv, darkness, chroma, (u_flags & 4) != 0);\n"
	/* '{' textures -- fences, grates, foliage -- are drawn in the opaque
	 * chains with blending off, so the cut-out has to be a discard. */
	" if ((u_flags & 4) != 0 && albedo.a < 0.666)\n"
	"  discard;\n"
	" vec3 color = albedo.rgb;\n"
	" if (u_debug == 1) color = vec3 (float (source_index) / 255.0);\n"
	" else if (u_debug == 2) color = (int (source_index) >= u_fullbright && source_index != 255u) ? vec3 (1.0, 0.0, 1.0) : vec3 (0.0);\n"
	" else if (u_debug == 3) color = vec3 (intensity);\n"
	" else if (u_debug == 4) color = vec3 (darkness / 63.0);\n"
	" frag_color = vec4 (GlideDither (GlideFog (color)), albedo.a * u_alpha);\n"
	"}\n";

/*
=============================================================================

	sky

	Two 128x128 layers projected onto a flattened sphere and scrolled at
	different speeds -- the GLQuake sky, which is what the 3Dfx miniGL
	drivers ran.

=============================================================================
*/

static const char glide_sky_vert[] =
	GLIDE_VERSION
	GLIDE_VERT_PRECISION
	"layout(location=0) in vec3 a_position;\n"
	"uniform mat4 u_mvp;\n"
	"out vec3 v_worldpos;\n"
	"out float v_fogdepth;\n"
	"void main ()\n"
	"{\n"
	" gl_Position = u_mvp * vec4 (a_position, 1.0);\n"
	" v_worldpos = a_position;\n"
	" v_fogdepth = gl_Position.w;\n"
	"}\n";

static const char glide_sky_frag[] =
	GLIDE_VERSION
	GLIDE_FRAG_PRECISION
	"in highp vec3 v_worldpos;\n"
	GLIDE_BAYER_FN
	GLIDE_FOG_FN
	GLIDE_DITHER_FN
	"uniform sampler2D u_diffuse;\n"
	"uniform sampler2D u_texture2;\n"
	"uniform highp vec3 u_eye;\n"
	"uniform highp float u_time;\n"
	"out vec4 frag_color;\n"
	"void main ()\n"
	"{\n"
	" highp vec3 dir = v_worldpos - u_eye;\n"
	" dir.z *= 3.0;\n"
	" highp float scale = 6.0 * 63.0 / max (length (dir), 0.001);\n"
	" highp vec2 base = dir.xy * scale;\n"
	" vec4 solid = texture (u_diffuse, (base + u_time * 8.0) * (1.0 / 128.0));\n"
	" vec4 clouds = texture (u_texture2, (base + u_time * 16.0) * (1.0 / 128.0));\n"
	" vec3 color = mix (solid.rgb, clouds.rgb, clouds.a);\n"
	" frag_color = vec4 (GlideDither (GlideFog (color)), 1.0);\n"
	"}\n";

/*
=============================================================================

	models, sprites and particles

	One program for everything the CPU transformed and lit.

=============================================================================
*/

static const char glide_model_vert[] =
	GLIDE_VERSION
	GLIDE_VERT_PRECISION
	"layout(location=0) in vec3 a_position;\n"
	"layout(location=1) in vec2 a_texcoord;\n"
	"layout(location=3) in vec4 a_color;\n"
	"uniform mat4 u_mvp;\n"
	"out vec2 v_texcoord;\n"
	"out vec4 v_color;\n"
	"out float v_fogdepth;\n"
	"void main ()\n"
	"{\n"
	" gl_Position = u_mvp * vec4 (a_position, 1.0);\n"
	" v_texcoord = a_texcoord;\n"
	" v_color = a_color;\n"
	" v_fogdepth = gl_Position.w;\n"
	"}\n";

static const char glide_model_frag[] =
	GLIDE_VERSION
	GLIDE_FRAG_PRECISION
	"in highp vec2 v_texcoord;\n"
	"in vec4 v_color;\n"
	GLIDE_BAYER_FN
	GLIDE_FOG_FN
	GLIDE_DITHER_FN
	GLIDE_LOD_FN
	GLIDE_PALETTE_FN
	"uniform sampler2D u_diffuse;\n"
	"uniform float u_alpha;\n"
	"uniform int u_flags;\n"
	"out vec4 frag_color;\n"
	"void main ()\n"
	"{\n"
	" float darkness = v_color.a * 255.0 * 0.25;\n"
	" uint source_index = ((u_flags & 4) != 0) ? GlideIndex (v_texcoord, 0) : 0u;\n"
	" vec4 texel = ((u_flags & 4) != 0) ?\n"
	"  GlideIndexed (v_texcoord, darkness, v_color.rgb, (u_flags & 8) != 0) :\n"
	"  texture (u_diffuse, v_texcoord, GlideLod ());\n"
	" if ((u_flags & 1) != 0 && texel.a < 0.666)\n"
	"  discard;\n"
	" vec3 color = ((u_flags & 4) != 0) ? texel.rgb : texel.rgb * v_color.rgb;\n"
	" if ((u_flags & 4) != 0 && u_debug == 1) color = vec3 (float (source_index) / 255.0);\n"
	" else if ((u_flags & 4) != 0 && u_debug == 2) color = (int (source_index) >= u_fullbright && source_index != 255u) ? vec3 (1.0, 0.0, 1.0) : vec3 (0.0);\n"
	" else if ((u_flags & 4) != 0 && u_debug == 3) color = vec3 (1.0 - darkness / 63.0);\n"
	" else if ((u_flags & 4) != 0 && u_debug == 4) color = vec3 (darkness / 63.0);\n"
	/* Additive passes (glow orbs, particle flares) are light, not
	 * surface: fogging them would tint the light source itself. */
	" if ((u_flags & 2) == 0)\n"
	"  color = GlideFog (color);\n"
	" frag_color = vec4 (GlideDither (color), texel.a * v_color.a * u_alpha);\n"
	"}\n";

/*
=============================================================================

	scan-out

	u_flags 1 selects the T-buffer accumulation pass (blend the new frame
	into the retained one); otherwise this is the video pass: 2x2
	postfilter, gamma ramp and palette blend.

=============================================================================
*/

static const char glide_post_vert[] =
	GLIDE_VERSION
	GLIDE_VERT_PRECISION
	"out vec2 v_texcoord;\n"
	"void main ()\n"
	"{\n"
	" vec2 corner = vec2 (float ((gl_VertexID << 1) & 2), float (gl_VertexID & 2));\n"
	" v_texcoord = corner;\n"
	" gl_Position = vec4 (corner * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char glide_post_frag[] =
	GLIDE_VERSION
	GLIDE_FRAG_PRECISION
	"in highp vec2 v_texcoord;\n"
	"uniform sampler2D u_source;\n"
	"uniform sampler2D u_history;\n"
	"uniform highp vec2 u_screensize;\n"
	"uniform highp vec2 u_sourcesize;\n"
	"uniform float u_postfilter;\n"
	"uniform float u_gamma;\n"
	"uniform float u_contrast;\n"
	"uniform float u_blend;\n"
	"uniform vec4 u_tint;\n"
	"uniform vec4 u_crt;\n"
	"uniform int u_flags;\n"
	"out vec4 frag_color;\n"
	/* Barrel distortion, in scan-out texture space. */
	"highp vec2 CrtWarp (highp vec2 uv)\n"
	"{\n"
	" if (u_crt.z <= 0.0) return uv;\n"
	" highp vec2 c = uv * 2.0 - 1.0;\n"
	" highp vec2 off = abs (c.yx) / vec2 (6.0, 5.0) * u_crt.z;\n"
	" c += c * off * off;\n"
	" return c * 0.5 + 0.5;\n"
	"}\n"
	"void main ()\n"
	"{\n"
	" if ((u_flags & 1) != 0)\n"
	" {\n"
	/* T-buffer accumulation pass: retain a share of the previous frame.
	 * The VSA-100 did this in hardware across sub-buffers; here it is one
	 * fullscreen blend, which is what an M-series GPU is happy to do. */
	"  vec3 scene = texture (u_source, v_texcoord).rgb;\n"
	"  vec3 history = texture (u_history, v_texcoord).rgb;\n"
	"  frag_color = vec4 (mix (scene, history, u_blend), 1.0);\n"
	"  return;\n"
	" }\n"
	" highp vec2 uv = CrtWarp (v_texcoord);\n"
	" if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)\n"
	" {\n"
	"  frag_color = vec4 (0.0, 0.0, 0.0, 1.0);\n"
	"  return;\n"
	" }\n"
	/* The scene buffer is resolved here by the sampler: a quarter-res
	 * source is stretched bilinearly, a supersampled one is box-filtered
	 * down. The postfilter is the Voodoo's 2x2 scan-out average, taken
	 * over source texels so it stays a scan-out filter at any scale. */
	" vec3 color = texture (u_source, uv).rgb;\n"
	" if (u_postfilter > 0.0)\n"
	" {\n"
	"  highp vec2 texel = 1.0 / max (u_sourcesize, vec2 (1.0));\n"
	"  vec3 sum = color;\n"
	"  sum += texture (u_source, uv + vec2 (texel.x, 0.0)).rgb;\n"
	"  sum += texture (u_source, uv + vec2 (0.0, texel.y)).rgb;\n"
	"  sum += texture (u_source, uv + texel).rgb;\n"
	"  color = mix (color, sum * 0.25, u_postfilter);\n"
	" }\n"
	" color = mix (color, u_tint.rgb, u_tint.a);\n"
	/* Optional CRT: a scanline per line of the video mode, an aperture
	 * grille in output pixels, and a little vignette. */
	" if (u_crt.x > 0.0 || u_crt.y > 0.0 || u_crt.w > 0.0)\n"
	" {\n"
	"  highp float line = fract (uv.y * max (u_screensize.y, 1.0));\n"
	"  float scan = mix (1.0, 0.55 + 0.45 * sin (line * 3.14159265), u_crt.x);\n"
	"  vec3 grille = vec3 (1.0);\n"
	"  float phase = mod (gl_FragCoord.x, 3.0);\n"
	"  if (phase < 1.0) grille = vec3 (1.0, 0.75, 0.75);\n"
	"  else if (phase < 2.0) grille = vec3 (0.75, 1.0, 0.75);\n"
	"  else grille = vec3 (0.75, 0.75, 1.0);\n"
	"  highp vec2 d = uv - 0.5;\n"
	"  float vignette = 1.0 - u_crt.w * dot (d, d) * 2.0;\n"
	"  color *= scan * mix (vec3 (1.0), grille, u_crt.y) * vignette;\n"
	"  color *= 1.0 + 0.35 * u_crt.x + 0.25 * u_crt.y;\n"
	" }\n"
	/* Contrast, then gamma, in the engine's convention: gamma is the
	 * exponent itself (v_gamma 0.5 is brighter than 1.0), the same
	 * ramp the software renderer bakes into the palette. */
	" color = (color - 0.5) * u_contrast + 0.5;\n"
	" color = pow (max (color, vec3 (0.0)), vec3 (max (u_gamma, 0.1)));\n"
	" frag_color = vec4 (clamp (color, 0.0, 1.0), 1.0);\n"
	"}\n";

/*
=============================================================================

	compilation

=============================================================================
*/

gl2program_t	gl2_world_program;
gl2program_t	gl2_sky_program;
gl2program_t	gl2_model_program;
gl2program_t	gl2_post_program;

static qboolean	gl2_shaders_ready;
static const gl2program_t *gl2_active_program;

static GLuint GL2_CompileStage (const char *name, GLenum type, const char *source)
{
	GLuint	shader;
	GLint	status;
	char	log[1024];

	shader = glCreateShader (type);
	glShaderSource (shader, 1, &source, NULL);
	glCompileShader (shader);
	glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		log[0] = '\0';
		glGetShaderInfoLog (shader, sizeof(log), NULL, log);
		Con_Printf ("WebGlide: %s %s shader failed:\n%s\n", name,
			(type == GL_VERTEX_SHADER) ? "vertex" : "fragment", log);
		glDeleteShader (shader);
		return 0;
	}
	return shader;
}

static qboolean GL2_LinkProgram (gl2program_t *out, const char *name,
					const char *vertex_source, const char *fragment_source)
{
	GLuint	vertex, fragment, program;
	GLint	status;
	char	log[1024];

	memset (out, 0, sizeof(*out));

	vertex = GL2_CompileStage (name, GL_VERTEX_SHADER, vertex_source);
	if (!vertex)
		return false;
	fragment = GL2_CompileStage (name, GL_FRAGMENT_SHADER, fragment_source);
	if (!fragment)
	{
		glDeleteShader (vertex);
		return false;
	}

	program = glCreateProgram ();
	glAttachShader (program, vertex);
	glAttachShader (program, fragment);
	glLinkProgram (program);
	glDeleteShader (vertex);
	glDeleteShader (fragment);
	glGetProgramiv (program, GL_LINK_STATUS, &status);
	if (!status)
	{
		log[0] = '\0';
		glGetProgramInfoLog (program, sizeof(log), NULL, log);
		Con_Printf ("WebGlide: %s program failed to link:\n%s\n", name, log);
		glDeleteProgram (program);
		return false;
	}

	out->program = program;
	out->u_mvp = glGetUniformLocation (program, "u_mvp");
	out->u_diffuse = glGetUniformLocation (program, "u_diffuse");
	out->u_indices = glGetUniformLocation (program, "u_indices");
	out->u_lightmap = glGetUniformLocation (program, "u_lightmap");
	out->u_texture2 = glGetUniformLocation (program, "u_texture2");
	out->u_palette = glGetUniformLocation (program, "u_palette");
	out->u_colormap = glGetUniformLocation (program, "u_colormap");
	out->u_fullbright = glGetUniformLocation (program, "u_fullbright");
	out->u_filter = glGetUniformLocation (program, "u_filter");
	out->u_debug = glGetUniformLocation (program, "u_debug");
	out->u_alpha = glGetUniformLocation (program, "u_alpha");
	out->u_overbright = glGetUniformLocation (program, "u_overbright");
	out->u_light = glGetUniformLocation (program, "u_light");
	out->u_turbtime = glGetUniformLocation (program, "u_turbtime");
	out->u_turbscale = glGetUniformLocation (program, "u_turbscale");
	out->u_fogcolor = glGetUniformLocation (program, "u_fogcolor");
	out->u_fogdensity = glGetUniformLocation (program, "u_fogdensity");
	out->u_flags = glGetUniformLocation (program, "u_flags");
	out->u_scroll = glGetUniformLocation (program, "u_scroll");
	out->u_eye = glGetUniformLocation (program, "u_eye");
	out->u_time = glGetUniformLocation (program, "u_time");
	out->u_source = glGetUniformLocation (program, "u_source");
	out->u_history = glGetUniformLocation (program, "u_history");
	out->u_screensize = glGetUniformLocation (program, "u_screensize");
	out->u_gamma = glGetUniformLocation (program, "u_gamma");
	out->u_contrast = glGetUniformLocation (program, "u_contrast");
	out->u_dither = glGetUniformLocation (program, "u_dither");
	out->u_postfilter = glGetUniformLocation (program, "u_postfilter");
	out->u_blend = glGetUniformLocation (program, "u_blend");
	out->u_tint = glGetUniformLocation (program, "u_tint");
	out->u_lodbias = glGetUniformLocation (program, "u_lodbias");
	out->u_mipdither = glGetUniformLocation (program, "u_mipdither");
	out->u_sourcesize = glGetUniformLocation (program, "u_sourcesize");
	out->u_crt = glGetUniformLocation (program, "u_crt");
	return true;
}

void GL2_ShaderInit (void)
{
	if (gl2_shaders_ready)
		return;

	gl2_active_program = NULL;
	gl2_shaders_ready =
		GL2_LinkProgram (&gl2_world_program, "world", glide_world_vert, glide_world_frag) &&
		GL2_LinkProgram (&gl2_sky_program, "sky", glide_sky_vert, glide_sky_frag) &&
		GL2_LinkProgram (&gl2_model_program, "model", glide_model_vert, glide_model_frag) &&
		GL2_LinkProgram (&gl2_post_program, "post", glide_post_vert, glide_post_frag);

	if (!gl2_shaders_ready)
	{
		/* Without shaders there is no renderer at all. Say so loudly
		 * and leave the engine running: the launcher's WebGlide toggle
		 * is how the player gets back to the software build. */
		Con_Printf ("WebGlide: shader compilation failed; the 3D view will be blank.\n");
		return;
	}

	/* Sampler units are fixed for the life of the process. */
	glUseProgram (gl2_world_program.program);
	glUniform1i (gl2_world_program.u_indices, 0);
	glUniform1i (gl2_world_program.u_lightmap, 1);
	glUniform1i (gl2_world_program.u_palette, 3);
	glUniform1i (gl2_world_program.u_colormap, 4);
	glUseProgram (gl2_sky_program.program);
	glUniform1i (gl2_sky_program.u_diffuse, 0);
	glUniform1i (gl2_sky_program.u_texture2, 2);
	glUseProgram (gl2_model_program.program);
	glUniform1i (gl2_model_program.u_diffuse, 0);
	glUniform1i (gl2_model_program.u_indices, 2);
	glUniform1i (gl2_model_program.u_palette, 3);
	glUniform1i (gl2_model_program.u_colormap, 4);
	glUseProgram (gl2_post_program.program);
	glUniform1i (gl2_post_program.u_source, 0);
	glUniform1i (gl2_post_program.u_history, 1);
	glUseProgram (0);
}

qboolean GL2_ShadersReady (void)
{
	return gl2_shaders_ready;
}

/*
================
GL2_InvalidateProgram

draw_webgl2.c binds its own 2D program straight through glUseProgram, so
the cached "current program" is only valid within one 3D pass.  Called at
the top of every scene.
================
*/
void GL2_InvalidateProgram (void)
{
	gl2_active_program = NULL;
}

void GL2_UseProgram (const gl2program_t *program)
{
	if (gl2_active_program == program)
		return;
	gl2_active_program = program;
	glUseProgram (program ? program->program : 0);
}
