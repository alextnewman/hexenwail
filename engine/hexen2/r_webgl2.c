#include "quakedef.h"
#include "gl2_glide.h"

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
int gl_filter_idx;
int gl_max_anisotropy = 1;
webgl2_caps_t gl_renderer_caps;
byte *playerTranslation;
const int color_offsets[MAX_PLAYER_CLASS] = {
	2 * 14 * 256, 0, 1 * 14 * 256, 2 * 14 * 256, 2 * 14 * 256
};

qboolean r_cache_thrash;

/* Per-entity PimpModel overrides, keyed by entity number.  Written by the
 * pimpmodel() QuakeC builtin (PF_pimpmodel) and cleared per map; the
 * WebGL2 renderer does not consume the glow/trail data yet. */
static pimp_override_t	pimp_overrides[MAX_EDICTS];

void R_ClearPimpOverrides (void)
{
	memset(pimp_overrides, 0, sizeof(pimp_overrides));
}

pimp_override_t *R_GetPimpOverride (int entnum)
{
	if (entnum < 0 || entnum >= MAX_EDICTS)
		return NULL;
	return &pimp_overrides[entnum];
}

// Returns model flags for an entity, with per-entity trail overrides
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

// Returns combined ex_flags for an entity (pimp override | model defaults)
// and sets *gsettings_out to the active glow settings
static float null_glow_settings[GLOW_SETTINGS_COUNT];
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

	WebGlide cvars

	The Glide-era knobs, and they default to *on*.  WebGlide is not a
	1:1 GL port with optional period garnish; it is what Hexen II might
	have looked like if the artists had targeted Glide and shipped a
	reference renderer for it.  So the shipped configuration is the
	period look: 16bpp output through the ordered dither, the 2x2
	scan-out filter over it, a sharpening LOD bias with Voodoo Graphics
	mip dithering, a T-buffer trail and a CRT.  The scene is drawn at
	quarter resolution, which is both what the hardware did and what a
	phone GPU wants.

	Every one of these is archived, so any of it can be dialled back
	from the console without a rebuild; docs/web/WEBGLIDE.md has the
	table and the reasoning.  Being archived also means a config.cfg
	written by an older build wins over these defaults, so a returning
	install may need them set explicitly once.

=============================================================================
*/

cvar_t gl_glide_dither = {"gl_glide_dither", "1", CVAR_ARCHIVE};
cvar_t gl_glide_postfilter = {"gl_glide_postfilter", "1", CVAR_ARCHIVE};
cvar_t gl_glide_lodbias = {"gl_glide_lodbias", "-0.5", CVAR_ARCHIVE};
cvar_t gl_glide_gamma = {"gl_glide_gamma", "1", CVAR_ARCHIVE};
cvar_t gl_glide_tbuffer = {"gl_glide_tbuffer", "1", CVAR_ARCHIVE};
cvar_t gl_glide_motionblur = {"gl_glide_motionblur", "0.25", CVAR_ARCHIVE};
cvar_t gl_glide_fogtable = {"gl_glide_fogtable", "1", CVAR_ARCHIVE};
cvar_t gl_glide_colordepth = {"gl_glide_colordepth", "16", CVAR_ARCHIVE};
cvar_t gl_glide_mipmapdither = {"gl_glide_mipmapdither", "1", CVAR_ARCHIVE};
cvar_t gl_glide_scenescale = {"gl_glide_scenescale", "0.5", CVAR_ARCHIVE};
cvar_t gl_glide_anisotropy = {"gl_glide_anisotropy", "8", CVAR_ARCHIVE};
cvar_t gl_glide_crt = {"gl_glide_crt", "0.35", CVAR_ARCHIVE};
cvar_t gl_glide_crt_mask = {"gl_glide_crt_mask", "0.35", CVAR_ARCHIVE};
cvar_t gl_glide_crt_curve = {"gl_glide_crt_curve", "0.15", CVAR_ARCHIVE};
cvar_t gl_glide_crt_vignette = {"gl_glide_crt_vignette", "0.2", CVAR_ARCHIVE};
cvar_t gl_glide_debug = {"gl_glide_debug", "0", CVAR_NONE};

/*
=============================================================================

	shared frame state

=============================================================================
*/

gl2frustum_t	gl2_frustum;
gl2matrix_t	gl2_view_projection;
int		gl2_visframecount;
int		gl2_dlightframecount;
mleaf_t		*gl2_viewleaf;
entity_t	*gl2_currententity;
vec3_t		gl2_modelorg;
float		gl2_time;
int		gl2_frame_polys;
int		gl2_frame_batches;

static mleaf_t	*gl2_oldviewleaf;

/*
================
GL2_FilterChanged

gl_texturemode and gl_glide_anisotropy are sampled once per texture, at
load time, so a change has to be pushed back over everything already
resident.
================
*/
static void GL2_FilterChanged (cvar_t *var)
{
	(void) var;
	GL2_ApplyTextureMode ();
}

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

	Cvar_RegisterVariable(&gl_glide_dither);
	Cvar_RegisterVariable(&gl_glide_postfilter);
	Cvar_RegisterVariable(&gl_glide_lodbias);
	Cvar_RegisterVariable(&gl_glide_gamma);
	Cvar_RegisterVariable(&gl_glide_tbuffer);
	Cvar_RegisterVariable(&gl_glide_motionblur);
	Cvar_RegisterVariable(&gl_glide_fogtable);
	Cvar_RegisterVariable(&gl_glide_colordepth);
	Cvar_RegisterVariable(&gl_glide_mipmapdither);
	Cvar_RegisterVariable(&gl_glide_scenescale);
	Cvar_RegisterVariable(&gl_glide_anisotropy);
	Cvar_RegisterVariable(&gl_glide_crt);
	Cvar_RegisterVariable(&gl_glide_crt_mask);
	Cvar_RegisterVariable(&gl_glide_crt_curve);
	Cvar_RegisterVariable(&gl_glide_crt_vignette);
	Cvar_RegisterVariable(&gl_glide_debug);

	Cvar_SetCallback(&gl_texturemode, GL2_FilterChanged);
	Cvar_SetCallback(&gl_glide_anisotropy, GL2_FilterChanged);
}


/*
=============================================================================

	frame setup

=============================================================================
*/

static int GL2_SignbitsForPlane (const mplane_t *out)
{
	int	bits = 0, j;

	for (j = 0; j < 3; j++)
	{
		if (out->normal[j] < 0)
			bits |= 1 << j;
	}
	return bits;
}

static void GL2_TurnVector (vec3_t out, const vec3_t forward, const vec3_t side,
				float angle)
{
	float	scale_forward = (float) cos (angle * M_PI / 180.0);
	float	scale_side = (float) sin (angle * M_PI / 180.0);

	out[0] = scale_forward * forward[0] + scale_side * side[0];
	out[1] = scale_forward * forward[1] + scale_side * side[1];
	out[2] = scale_forward * forward[2] + scale_side * side[2];
}

static void GL2_SetFrustum (float fovx, float fovy)
{
	int	i;

	GL2_TurnVector (gl2_frustum.planes[0].normal, vpn, vright, fovx / 2 - 90);
	GL2_TurnVector (gl2_frustum.planes[1].normal, vpn, vright, 90 - fovx / 2);
	GL2_TurnVector (gl2_frustum.planes[2].normal, vpn, vup, 90 - fovy / 2);
	GL2_TurnVector (gl2_frustum.planes[3].normal, vpn, vup, fovy / 2 - 90);

	for (i = 0; i < 4; i++)
	{
		gl2_frustum.planes[i].type = PLANE_ANYZ;
		gl2_frustum.planes[i].dist = DotProduct (r_origin, gl2_frustum.planes[i].normal);
		gl2_frustum.planes[i].signbits = (byte) GL2_SignbitsForPlane (&gl2_frustum.planes[i]);
	}
}

qboolean GL2_CullBox (const vec3_t mins, const vec3_t maxs)
{
	int	i;

	for (i = 0; i < 4; i++)
	{
		if (BOX_ON_PLANE_SIDE(*(vec3_t *)mins, *(vec3_t *)maxs,
					&gl2_frustum.planes[i]) == 2)
			return true;
	}
	return false;
}

/*
================
GL2_RotateForEntity

GLQuake's order: yaw about Z, -pitch about Y, roll about X, after the
translation.
================
*/
void GL2_RotateForEntity (gl2matrix_t *out, const vec3_t origin, const vec3_t angles)
{
	gl2matrix_t	temp, rot;

	GL2_MatrixTranslate (out, origin[0], origin[1], origin[2]);

	GL2_MatrixRotate (&rot, angles[1], 0, 0, 1);
	GL2_MatrixMultiply (&temp, out, &rot);
	GL2_MatrixRotate (&rot, -angles[0], 0, 1, 0);
	GL2_MatrixMultiply (out, &temp, &rot);
	GL2_MatrixRotate (&rot, angles[2], 1, 0, 0);
	GL2_MatrixMultiply (&temp, out, &rot);
	*out = temp;
}

/*
================
GL2_PolyBlendColor

v_blend[] only exists under GLQUAKE, which the web build does not define,
so recompute the damage/powerup tint here with V_CalcBlend's own maths.
The scan-out pass applies it as a full-screen tint.
================
*/
void GL2_PolyBlendColor (float *rgba)
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
GL2_SetupViewProjection

Quake's world axes are not GL's: +X forward, +Y left, +Z up.  The two
leading rotations are GLQuake's, and they are what makes every later
"why is my model mirrored" question answer itself.
================
*/
static void GL2_SetupViewProjection (void)
{
	gl2matrix_t	projection, view, temp, rot;

	GL2_MatrixFrustum (&projection, r_refdef.fov_x, r_refdef.fov_y, 4.0f);

	GL2_MatrixRotate (&view, -90, 1, 0, 0);		/* put Z going up */
	GL2_MatrixRotate (&rot, 90, 0, 0, 1);
	GL2_MatrixMultiply (&temp, &view, &rot);

	GL2_MatrixRotate (&rot, -r_refdef.viewangles[2], 1, 0, 0);
	GL2_MatrixMultiply (&view, &temp, &rot);
	GL2_MatrixRotate (&rot, -r_refdef.viewangles[0], 0, 1, 0);
	GL2_MatrixMultiply (&temp, &view, &rot);
	GL2_MatrixRotate (&rot, -r_refdef.viewangles[1], 0, 0, 1);
	GL2_MatrixMultiply (&view, &temp, &rot);

	GL2_MatrixTranslate (&rot, -r_refdef.vieworg[0], -r_refdef.vieworg[1],
				-r_refdef.vieworg[2]);
	GL2_MatrixMultiply (&temp, &view, &rot);

	GL2_MatrixMultiply (&gl2_view_projection, &projection, &temp);
}

/*
================
GL2_SetupFrame
================
*/
static void GL2_SetupFrame (void)
{
	r_framecount++;
	gl2_frame_polys = 0;
	gl2_frame_batches = 0;

	/* The liquid warp and the sky scroll are absolute phases, not deltas:
	 * the shaders take cl.time straight, as the desktop GL path does. */
	gl2_time = (float)cl.time;

	VectorCopy (r_refdef.vieworg, r_origin);
	AngleVectors (r_refdef.viewangles, vpn, vright, vup);

	gl2_oldviewleaf = gl2_viewleaf;
	if (cl.worldmodel)
		gl2_viewleaf = Mod_PointInLeaf (r_origin, cl.worldmodel);
	(void) gl2_oldviewleaf;

	GL2_SetFrustum (r_refdef.fov_x, r_refdef.fov_y);
	GL2_SetupViewProjection ();

	Fog_SetupFrame ();
	GL2_AnimateLight ();

	c_brush_polys = c_alias_polys = 0;
}

/*
=============================================================================

	entity lists

=============================================================================
*/

static qboolean GL2_EntityIsTranslucent (const entity_t *entity)
{
	if (!entity->model)
		return false;
	if (entity->drawflags & DRF_TRANSLUCENT)
		return true;
	if (entity->model->flags & (EF_TRANSPARENT | EF_SPECIAL_TRANS))
		return true;
	if (entity->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(entity->alpha))
		return true;
	return entity->model->type == mod_sprite;
}

static void GL2_DrawEntity (entity_t *entity)
{
	gl2_currententity = entity;

	switch (entity->model->type)
	{
	case mod_brush:
		GL2_FlushModelBatch ();
		GL2_DrawBrushEntity (entity);
		break;
	case mod_alias:
		GL2_DrawAliasModel (entity);
		break;
	case mod_sprite:
		GL2_DrawSpriteModel (entity);
		break;
	default:
		break;
	}
}

static void GL2_DrawEntitiesOnList (qboolean translucent)
{
	int	i;

	if (!r_drawentities.integer)
		return;

	for (i = 0; i < cl_numvisedicts; i++)
	{
		entity_t	*entity = cl_visedicts[i];

		if (!entity || !entity->model)
			continue;
		if (entity == &cl.viewent)
			continue;
		if (GL2_EntityIsTranslucent (entity) != translucent)
			continue;
		GL2_DrawEntity (entity);
	}

	GL2_FlushModelBatch ();
}

/*
================
GL2_DrawViewModel

Drawn last, into the near 30% of the depth range, so the weapon cannot
poke through a wall the player is standing against.
================
*/
static void GL2_DrawViewModel (void)
{
	entity_t	*entity = &cl.viewent;

	if (!entity->model)
		return;
	cl.light_level = GL2_ViewModelLightLevel (entity);
	if (cl.v.health <= 0 || chase_active.integer || !r_drawviewmodel.integer ||
	    !r_drawentities.integer || scr_viewsize.integer >= 140)
		return;

	glDepthRangef (0.0f, 0.3f);
	GL2_DrawEntity (entity);
	GL2_FlushModelBatch ();
	glDepthRangef (0.0f, 1.0f);
}

/*
=============================================================================

	renderer entry points

=============================================================================
*/

void WebGL2_Init (void)
{
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	GL2_TextureInit();
	GL2_ShaderInit();
	GL2_GlideInit();
	GL2_ModelInit();
}

void WebGL2_Shutdown (void)
{
	GL2_WorldShutdown();
	GL2_ModelShutdown();
	GL2_GlideShutdown();
}

void WebGL2_Resize (int width, int height)
{
	glx = gly = 0;
	glwidth = width;
	glheight = height;
	glViewport(0, 0, width, height);
}

void WebGL2_BeginFrame (void)
{
	/* The canvas itself is cleared by the browser between composites
	 * (preserveDrawingBuffer is false), and the scene buffer is cleared
	 * in GL2_GlideBeginScene, so there is nothing to do here but make
	 * sure the 2D layer is looking at the whole canvas. */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, glwidth, glheight);
}

void WebGL2_EndFrame (void)
{
	Draw_FlushCharBatch();
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
	Web_RegisterRendererCvars();
	Fog_Init();
	R_InitParticles();
	playerTranslation = FS_LoadHunkFile("gfx/player.lmp", NULL);
	if (!playerTranslation)
		Sys_Error("Couldn't load gfx/player.lmp");
	WebGL2_Init();
}

void R_NewMap (void)
{
	int i;
	for (i = 0; i < 256; ++i)
		d_lightstylevalue[i] = 264;
	memset(&r_worldentity, 0, sizeof(r_worldentity));
	r_worldentity.model = cl.worldmodel;
	if (!sv.active)
	{
		Mod_RestoreAliasModelDefaults();
		R_ClearPimpOverrides();
	}
	R_ClearParticles();

	gl2_viewleaf = NULL;
	gl2_oldviewleaf = NULL;
	gl2_visframecount = 0;
	gl2_dlightframecount = 0;

	GL2_WorldNewMap();
	Fog_NewMap();
}

void R_RenderView (void)
{
	if (r_norefresh.integer)
		return;
	if (!cl.worldmodel)
		return;

	GL2_SetupFrame();

	/* r_refdef.vrect is in UI space and top-down; GL wants device pixels
	 * from the bottom left. */
	{
		float	sx = (vid.width > 0) ? ((float)glwidth / (float)vid.width) : 1.0f;
		float	sy = (vid.height > 0) ? ((float)glheight / (float)vid.height) : 1.0f;
		int	x = (int)(r_refdef.vrect.x * sx + 0.5f);
		int	w = (int)(r_refdef.vrect.width * sx + 0.5f);
		int	h = (int)(r_refdef.vrect.height * sy + 0.5f);
		int	y = (int)((vid.height - r_refdef.vrect.y - r_refdef.vrect.height) * sy + 0.5f);

		GL2_GlideBeginScene(x, y, q_max(w, 1), q_max(h, 1));
	}

	GL2_BeginModelFrame();
	GL2_BeginWorldFrame();

	if (r_drawworld.integer)
		GL2_DrawWorld();

	GL2_DrawEntitiesOnList(false);
	/* World liquids blend with depth writes off, so they belong after the
	 * opaque entities and before the translucent ones. */
	GL2_DrawWorldWater();
	GL2_DrawEntitiesOnList(true);
	GL2_DrawViewModel();
	R_DrawParticles();

	GL2_EndModelFrame();
	GL2_GlideEndScene();

	if (gl2_viewleaf)
		V_SetContentsColor(gl2_viewleaf->contents);

	WebGL2_BeginFrame();
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

void R_PushDlights (void)
{
	GL2_PushDlights();
}

void R_InitSky (texture_t *texture)
{
	GL2_SkyNewMap(texture);
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

void WebGL2_DrawParticles (particle_t *first)
{
	GL2_DrawParticleList(first);
}
