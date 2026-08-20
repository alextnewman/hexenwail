#ifndef UHEXEN2_R_WEBGL2_H
#define UHEXEN2_R_WEBGL2_H

#include <GLES3/gl3.h>

#define WEB_MAX_TEXTURES	4096
#define WEB_MAX_LIGHTMAPS	128

extern int glx, gly, glwidth, glheight;
extern int c_brush_polys, c_alias_polys;
extern int d_lightstylevalue[256];
extern int r_framecount, r_visframecount;
extern int r_pixbytes;
extern byte *playerTranslation;
extern const int color_offsets[MAX_PLAYER_CLASS];

extern cvar_t r_norefresh;
extern cvar_t r_drawentities;
extern cvar_t r_drawworld;
extern cvar_t r_drawviewmodel;
extern cvar_t r_speeds;
extern cvar_t r_waterwarp;
extern cvar_t r_fullbright;
extern cvar_t r_lightmap;
extern cvar_t r_dynamic;
extern cvar_t r_ambient;
extern cvar_t r_novis;
extern cvar_t r_lerpmodels;
extern cvar_t r_lerp_viewmodel;
extern cvar_t r_texture_external;
extern cvar_t r_texture_external_hud;
extern cvar_t r_wateralpha;
extern cvar_t r_lavaalpha;
extern cvar_t r_slimealpha;
extern cvar_t r_telealpha;
extern cvar_t r_turbalpha;
extern cvar_t gl_overbright;
extern cvar_t gl_missile_glows;
extern cvar_t gl_flashintensity;
extern cvar_t gl_torch_dlight;
extern cvar_t gl_extra_dynamic_lights;
extern cvar_t gl_particles;
extern cvar_t gl_fullbrights;
extern cvar_t gl_glows;
extern cvar_t gl_other_glows;
extern cvar_t gl_glow_intensity;
extern cvar_t gl_fxaa;
extern cvar_t gl_overbright_models;
extern cvar_t gl_coloredlight;
extern cvar_t gl_flashblend;
extern cvar_t gl_texture_anisotropy;
extern cvar_t gl_texturemode;
extern cvar_t r_dither;
extern cvar_t r_hdr;
extern cvar_t r_hdr_exposure;
extern cvar_t r_lightmap_bicubic;
extern cvar_t r_motionblur;
extern cvar_t r_scale;
extern cvar_t r_softemu;
extern int gl_filter_idx;
extern int gl_max_anisotropy;

typedef struct
{
	qboolean anisotropy;
	qboolean float_color_buffer;
} webgl2_caps_t;
extern webgl2_caps_t gl_renderer_caps;

#define GL_PostProcess_RequestWaterwarpPreview(x) ((void)(x))
#define GL_PostProcess_ResetWaterwarpPreview() ((void)0)

extern const int ColorIndex[16];
extern const unsigned int ColorPercent[16];

/* Hexen II's 16x16 entity colour-shade tints, built in VID_Init. */
extern float RTint[256], GTint[256], BTint[256];

void WebGL2_Init (void);
void WebGL2_Shutdown (void);
void WebGL2_Resize (int width, int height);
void WebGL2_BeginFrame (void);
void WebGL2_EndFrame (void);
void WebGL2_DrawParticles (particle_t *first);

/* efrags -- r_efrag.c is software-only, so the webgl2 build compiles the
 * renderer-agnostic gl_refrag.c instead. */
void R_AddEfrags (entity_t *ent);
void R_RemoveEfrags (entity_t *ent);
void R_StoreEfrags (efrag_t **ppefrag);
void Fog_ParseServerMessage (void);

void GL_SetCanvas (canvastype canvas);
void D_EnableBackBufferAccess (void);
void D_DisableBackBufferAccess (void);

#endif
