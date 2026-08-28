/*
 * r_webgpu.h -- what the shared client sees of the WebGlideNitro renderer.
 *
 * The mirror image of r_webgl2.h for WEBGPUQUAKE builds, minus every GL
 * declaration: this renderer has no GL context and includes no GL headers.
 * quakeinc.h pulls it in, so it must declare exactly the globals, cvars and
 * entry points the renderer-agnostic client code references.
 *
 * Copyright (C) 2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef UHEXEN2_R_WEBGPU_H
#define UHEXEN2_R_WEBGPU_H

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
extern cvar_t r_nitro_overbright;
extern cvar_t r_nitro_missile_glows;
extern cvar_t r_nitro_flashintensity;
extern cvar_t r_nitro_torch_dlight;
extern cvar_t r_nitro_extra_dynamic_lights;
extern cvar_t r_nitro_particles;
extern cvar_t r_nitro_fullbrights;
extern cvar_t r_nitro_glows;
extern cvar_t r_nitro_other_glows;
extern cvar_t r_nitro_glow_intensity;
extern cvar_t r_nitro_fxaa;
extern cvar_t r_nitro_overbright_models;
extern cvar_t r_nitro_coloredlight;
extern cvar_t r_nitro_flashblend;
extern cvar_t r_nitro_texture_anisotropy;
extern cvar_t r_nitro_texturemode;
extern cvar_t r_dither;
extern cvar_t r_hdr;
extern cvar_t r_hdr_exposure;
extern cvar_t r_lightmap_bicubic;
extern cvar_t r_motionblur;
extern cvar_t r_scale;
extern cvar_t r_softemu;
extern int r_nitro_filter_idx;
extern int r_nitro_max_anisotropy;

#define GL_PostProcess_RequestWaterwarpPreview(x) ((void)(x))
#define GL_PostProcess_ResetWaterwarpPreview() ((void)0)

extern const int ColorIndex[16];
extern const unsigned int ColorPercent[16];

/* Hexen II's 16x16 entity colour-shade tints, built in VID_Init. */
extern float RTint[256], GTint[256], BTint[256];

void WebGPU_Init (void);
void WebGPU_Shutdown (void);
void WebGPU_Resize (int width, int height);
void WebGPU_BeginFrame (void);
void WebGPU_EndFrame (void);
void WebGPU_DrawParticles (particle_t *first);

/* efrags -- r_efrag.c is software-only, so this build compiles the
 * renderer-agnostic gl_refrag.c instead. */
void R_AddEfrags (entity_t *ent);
void R_RemoveEfrags (entity_t *ent);
void R_StoreEfrags (efrag_t **ppefrag);
void Fog_ParseServerMessage (void);

void GL_SetCanvas (canvastype canvas);
void D_EnableBackBufferAccess (void);
void D_DisableBackBufferAccess (void);

#endif	/* UHEXEN2_R_WEBGPU_H */
