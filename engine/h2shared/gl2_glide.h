/*
 * gl2_glide.h -- internal interface of the WebGlide renderer.
 *
 * WebGlide is the web port's experimental GPU renderer.  The brief is the
 * mid-90s dream rather than mid-90s silicon: the look and feel of Hexen II
 * as it was sold to us on the back of a 3Dfx box -- filtered textures,
 * coloured light, translucent water, fog you can see the world through --
 * realised with as much of the hardware under it as we can actually use.
 * It is NOT the shipping renderer -- the 8bpp software rasteriser is (see
 * docs/web/SOFTWARE_RENDERER.md) -- and it is only reachable through the
 * launcher's WebGlide toggle.
 *
 * The rules that follow from that brief:
 *
 *   - The CPU does all transform, lighting and clipping for models,
 *     sprites and particles, and hands the GPU nothing but textured,
 *     pre-lit triangles.  That is what Glide wanted, and on one WASM
 *     thread it is also simply the cheapest thing that works.
 *   - The world is the exception: its geometry never moves, so it lives in
 *     a static vertex buffer and is drawn with per-texture index batches.
 *   - Everything expensive that only depends on the assets -- mip chains,
 *     alpha fringe repair, fullbright masks, lightmap atlases, .lit
 *     colour -- is done once at load time, never per frame.
 *   - The scene is rendered into an offscreen buffer that may be larger
 *     than the display (gl_glide_supersample) and resolved on scan-out.
 *     Free antialiasing is not period-correct; it is, however, what the
 *     brochure promised.
 *   - The period look is a set of scan-out choices, not a limitation: the
 *     16bpp ordered dither, the 2x2 "22-bit" postfilter, the T-buffer
 *     blur and the optional CRT are all cvars.
 *
 * See docs/web/WEBGLIDE.md.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
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

#ifndef UHEXEN2_GL2_GLIDE_H
#define UHEXEN2_GL2_GLIDE_H

/*
=============================================================================

	cvars

=============================================================================
*/

extern cvar_t	gl_glide_dither;	/* 16bpp ordered dither */
extern cvar_t	gl_glide_postfilter;	/* the 2x2 "22-bit" scan-out filter */
extern cvar_t	gl_glide_lodbias;	/* grTexLodBiasValue() */
extern cvar_t	gl_glide_gamma;		/* the Voodoo gamma ramp */
extern cvar_t	gl_glide_tbuffer;	/* VSA-100 T-buffer accumulation */
extern cvar_t	gl_glide_motionblur;	/* T-buffer temporal blend, 0..0.9 */
extern cvar_t	gl_glide_fogtable;	/* GR_FOG_WITH_TABLE emulation */
extern cvar_t	gl_glide_colordepth;	/* 16 = dithered, 32 = straight RGBA8 */
extern cvar_t	gl_glide_mipmapdither;	/* Voodoo Graphics mip dithering */
extern cvar_t	gl_glide_supersample;	/* scene buffer scale, 1..2 */
extern cvar_t	gl_glide_anisotropy;	/* max anisotropy, 1 = off */
extern cvar_t	gl_glide_crt;		/* scanline strength, 0 = off */
extern cvar_t	gl_glide_crt_mask;	/* aperture grille strength */
extern cvar_t	gl_glide_crt_curve;	/* barrel distortion */
extern cvar_t	gl_glide_crt_vignette;	/* corner falloff */

/*
=============================================================================

	matrices (gl2_glide.c)

	Column-major 4x4, laid out the way glUniformMatrix4fv wants them.

=============================================================================
*/

typedef struct
{
	float	m[16];
} gl2matrix_t;

void GL2_MatrixIdentity (gl2matrix_t *out);
void GL2_MatrixMultiply (gl2matrix_t *out, const gl2matrix_t *a, const gl2matrix_t *b);
void GL2_MatrixFrustum (gl2matrix_t *out, float fovx, float fovy, float zNear);
void GL2_MatrixOrtho (gl2matrix_t *out, float left, float right, float bottom,
			float top, float zNear, float zFar);
void GL2_MatrixRotate (gl2matrix_t *out, float degrees, float x, float y, float z);
void GL2_MatrixTranslate (gl2matrix_t *out, float x, float y, float z);
void GL2_MatrixScale (gl2matrix_t *out, float x, float y, float z);

/*
=============================================================================

	textures (gl2_texture.c)

=============================================================================
*/

#define GL2TEX_MIPMAP		(1u << 0)	/* build and use a mip chain */
#define GL2TEX_ALPHA		(1u << 1)	/* palette index 255 is clear */
#define GL2TEX_HOLEY		(1u << 2)	/* palette index 0 is clear */
#define GL2TEX_CLAMP		(1u << 3)
#define GL2TEX_NEAREST		(1u << 4)	/* never filter (particles, sky) */
#define GL2TEX_PERSIST		(1u << 6)	/* survives a map change */
#define GL2TEX_RGBA		(1u << 7)	/* source is already 32bpp */

typedef struct gl2texture_s
{
	GLuint			id;
	int			width, height;
	unsigned int		flags;
	char			name[MAX_QPATH];
	struct gl2texture_s	*next;
} gl2texture_t;

void GL2_TextureInit (void);
gl2texture_t *GL2_LoadTexture (const char *name, int width, int height,
				const byte *data, unsigned int flags);
gl2texture_t *GL2_FindTexture (const char *name);
void GL2_FreeMapTextures (void);
void GL2_Bind (int unit, gl2texture_t *texture);
void GL2_BindName (int unit, GLuint texture);
void GL2_InvalidateBindings (void);
void GL2_ApplyTextureMode (void);
gl2texture_t *GL2_ParticleTexture (void);
gl2texture_t *GL2_WhiteTexture (void);

/*
=============================================================================

	shaders (gl2_shader.c)

	Four programs, which is all a fixed-function-era renderer needs:
	the world, the sky, everything the CPU transformed, and scan-out.

=============================================================================
*/

typedef struct
{
	GLuint	program;
	GLint	u_mvp;
	GLint	u_diffuse;
	GLint	u_lightmap;
	GLint	u_texture2;
	GLint	u_alpha;
	GLint	u_overbright;
	GLint	u_turbtime;
	GLint	u_turbscale;
	GLint	u_fogcolor;
	GLint	u_fogdensity;
	GLint	u_flags;
	GLint	u_scroll;
	GLint	u_eye;
	GLint	u_time;
	GLint	u_source;
	GLint	u_history;
	GLint	u_screensize;
	GLint	u_gamma;
	GLint	u_contrast;
	GLint	u_dither;
	GLint	u_postfilter;
	GLint	u_blend;
	GLint	u_tint;
	GLint	u_lodbias;
	GLint	u_mipdither;
	GLint	u_outputsize;
	GLint	u_crt;
} gl2program_t;

extern gl2program_t	gl2_world_program;
extern gl2program_t	gl2_sky_program;
extern gl2program_t	gl2_model_program;
extern gl2program_t	gl2_post_program;

void GL2_ShaderInit (void);
void GL2_UseProgram (const gl2program_t *program);
void GL2_InvalidateProgram (void);
qboolean GL2_ShadersReady (void);

/* World shader feature bits, mirrored in the GLSL source. */
#define GL2_WORLDFLAG_TURB	1
#define GL2_WORLDFLAG_LIGHTMAP	2

/* Model shader feature bits. */
#define GL2_MODELFLAG_ALPHATEST	1
#define GL2_MODELFLAG_NOFOG	2

/*
=============================================================================

	fog (gl_fog.c, shared with the desktop GL renderer)

	Hexen II's svc_fog, realised as a Glide fog table: the Voodoo faded
	to a constant colour over a table indexed by 1/w, which is an
	exponential-squared curve in eye distance.  gl_fog.c already owns
	the protocol, the worldspawn key and the fade, and is renderer
	agnostic, so WebGlide compiles it as-is and reads the two globals
	it publishes.

=============================================================================
*/

extern float	r_fog_density;
extern float	r_fog_color[3];

void Fog_Init (void);
void Fog_NewMap (void);
void Fog_SetupFrame (void);
float *Fog_GetColor (void);
float Fog_GetDensity (void);
void Fog_StartAdditive (void);
void Fog_StopAdditive (void);

/*
=============================================================================

	frame buffer and scan-out (gl2_glide.c)

=============================================================================
*/

void GL2_GlideInit (void);
void GL2_GlideBeginScene (int x, int y, int width, int height);
void GL2_GlideEndScene (void);
void GL2_GlideShutdown (void);
void GL2_SetupProgramFog (const gl2program_t *program);
/* Fog plus the dither, LOD bias and mip dither every scene program shares. */
void GL2_SetupSceneUniforms (const gl2program_t *program);

/*
=============================================================================

	world (gl2_world.c)

=============================================================================
*/

void GL2_WorldNewMap (void);
void GL2_WorldShutdown (void);
void GL2_BuildLightmaps (void);
void GL2_DrawWorld (void);
void GL2_DrawBrushEntity (entity_t *entity);
void GL2_SkyNewMap (texture_t *texture);
qboolean GL2_WorldHasColoredLight (void);

/* Returns the average intensity at a point and fills colour with what the
 * lightmap -- or the map's .lit file -- says the light there looks like. */
int GL2_LightPoint (const vec3_t point, vec3_t color);
void GL2_PushDlights (void);
void GL2_AnimateLight (void);

/*
=============================================================================

	models, sprites and particles (gl2_alias.c)

=============================================================================
*/

typedef struct
{
	float	x, y, z;
	float	s, t;
	byte	color[4];
} gl2vertex_t;

/* How a batch reaches the frame buffer. */
#define GL2_BLEND_OPAQUE	0
#define GL2_BLEND_ALPHA		1
#define GL2_BLEND_ADD		2	/* light, not surface: glows and flares */

void GL2_ModelInit (void);
void GL2_ModelShutdown (void);
void GL2_BeginModelFrame (void);
void GL2_EndModelFrame (void);
void GL2_BeginModelBatch (gl2texture_t *texture, unsigned int shaderflags,
				float alpha, int blend);
gl2vertex_t *GL2_ModelVertices (int count);
void GL2_FlushModelBatch (void);
void GL2_DrawAliasModel (entity_t *entity);
void GL2_DrawSpriteModel (entity_t *entity);
void GL2_DrawParticleList (particle_t *first);

/*
=============================================================================

	shared frame state (r_webgl2.c)

=============================================================================
*/

typedef struct
{
	mplane_t	planes[4];
} gl2frustum_t;

extern gl2frustum_t	gl2_frustum;
extern gl2matrix_t	gl2_view_projection;
extern int		gl2_visframecount;
extern int		gl2_dlightframecount;
extern mleaf_t		*gl2_viewleaf;
extern entity_t		*gl2_currententity;
extern vec3_t		gl2_modelorg;
extern float		gl2_frametime;
extern int		gl2_scene_width, gl2_scene_height;
extern int		gl2_frame_polys;
extern int		gl2_frame_batches;

qboolean GL2_CullBox (const vec3_t mins, const vec3_t maxs);
void GL2_RotateForEntity (gl2matrix_t *out, const vec3_t origin, const vec3_t angles);
void GL2_PolyBlendColor (float *rgba);

#endif	/* UHEXEN2_GL2_GLIDE_H */
