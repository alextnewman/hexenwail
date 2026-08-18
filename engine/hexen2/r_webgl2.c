#include "quakedef.h"

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
cvar_t gl_coloredlight = {"gl_coloredlight", "0", CVAR_ARCHIVE};
cvar_t gl_flashblend = {"gl_flashblend", "0", CVAR_ARCHIVE};
cvar_t gl_texture_anisotropy = {"gl_texture_anisotropy", "1", CVAR_ARCHIVE};
cvar_t gl_texturemode = {"gl_texturemode", "GL_NEAREST", CVAR_ARCHIVE};
cvar_t r_dither = {"r_dither", "0", CVAR_ARCHIVE};
cvar_t r_hdr = {"r_hdr", "0", CVAR_ARCHIVE};
cvar_t r_hdr_exposure = {"r_hdr_exposure", "1", CVAR_ARCHIVE};
cvar_t r_lightmap_bicubic = {"r_lightmap_bicubic", "0", CVAR_ARCHIVE};
cvar_t r_motionblur = {"r_motionblur", "0", CVAR_ARCHIVE};
cvar_t r_scale = {"r_scale", "1", CVAR_ARCHIVE};
cvar_t r_softemu = {"r_softemu", "0", CVAR_ARCHIVE};
cvar_t r_watercolor = {"r_watercolor", "1", CVAR_ARCHIVE};
int gl_filter_idx;
int gl_max_anisotropy = 1;
webgl2_caps_t gl_renderer_caps;
byte *playerTranslation;
const int color_offsets[MAX_PLAYER_CLASS] = {
	2 * 14 * 256, 0, 1 * 14 * 256, 2 * 14 * 256, 2 * 14 * 256
};

qboolean r_cache_thrash;

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
	Cvar_RegisterVariable(&r_watercolor);
}

void WebGL2_Init (void)
{
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void WebGL2_Shutdown (void) {}

void WebGL2_Resize (int width, int height)
{
	glx = gly = 0;
	glwidth = width;
	glheight = height;
	glViewport(0, 0, width, height);
}

void WebGL2_BeginFrame (void)
{
	glViewport(0, 0, vid.width, vid.height);
	glClearColor(0.08f, 0.06f, 0.04f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
	R_ClearParticles();
}

void R_RenderView (void)
{
	if (r_norefresh.integer)
		return;
	++r_framecount;
	VectorCopy(r_refdef.vieworg, r_origin);
	AngleVectors(r_refdef.viewangles, vpn, vright, vup);
	WebGL2_BeginFrame();
	R_DrawParticles();
}

void R_ViewChanged (float aspect) { (void)aspect; }

void R_SetVrect (vrect_t *pvrect, vrect_t *pvrectin, int lineadj)
{
	*pvrect = *pvrectin;
	pvrect->height -= lineadj;
}

void R_PushDlights (void) {}
void R_InitSky (texture_t *texture) { (void)texture; }

void R_LerpEntity (entity_t *entity, vec3_t origin, vec3_t angles)
{
	VectorCopy(entity->origin, origin);
	VectorCopy(entity->angles, angles);
}

void R_AddEfrags (entity_t *entity) { (void)entity; }
void R_RemoveEfrags (entity_t *entity) { entity->efrag = NULL; }

void D_FlushCaches (void) {}
void D_DeleteSurfaceCache (void) {}
void D_InitCaches (void *buffer, int size) { (void)buffer; (void)size; }
int D_SurfaceCacheForRes (int width, int height) { (void)width; (void)height; return 0; }
void D_EnableBackBufferAccess (void) {}
void D_DisableBackBufferAccess (void) {}

void WebGL2_DrawParticles (particle_t *first)
{
	(void)first;
}

void Fog_ParseServerMessage (void)
{
	MSG_ReadByte();
	MSG_ReadByte();
	MSG_ReadByte();
	MSG_ReadByte();
	MSG_ReadShort();
}
