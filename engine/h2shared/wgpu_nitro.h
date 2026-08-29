/*
 * wgpu_nitro.h -- internal interface of the WebGlideNitro renderer.
 *
 * WebGlideNitro is the web port's native WebGPU renderer. It does not present
 * the software rasteriser's framebuffer: it builds its own scene geometry and
 * hands WebGPU batched, indexed triangles. What Nitro must look like is defined
 * by the software rasteriser, and what it costs is measured on the target iPad
 * against Nitro's own captures.
 *
 * WebGlideNitro is the primary renderer. Its design lives in
 * docs/web/WEBGLIDE_NITRO.md; the software rasteriser is parked as its
 * correctness reference.
 *
 * The rules this file encodes:
 *
 *   - Anything that only depends on the map is built once, at load time:
 *     the world vertex buffer (immutable -- it is never given COPY_DST),
 *     every indexed texture, its parameter uniform and its bind group, and
 *     the lightmap texture array.
 *   - A frame costs four calls into JavaScript, one command encoder and two
 *     render passes.  Per-texture work in a pass is one setBindGroup plus
 *     one drawIndexed.
 *   - Shading stays indexed: the diffuse texture is r8uint, the lightmap
 *     selects an authored colormap row, and only then does the palette turn
 *     the result into colour.  That is d_scan.c's arithmetic, on the GPU.
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

#ifndef UHEXEN2_WGPU_NITRO_H
#define UHEXEN2_WGPU_NITRO_H

/*
=============================================================================

	cvars

=============================================================================
*/

extern cvar_t	r_nitro_scenescale;	/* scene buffer scale, 0.25..2 */
extern cvar_t	r_nitro_report;		/* print the slice's content gaps per map */
extern cvar_t	r_nitro_dither;		/* mood-weighted ordered RGB565 dither */
extern cvar_t	r_nitro_resolve;	/* restrained edge-aware 2x2 resolve */
extern cvar_t	r_nitro_persistence;	/* coloured-light history, 0 disables */
extern cvar_t	r_nitro_fogbands;	/* depth steps in authored fog, 0 is smooth */
extern cvar_t	r_nitro_haze;		/* world-space fog pockets, 0 disables */
extern cvar_t	r_nitro_paletteshifts;	/* palette-snap contents and flash shifts */
extern cvar_t	r_nitro_liquidmotion;	/* distinct material motion, 0 is legacy */
extern cvar_t	r_nitro_liquidstipple;	/* ordered liquid translucency, 0 disables */
extern cvar_t	r_nitro_liquidrefract;	/* retained-frame refraction strength */
extern cvar_t	r_nitro_liquidglow;	/* palette-domain lava/portal luminosity */
extern cvar_t	r_nitro_spelleffects;	/* spell-family particle silhouettes */
extern cvar_t	r_nitro_glowhaze;	/* palette-domain luminous haze */
extern cvar_t	r_nitro_projectileribbons; /* velocity-shaped missile silhouettes */
extern cvar_t	r_nitro_shadowmotion;	/* restrained near-black scan-out motion */
extern cvar_t	r_nitro_lightvol;	/* shared coarse world/entity irradiance */
extern cvar_t	r_nitro_lightvol_cell;	/* map-space cell size */
extern cvar_t	r_nitro_lightvol_budget; /* cells resolved per frame */

/*
=============================================================================

	matrices

	Column major, in the order a WGSL mat4x4f wants them.

=============================================================================
*/

typedef struct
{
	float	m[16];
} wgpumatrix_t;

void WGPU_MatrixIdentity (wgpumatrix_t *out);
void WGPU_MatrixMultiply (wgpumatrix_t *out, const wgpumatrix_t *a, const wgpumatrix_t *b);
void WGPU_MatrixFrustum (wgpumatrix_t *out, float fovx, float fovy, float zNear);
void WGPU_MatrixRotate (wgpumatrix_t *out, float degrees, float x, float y, float z);
void WGPU_MatrixTranslate (wgpumatrix_t *out, float x, float y, float z);

/*
=============================================================================

	shared frame state (r_webgpu.c)

=============================================================================
*/

typedef struct
{
	mplane_t	planes[4];
} wgpufrustum_t;

extern wgpufrustum_t	wgpu_frustum;
extern wgpumatrix_t	wgpu_view_projection;
extern int		wgpu_visframecount;
extern mleaf_t		*wgpu_viewleaf;
extern vec3_t		wgpu_modelorg;
extern int		wgpu_frame_batches;
extern int		wgpu_frame_polys;

qboolean WGPU_CullBox (const vec3_t mins, const vec3_t maxs);
void WGPU_GammaContrast (float *gamma, float *contrast);
void WGPU_PolyBlendColor (float *rgba);

/*
=============================================================================

	the JavaScript backend (engine/web/webgpu_nitro.js)

	Every one of these is a real WebGPU object operation, not a GL call
	in disguise.  Buffers are handed whole arrays; nothing here is
	per-primitive.

=============================================================================
*/

/* Texture parameter flags; mirrored by the WGSL in webgpu_nitro.js. */
#define NITROTEX_HOLEY		1u	/* palette index 0 is clear ('{' textures) */
#define NITROTEX_ALPHA		2u	/* palette index 255 is clear (2D pics) */
#define NITROTEX_WRAP		4u	/* repeat instead of clamping */
#define NITROTEX_TURB		8u	/* authored liquid sine warp */
#define NITROTEX_SPECIAL_TRANS	64u	/* packed Hexen II colour/alpha nibbles */
#define NITROTEX_LIQUID_SHIFT	4u
#define NITROTEX_LIQUID_MASK	(3u << NITROTEX_LIQUID_SHIFT)
#define NITROTEX_LIQUID_WATER	(0u << NITROTEX_LIQUID_SHIFT)
#define NITROTEX_LIQUID_SLIME	(1u << NITROTEX_LIQUID_SHIFT)
#define NITROTEX_LIQUID_LAVA	(2u << NITROTEX_LIQUID_SHIFT)
#define NITROTEX_LIQUID_PORTAL	(3u << NITROTEX_LIQUID_SHIFT)

/* Vertex layouts; mirrored by the pipeline descriptors in webgpu_nitro.js. */
typedef struct
{
	float		position[3];
	float		texcoord[2];	/* normalised diffuse coordinates */
	float		lightmap[3];	/* s, t within the layer; layer, or -1 */
} wgpuworld_vertex_t;

typedef struct
{
	float		x, y;
	float		s, t;
	unsigned int	color;		/* RGBA bytes, little endian */
} wgpuui_vertex_t;

typedef struct
{
	float		position[3];
	float		scale;
	unsigned int	color;		/* RGBA bytes, little endian */
	unsigned int	style;		/* NITROPARTICLE_* visual family */
} wgpuparticle_t;
_Static_assert (sizeof(wgpuparticle_t) == 24,
		"wgpuparticle_t must match the WebGPU vertex stride");

#define NITROPARTICLE_CLASSIC	0u
#define NITROPARTICLE_FIRE	1u
#define NITROPARTICLE_ICE	2u
#define NITROPARTICLE_POISON	3u
#define NITROPARTICLE_NECRO	4u

extern wgpuparticle_t	*wgpu_particles;
extern int		wgpu_particle_count;

float R_NitroGlowPhase (const entity_t *entity, int flags);
float R_NitroGlowLightScale (const entity_t *entity, int flags, int style,
			    float frequency);

/*
 * Models and sprites: CPU-transformed, but still indexed.  The vertex
 * carries the colormap row d_polyse.c would have interpolated, not a
 * lit RGB colour, so the fragment shader can run the software
 * rasteriser's lookup -- colormap[row][index] -- unchanged.
 */
typedef struct
{
	float		position[3];
	float		texcoord[2];	/* normalised skin coordinates */
	float		light;		/* colormap row 0..63.75; <0 is unlit */
	unsigned int	shade;		/* byte 0 alpha, byte 1 tint-table row */
	unsigned int	lightcolor;	/* RGB light multiplier, packed as bytes */
} wgpumodel_vertex_t;
_Static_assert (sizeof(wgpumodel_vertex_t) == 32,
		"wgpumodel_vertex_t must match the WebGPU vertex stride");

/* One drawIndexed (world, brush entities) or one draw (2D, models). */
typedef struct
{
	int	texture;
	int	first;
	int	count;
	int	entity;		/* wgpuentity_t block; 0 is the world itself,
				   and the 2D path leaves it zero */
} wgpubatch_t;

/* Model batch flags; mirrored by webgpu_nitro.js. */
#define NITROMODEL_BLEND_ALPHA	1u	/* src-alpha / one-minus-src-alpha */
#define NITROMODEL_BLEND_ADD	2u	/* EF_SPECIAL_TRANS: additive */
#define NITROMODEL_VIEWMODEL	4u	/* compressed depth range */
#define NITROMODEL_GLOW		8u	/* authored additive billboard */
#define NITROMODEL_SHADOW	16u	/* cheap projected alias shadow */
#define NITROMODEL_RIBBON	32u	/* velocity-aligned projectile wake */

/* DRF_TRANSLUCENT selects the software renderer's tinttab2.lmp half blend.
 * It is an authored entity property, not a liquid-alpha control. */
#define NITRO_DRF_ALPHA		0.5f

typedef struct
{
	int		texture;
	int		first;		/* first vertex, not index */
	int		count;
	unsigned int	flags;
} wgpumodelbatch_t;

/*
 * Per-entity block for the world pipeline.  Brush entities draw straight
 * out of the map's immutable vertex buffer -- every submodel shares
 * cl.worldmodel->surfaces -- so all that changes per entity is this
 * block, bound with a dynamic offset.  WebGPU's minimum uniform buffer
 * offset alignment is 256 bytes, so the padding is part of the contract
 * and the whole arena uploads in one writeBuffer.
 */
#define NITRO_ENTITY_ALIGN	256

typedef struct
{
	float	mvp[16];
	float	alpha;
	float	light;		/* flat MLS light 0..1; <0 uses the lightmap */
	float	pad[2];
	float	align[NITRO_ENTITY_ALIGN / 4 - 20];
} wgpuentity_t;
_Static_assert (sizeof(wgpuentity_t) == NITRO_ENTITY_ALIGN,
		"wgpuentity_t must match WebGPU's dynamic uniform stride");

/* The whole per-frame scene description, uploaded in one go. */
#define NITROSCENE_FULLBRIGHT	1
#define NITROSCENE_MODEL_FULLBRIGHTS	2

typedef struct
{
	float	mvp[16];
	float	tint[4];
	float	gamma;
	float	contrast;
	int	scene_width;
	int	scene_height;
	int	dest_x;
	int	dest_y;
	int	dest_width;
	int	dest_height;
	int	flags;
	float	particle_right[4];
	float	particle_up[4];
	float	sky_eye[3];
	float	sky_time;
	float	fog_color[3];
	float	fog_density;
	float	fog_bands;
	float	haze;
	float	scan_dither;
	float	scan_resolve;
	float	scan_persistence;
	float	scan_paletteshifts;
	float	liquid_motion;
	float	liquid_stipple;
	float	liquid_refract;
	float	liquid_glow;
	float	glow_haze;
	float	shadow_motion;
} wgpuscene_t;
_Static_assert (sizeof(wgpuscene_t) == 57 * sizeof(float),
		"wgpuscene_t must remain a packed 57-word JavaScript contract");
_Static_assert (offsetof(wgpuscene_t, liquid_motion) == 51 * sizeof(float),
		"wgpuscene_t liquid controls must match JavaScript offsets");

typedef struct
{
	float	screen_width;
	float	screen_height;
	float	gamma;
	float	contrast;
} wgpuui_params_t;

/*
 * Everything the scene pass draws, gathered by the C side and handed over
 * as one description.  Keeping it in a struct is what keeps a frame at
 * four calls into JavaScript however much is on screen.
 *
 * Both batch lists mark where entity gathering switches from opaque to
 * translucent. The backend additionally defers alpha liquids, glows and
 * shadows that share the opaque gathering phase.
 */
typedef struct
{
	const unsigned int	*indices;
	int			indexcount;
	const wgpubatch_t	*batches;
	int			batchcount;
	int			opaquebatches;
	const wgpuentity_t	*entities;
	int			entitycount;
	const wgpumodel_vertex_t *modelvertices;
	int			modelvertexcount;
	const wgpumodelbatch_t	*modelbatches;
	int			modelbatchcount;
	int			opaquemodelbatches;
	const wgpuparticle_t	*particles;
	int			particlecount;
} wgpuscenedata_t;

extern int  Nitro_Init (void);
extern void Nitro_Shutdown (void);
extern void Nitro_Resize (int width, int height);
extern void Nitro_SetPalette (const byte *palette);
extern void Nitro_SetColormap (const byte *colormap, int rows);
extern void Nitro_SetTintTable (const byte *table);
extern int  Nitro_CreateTexture (const char *name, int width, int height,
			const byte *pixels, unsigned int flags);
extern int  Nitro_CreateSky (const char *name, int width, int height,
			const byte *solid, const byte *clouds);
extern void Nitro_UpdateTexture (int texture, const byte *pixels);
extern void Nitro_DestroyTexture (int texture);
extern int  Nitro_CreateWorld (const float *vertices, int vertexcount,
			int maxindices, int lightmaplayers, int lightmapsize);
extern void Nitro_UploadLightmap (int layer, const byte *texels);
extern void Nitro_DestroyWorld (void);
extern void Nitro_BeginFrame (void);
extern void Nitro_DrawScene (const wgpuscene_t *params, const wgpuscenedata_t *data);
extern void Nitro_EndFrame (const wgpuui_params_t *params, const wgpuui_vertex_t *vertices,
			int vertexcount, const wgpubatch_t *runs, int runcount,
			int sceneruncount);

/*
=============================================================================

	the static world and brush entities (wgpu_world.c)

=============================================================================
*/

#define NITRO_LIGHTMAP_SIZE	128
#define NITRO_MAX_LIGHTMAPS	128

void WGPUWorld_NewMap (void);
void WGPUWorld_Shutdown (void);
void WGPUWorld_UpdateLightstyles (void);
void WGPUWorld_PushDlights (void);
void WGPUWorld_BeginScene (void);
void WGPUWorld_DrawWorld (void);
void WGPUWorld_DrawBrushEntity (entity_t *entity);
void WGPUWorld_EndOpaque (void);
void WGPUWorld_SubmitScene (wgpuscene_t *scene);
qboolean WGPUWorld_Ready (void);
void WGPUWorld_ReportGaps (void);
int  WGPUWorld_LightPoint (const vec3_t point, vec3_t lightspot);
int  WGPUWorld_LightPointColor (const vec3_t point, vec3_t lightspot, vec3_t color);
int  WGPUWorld_TraceLight (const vec3_t start, const vec3_t end, vec3_t lightspot);
int  WGPUWorld_TraceLightColor (const vec3_t start, const vec3_t end,
			vec3_t lightspot, vec3_t color);
qboolean WGPUWorld_LineVisible (const vec3_t start, const vec3_t end);

/*
=============================================================================

	the shared coarse light volume (wgpu_lightvol.c)

=============================================================================
*/

typedef struct
{
	byte	direction[3];	/* incoming light vector, encoded from [-1,1] */
	byte	ambient;
	byte	color[3];	/* RGB multiplier, encoded at 1.0 == 64 */
	byte	pad;
} wgpulightcell_t;
_Static_assert (sizeof(wgpulightcell_t) == 8, "light cells must stay compact");

typedef struct
{
	float	ambient;
	float	shade;
	vec3_t	direction;
	vec3_t	color;		/* luminance-normalised dynamic-light colour */
} wgpulightsample_t;

void WGPULightVol_NewMap (void);
void WGPULightVol_Shutdown (void);
void WGPULightVol_BeginFrame (void);
qboolean WGPULightVol_Sample (const vec3_t point, wgpulightsample_t *sample);
void WGPULightVol_ApplyDynamic (const vec3_t point, wgpulightsample_t *sample);
qboolean WGPULightVol_Active (void);
void WGPULightVol_Stats (int *cells, int *resolved, int *cellsize);

/*
=============================================================================

	models, sprites and the view weapon (wgpu_entity.c)

=============================================================================
*/

void WGPUEntity_NewMap (void);
void WGPUEntity_Shutdown (void);
void WGPUEntity_BeginScene (void);
void WGPUEntity_DrawEntitiesOnList (qboolean translucent);
void WGPUEntity_DrawViewModel (void);
void WGPUEntity_SceneData (wgpuscenedata_t *data);
void WGPUEntity_EndOpaque (void);

/*
=============================================================================

	the 2D layer (draw_webgpu.c)

=============================================================================
*/

void WGPUDraw_BeginScene (void);
void WGPUDraw_EndFrame (void);
int WGPUDraw_LoadTexture (const char *name, const byte *pixels, int width,
			int height, unsigned int flags);

#endif	/* UHEXEN2_WGPU_NITRO_H */
