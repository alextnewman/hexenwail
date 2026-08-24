/*
 * r_webgpu.c -- WebGlideNitro: the web port's native WebGPU renderer.
 *
 * This is the WEBGPUQUAKE counterpart of r_webgl2.c.  It shares that file's
 * shape -- the same renderer-policy cvars, the same frustum, the same
 * GLQuake axis convention -- because the shared client is written against
 * that surface, and for no other reason.  Everything below the frame setup
 * is native WebGPU: there is no GL context, no GL header, and no call in
 * this build that a GL driver would recognise.
 *
 * WebGlide is a reference for *scene preparation* only, and an optional one.
 * What is borrowed is the answer to "which surfaces are in this frame, and
 * what does the atlas look like" -- questions about Hexen II, not about an
 * API.  WebGlide is an abortive experiment: its frame cost is explicitly not
 * a baseline, gate or optimisation criterion here, and Nitro's own numbers
 * come from captures taken on the target iPad.  The output specification is
 * the software rasteriser.
 *
 * What this renderer does not yet draw is stated out loud in the console at
 * every map load; see WGPUWorld_ReportGaps() in wgpu_world.c and
 * docs/web/WEBGLIDE_NITRO.md.
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

#include "quakedef.h"
#include "wgpu_nitro.h"
#include "web_perf.h"

/* What the scene buffer is allowed to cost before the ladder shrinks it,
 * the same two-ceiling policy WebGlide uses (gl2_glide.c). */
#define NITRO_MAX_SCENE_PIXELS	(2560 * 1440)

refdef_t r_refdef;
vec3_t r_origin, vpn, vright, vup;
entity_t r_worldentity;
texture_t *r_notexture_mip;

int glx, gly, glwidth, glheight;
int c_brush_polys, c_alias_polys;
int d_lightstylevalue[256];
int r_framecount, r_visframecount;
int r_pixbytes = 1;

cvar_t r_norefresh = {"r_norefresh", "0", CVAR_NONE};
cvar_t r_drawentities = {"r_drawentities", "1", CVAR_NONE};
cvar_t r_drawworld = {"r_drawworld", "1", CVAR_NONE};
cvar_t r_drawviewmodel = {"r_drawviewmodel", "1", CVAR_NONE};
cvar_t r_speeds = {"r_speeds", "0", CVAR_NONE};
cvar_t r_waterwarp = {"r_waterwarp", "1", CVAR_NONE};
cvar_t r_fullbright = {"r_fullbright", "0", CVAR_NONE};
cvar_t r_lightmap = {"r_lightmap", "0", CVAR_NONE};
cvar_t r_dynamic = {"r_dynamic", "1", CVAR_NONE};
/* Shared with software and deliberately not archived: both renderers feed
 * this value into the same authored colormap darkness-row calculation. */
cvar_t r_ambient = {"r_ambient", "0", CVAR_NONE};
cvar_t r_novis = {"r_novis", "0", CVAR_NONE};
cvar_t r_lerpmodels = {"r_lerpmodels", "1", CVAR_NONE};
cvar_t r_lerp_viewmodel = {"r_lerp_viewmodel", "1", CVAR_NONE};
cvar_t r_texture_external = {"r_texture_external", "0", CVAR_NONE};
cvar_t r_texture_external_hud = {"r_texture_external_hud", "0", CVAR_NONE};
cvar_t r_wateralpha = {"r_wateralpha", "1", CVAR_ARCHIVE};
cvar_t r_lavaalpha = {"r_lavaalpha", "0", CVAR_NONE};
cvar_t r_slimealpha = {"r_slimealpha", "0", CVAR_NONE};
cvar_t r_telealpha = {"r_telealpha", "0", CVAR_NONE};
cvar_t r_turbalpha = {"r_turbalpha", "1", CVAR_NONE};
cvar_t r_shadows = {"r_shadows", "1", CVAR_ARCHIVE};
cvar_t gl_overbright = {"gl_overbright", "1", CVAR_NONE};
cvar_t gl_missile_glows = {"gl_missile_glows", "1", CVAR_ARCHIVE};
cvar_t gl_flashintensity = {"gl_flashintensity", "1", CVAR_ARCHIVE};
cvar_t gl_torch_dlight = {"gl_torch_dlight", "1", CVAR_ARCHIVE};
cvar_t gl_extra_dynamic_lights = {"gl_extra_dynamic_lights", "1", CVAR_ARCHIVE};
cvar_t gl_particles = {"gl_particles", "1", CVAR_ARCHIVE};
cvar_t gl_fullbrights = {"gl_fullbrights", "1", CVAR_ARCHIVE};
cvar_t gl_glows = {"gl_glows", "1", CVAR_ARCHIVE};
cvar_t gl_other_glows = {"gl_other_glows", "1", CVAR_ARCHIVE};
cvar_t gl_glow_intensity = {"gl_glow_intensity", "1", CVAR_ARCHIVE};
cvar_t gl_fxaa = {"gl_fxaa", "0", CVAR_ARCHIVE};
cvar_t gl_overbright_models = {"gl_overbright_models", "1", CVAR_ARCHIVE};
cvar_t gl_coloredlight = {"gl_coloredlight", "1", CVAR_ARCHIVE};
cvar_t gl_flashblend = {"gl_flashblend", "0", CVAR_ARCHIVE};
cvar_t gl_texture_anisotropy = {"gl_texture_anisotropy", "1", CVAR_ARCHIVE};
cvar_t gl_texturemode = {"gl_texturemode", "GL_LINEAR_MIPMAP_LINEAR", CVAR_ARCHIVE};
cvar_t r_dither = {"r_dither", "0", CVAR_ARCHIVE};
cvar_t r_hdr = {"r_hdr", "0", CVAR_ARCHIVE};
cvar_t r_hdr_exposure = {"r_hdr_exposure", "1", CVAR_ARCHIVE};
cvar_t r_lightmap_bicubic = {"r_lightmap_bicubic", "0", CVAR_ARCHIVE};
cvar_t r_motionblur = {"r_motionblur", "0", CVAR_ARCHIVE};
cvar_t r_scale = {"r_scale", "1", CVAR_ARCHIVE};
cvar_t r_softemu = {"r_softemu", "0", CVAR_ARCHIVE};

/* Nitro's own knobs.  The scene buffer is decoupled from the canvas for the
 * same reason WebGlide decouples it: the panel is far denser than the
 * rasteriser's native idea of a pixel, and the scan-out pass is the cheapest
 * place to resolve that. */
cvar_t r_nitro_scenescale = {"r_nitro_scenescale", "1", CVAR_ARCHIVE};
cvar_t r_nitro_report = {"r_nitro_report", "1", CVAR_ARCHIVE};
cvar_t r_nitro_dither = {"r_nitro_dither", "1", CVAR_ARCHIVE};
cvar_t r_nitro_resolve = {"r_nitro_resolve", "1", CVAR_ARCHIVE};
cvar_t r_nitro_persistence = {"r_nitro_persistence", "0.06", CVAR_ARCHIVE};
cvar_t r_nitro_lightvol = {"r_nitro_lightvol", "1", CVAR_ARCHIVE};
cvar_t r_nitro_lightvol_cell = {"r_nitro_lightvol_cell", "64", CVAR_ARCHIVE};
cvar_t r_nitro_lightvol_budget = {"r_nitro_lightvol_budget", "32", CVAR_ARCHIVE};

int gl_filter_idx;
int gl_max_anisotropy = 1;
byte *playerTranslation;
const int color_offsets[MAX_PLAYER_CLASS] = {
	2 * 14 * 256, 0, 1 * 14 * 256, 2 * 14 * 256, 2 * 14 * 256
};

qboolean r_cache_thrash;

wgpufrustum_t wgpu_frustum;
wgpumatrix_t wgpu_view_projection;
int wgpu_visframecount;
mleaf_t *wgpu_viewleaf;
vec3_t wgpu_modelorg;
int wgpu_frame_batches;
int wgpu_frame_polys;
wgpuparticle_t *wgpu_particles;
int wgpu_particle_count;

static qboolean	wgpu_frame_open;

extern float	r_fog_density;
extern float	r_fog_color[3];
void Fog_Init (void);
void Fog_NewMap (void);
void Fog_SetupFrame (void);
static qboolean wgpu_initialized;
static int wgpu_particle_max;

/*
=============================================================================

	per-entity PimpModel overrides

	model.h declares this table as renderer-owned; every renderer front end
	carries a copy with identical behaviour, because pr_cmds.c / pr_edict.c
	drive it from QuakeC.

=============================================================================
*/

static pimp_override_t	pimp_overrides[MAX_EDICTS];
static float		null_glow_settings[GLOW_SETTINGS_COUNT];

void R_ClearPimpOverrides (void)
{
	memset (pimp_overrides, 0, sizeof(pimp_overrides));
}

pimp_override_t *R_GetPimpOverride (int entnum)
{
	if (entnum < 0 || entnum >= MAX_EDICTS)
		return NULL;
	return &pimp_overrides[entnum];
}

int R_GetEntityModelFlags (entity_t *e)
{
	int entnum = (int)(e - cl_entities);

	if (entnum >= 0 && entnum < MAX_EDICTS && pimp_overrides[entnum].active
		&& pimp_overrides[entnum].trail_override)
	{
		return pimp_overrides[entnum].trail_flags;
	}
	return e->model ? e->model->flags : 0;
}

int R_GetPimpFlags (entity_t *e, float **gsettings_out)
{
	int entnum = (int)(e - cl_entities);

	if (entnum >= 0 && entnum < MAX_EDICTS && pimp_overrides[entnum].active)
	{
		if (gsettings_out)
			*gsettings_out = pimp_overrides[entnum].glow_settings;
		return pimp_overrides[entnum].ex_flags | (e->model ? e->model->ex_flags : 0);
	}
	if (gsettings_out)
		*gsettings_out = e->model ? e->model->glow_settings : null_glow_settings;
	return e->model ? e->model->ex_flags : 0;
}

/*
=============================================================================

	matrices

	Column major, in the order a WGSL mat4x4f wants them.  Identical to
	WebGlide's helpers except where WebGPU's clip space differs, which is
	called out where it happens.

=============================================================================
*/

void WGPU_MatrixIdentity (wgpumatrix_t *out)
{
	memset (out, 0, sizeof(*out));
	out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
}

void WGPU_MatrixMultiply (wgpumatrix_t *out, const wgpumatrix_t *a, const wgpumatrix_t *b)
{
	wgpumatrix_t	result;
	int		row, col;

	for (col = 0; col < 4; col++)
	{
		for (row = 0; row < 4; row++)
		{
			result.m[col * 4 + row] =
				a->m[0 * 4 + row] * b->m[col * 4 + 0] +
				a->m[1 * 4 + row] * b->m[col * 4 + 1] +
				a->m[2 * 4 + row] * b->m[col * 4 + 2] +
				a->m[3 * 4 + row] * b->m[col * 4 + 3];
		}
	}
	*out = result;
}

/*
================
WGPU_MatrixFrustum

An infinite far plane, as WebGlide uses, for the same reason: Hexen II's
maps are large and there is nothing to gain from a finite one.

The one number that differs from the GL version is m[14].  WebGPU's clip
space runs z from 0 at the near plane to 1 at the far one, not -1 to 1, so
the depth row is the "reversed-range" form: z_clip = -z_eye - zNear, and
w_clip = -z_eye, which maps z_eye = -zNear to 0 and z_eye -> -infinity
to 1.  Using GL's -2*zNear here would clip everything in front of the eye.
================
*/
void WGPU_MatrixFrustum (wgpumatrix_t *out, float fovx, float fovy, float zNear)
{
	float	w, h;

	if (fovx < 1.0f) fovx = 1.0f;
	if (fovx > 179.0f) fovx = 179.0f;
	if (fovy < 1.0f) fovy = 1.0f;
	if (fovy > 179.0f) fovy = 179.0f;

	w = 1.0f / tanf (fovx * (float)M_PI / 360.0f);
	h = 1.0f / tanf (fovy * (float)M_PI / 360.0f);

	memset (out, 0, sizeof(*out));
	out->m[0] = w;
	out->m[5] = h;
	out->m[10] = -1.0f;
	out->m[11] = -1.0f;
	out->m[14] = -zNear;
}

void WGPU_MatrixRotate (wgpumatrix_t *out, float degrees, float x, float y, float z)
{
	float	radians = degrees * (float)M_PI / 180.0f;
	float	c = cosf (radians);
	float	s = sinf (radians);
	float	length = sqrtf (x * x + y * y + z * z);

	WGPU_MatrixIdentity (out);
	if (length < 0.000001f)
		return;
	x /= length;
	y /= length;
	z /= length;

	out->m[0] = x * x * (1.0f - c) + c;
	out->m[1] = y * x * (1.0f - c) + z * s;
	out->m[2] = x * z * (1.0f - c) - y * s;
	out->m[4] = x * y * (1.0f - c) - z * s;
	out->m[5] = y * y * (1.0f - c) + c;
	out->m[6] = y * z * (1.0f - c) + x * s;
	out->m[8] = x * z * (1.0f - c) + y * s;
	out->m[9] = y * z * (1.0f - c) - x * s;
	out->m[10] = z * z * (1.0f - c) + c;
}

void WGPU_MatrixTranslate (wgpumatrix_t *out, float x, float y, float z)
{
	WGPU_MatrixIdentity (out);
	out->m[12] = x;
	out->m[13] = y;
	out->m[14] = z;
}

/*
=============================================================================

	frame setup

=============================================================================
*/

static int WGPU_SignbitsForPlane (const mplane_t *out)
{
	int	bits = 0, j;

	for (j = 0; j < 3; j++)
	{
		if (out->normal[j] < 0)
			bits |= 1 << j;
	}
	return bits;
}

static void WGPU_TurnVector (vec3_t out, const vec3_t forward, const vec3_t side,
				float angle)
{
	float	scale_forward = (float) cos (angle * M_PI / 180.0);
	float	scale_side = (float) sin (angle * M_PI / 180.0);

	out[0] = scale_forward * forward[0] + scale_side * side[0];
	out[1] = scale_forward * forward[1] + scale_side * side[1];
	out[2] = scale_forward * forward[2] + scale_side * side[2];
}

static void WGPU_SetFrustum (float fovx, float fovy)
{
	int	i;

	WGPU_TurnVector (wgpu_frustum.planes[0].normal, vpn, vright, fovx / 2 - 90);
	WGPU_TurnVector (wgpu_frustum.planes[1].normal, vpn, vright, 90 - fovx / 2);
	WGPU_TurnVector (wgpu_frustum.planes[2].normal, vpn, vup, 90 - fovy / 2);
	WGPU_TurnVector (wgpu_frustum.planes[3].normal, vpn, vup, fovy / 2 - 90);

	for (i = 0; i < 4; i++)
	{
		wgpu_frustum.planes[i].type = PLANE_ANYZ;
		wgpu_frustum.planes[i].dist = DotProduct (r_origin, wgpu_frustum.planes[i].normal);
		wgpu_frustum.planes[i].signbits = (byte) WGPU_SignbitsForPlane (&wgpu_frustum.planes[i]);
	}
}

qboolean WGPU_CullBox (const vec3_t mins, const vec3_t maxs)
{
	int	i;

	for (i = 0; i < 4; i++)
	{
		if (BOX_ON_PLANE_SIDE(*(vec3_t *)mins, *(vec3_t *)maxs,
					&wgpu_frustum.planes[i]) == 2)
			return true;
	}
	return false;
}

/*
================
WGPU_GammaContrast

v_gamma is an exponent, not a multiplier: the software renderer bakes
pow(c, v_gamma) into the palette, so values below 1 brighten.  Nitro cannot
bake it -- the palette texture has to stay the authored one so that the
colormap indexes it correctly -- so the scan-out and 2D shaders apply it.
================
*/
void WGPU_GammaContrast (float *gamma, float *contrast)
{
	float	g = v_gamma.value;

	if (g < 0.3f)
		g = 0.3f;
	if (g > 3.0f)
		g = 3.0f;
	*gamma = g;
	*contrast = q_max(v_contrast.value, 0.1f);
}

/*
================
WGPU_PolyBlendColor

v_blend[] only exists under GLQUAKE, which the web build does not define,
so recompute the damage/powerup tint here with V_CalcBlend's own maths.
The scan-out pass applies it as a full-screen tint.
================
*/
void WGPU_PolyBlendColor (float *rgba)
{
	float	r = 0, g = 0, b = 0, a = 0, a2;
	int	j;

	for (j = 0; j < NUM_CSHIFTS; j++)
	{
		if (cl.cshifts[j].percent > 10000)
			cl.cshifts[j].percent = 80;

		a2 = cl.cshifts[j].percent / 255.0f;
		if (!a2)
			continue;

		a = a + a2 * (1 - a);
		a2 = a2 / a;
		r = r * (1 - a2) + cl.cshifts[j].destcolor[0] * a2;
		g = g * (1 - a2) + cl.cshifts[j].destcolor[1] * a2;
		b = b * (1 - a2) + cl.cshifts[j].destcolor[2] * a2;
	}

	rgba[0] = r / 255.0f;
	rgba[1] = g / 255.0f;
	rgba[2] = b / 255.0f;
	rgba[3] = CLAMP(0.0f, a, 1.0f);
}

/*
================
WGPU_SetupViewProjection

Quake's world axes are not the renderer's: +X forward, +Y left, +Z up.  The
two leading rotations are GLQuake's, and they are what makes every later
"why is my model mirrored" question answer itself.
================
*/
static void WGPU_SetupViewProjection (void)
{
	wgpumatrix_t	projection, view, temp, rot;

	WGPU_MatrixFrustum (&projection, r_refdef.fov_x, r_refdef.fov_y, 4.0f);

	WGPU_MatrixRotate (&view, -90, 1, 0, 0);	/* put Z going up */
	WGPU_MatrixRotate (&rot, 90, 0, 0, 1);
	WGPU_MatrixMultiply (&temp, &view, &rot);

	WGPU_MatrixRotate (&rot, -r_refdef.viewangles[2], 1, 0, 0);
	WGPU_MatrixMultiply (&view, &temp, &rot);
	WGPU_MatrixRotate (&rot, -r_refdef.viewangles[0], 0, 1, 0);
	WGPU_MatrixMultiply (&temp, &view, &rot);
	WGPU_MatrixRotate (&rot, -r_refdef.viewangles[1], 0, 0, 1);
	WGPU_MatrixMultiply (&view, &temp, &rot);

	WGPU_MatrixTranslate (&rot, -r_refdef.vieworg[0], -r_refdef.vieworg[1],
				-r_refdef.vieworg[2]);
	WGPU_MatrixMultiply (&temp, &view, &rot);

	WGPU_MatrixMultiply (&wgpu_view_projection, &projection, &temp);
}

static void WGPU_SetupFrame (void)
{
	r_framecount++;
	wgpu_frame_polys = 0;
	wgpu_frame_batches = 0;

	VectorCopy (r_refdef.vieworg, r_origin);
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

	if (cl.worldmodel)
		wgpu_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);

	WGPU_SetFrustum (r_refdef.fov_x, r_refdef.fov_y);
	WGPU_SetupViewProjection ();

	c_brush_polys = c_alias_polys = 0;
}

/*
================
WGPU_SetupScene

Fills in everything about the frame that is not the geometry: where the
scene lands on the canvas, how big the scene buffer itself is, and the two
scan-out corrections.

r_refdef.vrect is in UI space with a top-left origin, and so is a WebGPU
viewport, so unlike the GL path there is no vertical flip here.
================
*/
static void WGPU_SetupScene (wgpuscene_t *scene)
{
	float	sx = (vid.width > 0) ? ((float)glwidth / (float)vid.width) : 1.0f;
	float	sy = (vid.height > 0) ? ((float)glheight / (float)vid.height) : 1.0f;
	float	scale;
	double	pixels;
	int	scene_w, scene_h;

	memset (scene, 0, sizeof(*scene));
	memcpy (scene->mvp, wgpu_view_projection.m, sizeof(scene->mvp));

	scene->dest_x = (int)(r_refdef.vrect.x * sx + 0.5f);
	scene->dest_y = (int)(r_refdef.vrect.y * sy + 0.5f);
	scene->dest_width = q_max((int)(r_refdef.vrect.width * sx + 0.5f), 1);
	scene->dest_height = q_max((int)(r_refdef.vrect.height * sy + 0.5f), 1);

	scale = r_nitro_scenescale.value;
	if (scale < 0.25f)
		scale = 0.25f;
	if (scale > 2.0f)
		scale = 2.0f;

	scene_w = q_max((int)(scene->dest_width * scale + 0.5f), 1);
	scene_h = q_max((int)(scene->dest_height * scale + 0.5f), 1);

	/* Scale the request rather than crop it, exactly as WebGlide does. */
	pixels = (double)scene_w * (double)scene_h;
	if (pixels > NITRO_MAX_SCENE_PIXELS)
	{
		float	shrink = (float)sqrt (NITRO_MAX_SCENE_PIXELS / pixels);

		scene_w = q_max((int)(scene_w * shrink), 1);
		scene_h = q_max((int)(scene_h * shrink), 1);
	}

	scene->scene_width = scene_w;
	scene->scene_height = scene_h;

	WGPU_GammaContrast (&scene->gamma, &scene->contrast);
	WGPU_PolyBlendColor (scene->tint);

	if (r_fullbright.integer || r_lightmap.integer)
		scene->flags |= NITROSCENE_FULLBRIGHT;
	if (gl_fullbrights.integer)
		scene->flags |= NITROSCENE_MODEL_FULLBRIGHTS;

	VectorScale (vright, 1.5f, scene->particle_right);
	VectorScale (vup, 1.5f, scene->particle_up);
	VectorCopy (r_origin, scene->sky_eye);
	scene->sky_time = (float)fmod (cl.time, 32768.0);
	VectorCopy (r_fog_color, scene->fog_color);
	scene->fog_density = r_fog_density;
	scene->scan_dither = CLAMP (0.0f, r_nitro_dither.value, 1.0f);
	scene->scan_resolve = CLAMP (0.0f, r_nitro_resolve.value, 1.0f);
	scene->scan_persistence = CLAMP (0.0f, r_nitro_persistence.value, 0.25f);
}

/*
=============================================================================

	renderer entry points

=============================================================================
*/

static void Web_RegisterRendererCvars (void)
{
	Cvar_RegisterVariable(&r_norefresh);
	Cvar_RegisterVariable(&r_drawentities);
	Cvar_RegisterVariable(&r_drawworld);
	Cvar_RegisterVariable(&r_drawviewmodel);
	Cvar_RegisterVariable(&r_speeds);
	Cvar_RegisterVariable(&r_waterwarp);
	Cvar_RegisterVariable(&r_fullbright);
	Cvar_RegisterVariable(&r_lightmap);
	Cvar_RegisterVariable(&r_dynamic);
	Cvar_RegisterVariable(&r_ambient);
	Cvar_RegisterVariable(&r_novis);
	Cvar_RegisterVariable(&r_lerpmodels);
	Cvar_RegisterVariable(&r_lerp_viewmodel);
	Cvar_RegisterVariable(&r_texture_external);
	Cvar_RegisterVariable(&r_texture_external_hud);
	Cvar_RegisterVariable(&r_wateralpha);
	Cvar_RegisterVariable(&r_lavaalpha);
	Cvar_RegisterVariable(&r_slimealpha);
	Cvar_RegisterVariable(&r_telealpha);
	Cvar_RegisterVariable(&r_turbalpha);
	Cvar_RegisterVariable(&r_shadows);
	Cvar_RegisterVariable(&gl_overbright);
	Cvar_RegisterVariable(&gl_missile_glows);
	Cvar_RegisterVariable(&gl_flashintensity);
	Cvar_RegisterVariable(&gl_torch_dlight);
	Cvar_RegisterVariable(&gl_extra_dynamic_lights);
	Cvar_RegisterVariable(&gl_particles);
	Cvar_RegisterVariable(&gl_fullbrights);
	Cvar_RegisterVariable(&gl_glows);
	Cvar_RegisterVariable(&gl_other_glows);
	Cvar_RegisterVariable(&gl_glow_intensity);
	Cvar_RegisterVariable(&gl_fxaa);
	Cvar_RegisterVariable(&gl_overbright_models);
	Cvar_RegisterVariable(&gl_coloredlight);
	Cvar_RegisterVariable(&gl_flashblend);
	Cvar_RegisterVariable(&gl_texture_anisotropy);
	Cvar_RegisterVariable(&gl_texturemode);
	Cvar_RegisterVariable(&r_dither);
	Cvar_RegisterVariable(&r_hdr);
	Cvar_RegisterVariable(&r_hdr_exposure);
	Cvar_RegisterVariable(&r_lightmap_bicubic);
	Cvar_RegisterVariable(&r_motionblur);
	Cvar_RegisterVariable(&r_scale);
	Cvar_RegisterVariable(&r_softemu);

	Cvar_RegisterVariable(&r_nitro_lightvol);
	Cvar_RegisterVariable(&r_nitro_lightvol_cell);
	Cvar_RegisterVariable(&r_nitro_lightvol_budget);
	Cvar_RegisterVariable(&r_nitro_scenescale);
	Cvar_RegisterVariable(&r_nitro_report);
	Cvar_RegisterVariable(&r_nitro_dither);
	Cvar_RegisterVariable(&r_nitro_resolve);
	Cvar_RegisterVariable(&r_nitro_persistence);
}

void WebGPU_Init (void)
{
	if (wgpu_initialized)
		return;
	if (!Nitro_Init ())
		Sys_Error ("WebGPU was not initialized by the launcher");
	wgpu_initialized = true;
}

void WebGPU_Shutdown (void)
{
	if (!wgpu_initialized)
		return;
	WGPUEntity_Shutdown ();
	WGPUWorld_Shutdown ();
	Nitro_Shutdown ();
	free (wgpu_particles);
	wgpu_particles = NULL;
	wgpu_particle_count = 0;
	wgpu_particle_max = 0;
	wgpu_initialized = false;
	wgpu_frame_open = false;
}

void WebGPU_Resize (int width, int height)
{
	glx = gly = 0;
	glwidth = width;
	glheight = height;
	Nitro_Resize (width, height);
}

/*
================
WebGPU_BeginFrame

Opens the frame's command encoder.  Idempotent, because a frame that never
reaches R_RenderView -- the menu, the loading screen -- still has to reach
the canvas.
================
*/
void WebGPU_BeginFrame (void)
{
	if (wgpu_frame_open)
		return;
	Nitro_BeginFrame ();
	wgpu_frame_open = true;
}

void WebGPU_EndFrame (void)
{
	WebGPU_BeginFrame ();
	WGPUDraw_EndFrame ();
	wgpu_frame_open = false;
}

void R_InitTextures (void)
{
	int x, y, m;
	byte *dest;

	r_notexture_mip = Hunk_AllocName(sizeof(texture_t) + 16 * 16 +
		8 * 8 + 4 * 4 + 2 * 2, "notexture");
	q_strlcpy(r_notexture_mip->name, "notexture", sizeof(r_notexture_mip->name));
	r_notexture_mip->width = r_notexture_mip->height = 16;
	dest = (byte *)(r_notexture_mip + 1);
	for (m = 0; m < 4; ++m)
	{
		r_notexture_mip->offsets[m] = (unsigned int)(dest - (byte *)r_notexture_mip);
		for (y = 0; y < (16 >> m); ++y)
			for (x = 0; x < (16 >> m); ++x)
				*dest++ = ((x < (8 >> m)) ^ (y < (8 >> m))) ? 0 : 255;
	}
}

void R_Init (void)
{
	byte *tinttable;

	Web_RegisterRendererCvars();
	R_InitParticles();
	playerTranslation = FS_LoadHunkFile("gfx/player.lmp", NULL);
	if (!playerTranslation)
		Sys_Error("Couldn't load gfx/player.lmp");
	WebGPU_Init();
	Fog_Init();

	/* gfx/tinttab.lmp is the 256x256 index-to-index table R_AliasDrawModel
	 * builds globalcolormap from when an entity has a colorshade.  Handing
	 * it to the backend is what lets Nitro keep the coloured glows indexed
	 * instead of multiplying an RGB tint over the palette result.  Missing
	 * data is a visual no-op: the backend starts from the identity. */
	tinttable = FS_LoadHunkFile("gfx/tinttab.lmp", NULL);
	if (tinttable && fs_filesize == 256 * 256)
		Nitro_SetTintTable(tinttable);
}

void R_NewMap (void)
{
	int i;

	for (i = 0; i < 256; ++i)
		d_lightstylevalue[i] = 264;

	memset(&r_worldentity, 0, sizeof(r_worldentity));
	r_worldentity.model = cl.worldmodel;
	Mod_RestoreAliasModelDefaults();
	R_ClearPimpOverrides();
	R_ClearParticles();

	wgpu_viewleaf = NULL;
	wgpu_visframecount = 0;

	WGPUWorld_NewMap();
	WGPUEntity_NewMap();
	Fog_NewMap();
	WGPUWorld_ReportGaps();
}

static void WGPU_AnimateLight (void)
{
	int	i, c, v;
	int	defaultLocus;
	int	locusHz[3];

	defaultLocus = locusHz[0] = (int)(cl.time * 10);
	locusHz[1] = (int)(cl.time * 20);
	locusHz[2] = (int)(cl.time * 30);
	for (i = 0; i < MAX_LIGHTSTYLES; i++)
	{
		if (!cl_lightstyle[i].length)
		{
			d_lightstylevalue[i] = 256;
			continue;
		}
		c = cl_lightstyle[i].map[0];
		if (c == '1' || c == '2' || c == '3')
		{
			/* Hexen II uses the first character as an explicit 10/20/30 Hz
			 * rate tag; only the remaining characters are light frames. */
			if (cl_lightstyle[i].length == 1)
			{
				d_lightstylevalue[i] = 256;
				continue;
			}
			v = locusHz[c - '1'] % (cl_lightstyle[i].length - 1);
			d_lightstylevalue[i] = (cl_lightstyle[i].map[v + 1] - 'a') * 22;
			continue;
		}
		v = defaultLocus % cl_lightstyle[i].length;
		d_lightstylevalue[i] = (cl_lightstyle[i].map[v] - 'a') * 22;
	}
}

void R_RenderView (void)
{
	wgpuscene_t	scene;

	if (r_norefresh.integer)
		return;
	if (!cl.worldmodel)
		return;

	WebGPU_BeginFrame ();
	WGPU_SetupFrame ();
	WGPU_AnimateLight ();
	WGPULightVol_BeginFrame ();
	Fog_SetupFrame ();
	WGPU_SetupScene (&scene);
	WGPUWorld_UpdateLightstyles ();
	wgpu_particle_count = 0;
	R_DrawParticles ();

	/* The frame is gathered opaque-first and submitted once.  Both batch
	 * lists record where they stop being opaque, so the backend can draw
	 * world, opaque entities, translucent entities, view model and
	 * particles in that order out of two arenas. */
	WGPUWorld_BeginScene ();
	WGPUEntity_BeginScene ();

	if (r_drawworld.integer)
		WGPUWorld_DrawWorld ();
	WGPUEntity_DrawEntitiesOnList (false);

	WGPUWorld_EndOpaque ();
	WGPUEntity_EndOpaque ();

	WGPUEntity_DrawEntitiesOnList (true);
	WGPUEntity_DrawViewModel ();

	WGPUWorld_SubmitScene (&scene);

	if (wgpu_viewleaf)
		V_SetContentsColor(wgpu_viewleaf->contents);
}

void R_ViewChanged (float aspect) { (void)aspect; }

/*
===============
R_SetVrect

pvrectin is the full screen rect, pvrect is the 3D view rect to fill in --
the same in/out order as the software rasteriser's R_SetVrect in r_main.c.
There is no 8/2 pixel alignment to honour on a GPU, so the view simply
fills the screen above the status bar strip.
===============
*/
void R_SetVrect (vrect_t *pvrectin, vrect_t *pvrect, int lineadj)
{
	*pvrect = *pvrectin;
	pvrect->height -= lineadj;
	if (pvrect->width < 1)
		pvrect->width = 1;
	if (pvrect->height < 1)
		pvrect->height = 1;
}

/*
===============
R_PushDlights

Marks world surfaces touched by the active dynamic lights.  The lightmap
update path rebuilds those rectangles and uploads each dirty atlas page once.
===============
*/
void R_PushDlights (void)
{
	WGPUWorld_PushDlights ();
}

/*
===============
R_InitSky

A sky texture is 256x128, with the *left* half the masked overlay and the
right half the solid background (see r_sky.c).  The slice draws the solid
half only, unscrolled; wgpu_world.c does the split when it uploads the
texture, so there is nothing to prepare here.
===============
*/
void R_InitSky (texture_t *texture)
{
	(void) texture;
}

void R_LerpEntity (entity_t *entity, vec3_t origin, vec3_t angles)
{
	VectorCopy(entity->origin, origin);
	VectorCopy(entity->angles, angles);
}

void D_FlushCaches (void) {}
void D_DeleteSurfaceCache (void) {}
void D_InitCaches (void *buffer, int size) { (void)buffer; (void)size; }
int D_SurfaceCacheForRes (int width, int height) { (void)width; (void)height; return 0; }
void D_EnableBackBufferAccess (void) {}
void D_DisableBackBufferAccess (void) {}

/*
================
WebGPU_DrawParticles

Builds one compact instance per particle.  WebGPU expands each instance into
a camera-facing quad in the vertex shader, avoiding six CPU-transformed
vertices per particle while retaining the software renderer's palette colour
and distance-scaled size.
================
*/
void WebGPU_DrawParticles (particle_t *first)
{
	particle_t	*particle;

	if (!first || !gl_particles.integer)
		return;

	for (particle = first; particle; particle = particle->next)
	{
		wgpuparticle_t	*out;
		unsigned int	color;
		vec3_t		delta;
		int		index = (int)particle->color & 0x01ff;
		float		scale;

		if (wgpu_particle_count == wgpu_particle_max)
		{
			wgpu_particle_max = wgpu_particle_max ? wgpu_particle_max * 2 : 1024;
			wgpu_particles = (wgpuparticle_t *) realloc (wgpu_particles,
				(size_t)wgpu_particle_max * sizeof(*wgpu_particles));
			if (!wgpu_particles)
				Sys_Error ("WebGlideNitro: out of memory for %d particles",
						wgpu_particle_max);
		}

		color = index < 256 ? d_8to24table[index] :
				d_8to24TranslucentTable[index - 256];
		if (index < 256)
			color |= 0xff000000u;

		VectorSubtract (particle->org, r_origin, delta);
		scale = 1.0f + DotProduct (delta, vpn) * 0.004f;
		scale = CLAMP(1.0f, scale, 12.0f);

		out = &wgpu_particles[wgpu_particle_count++];
		VectorCopy (particle->org, out->position);
		out->scale = scale;
		out->color = color;
	}

	WebPerf_CountDraw (wgpu_particle_count * 2);
	WebPerf_CountUpload ((size_t)wgpu_particle_count * sizeof(*wgpu_particles));
}
