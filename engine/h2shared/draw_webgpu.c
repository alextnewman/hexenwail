/*
 * draw_webgpu.c -- the 2D layer for WebGlideNitro.
 *
 * Same API as draw_webgl2.c and the same canvas placement, but the frame
 * shape is different.  Rather than a draw call per quad, every quad in the
 * frame lands in one CPU arena, consecutive quads sharing a texture collapse
 * into one run, and the whole thing is handed to Nitro_EndFrame as a single
 * vertex upload.  A console page of text is one draw call, not four hundred.
 *
 * The pixels stay indexed all the way to the fragment shader -- the 2D
 * pipeline samples an r8uint texture and looks the palette up itself -- so
 * the HUD, menus and console are the authored colours rather than a
 * resampled approximation of them.
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

#define WEB_MAX_CACHED_PICS	256
#define WEB_PLAYER_WIDTH	68
#define WEB_PLAYER_HEIGHT	114

typedef struct
{
	int	texture;
} nitropic_t;

typedef struct
{
	char		name[MAX_QPATH];
	qpic_t		pic;
	byte		padding[sizeof(nitropic_t) + 8];
} nitrocachepic_t;

qboolean draw_reinit;
cvar_t scr_sbarscale = {"scr_sbarscale", "1", CVAR_ARCHIVE};
cvar_t scr_menuscale = {"scr_menuscale", "1", CVAR_ARCHIVE};
cvar_t scr_crosshairscale = {"scr_crosshairscale", "1", CVAR_ARCHIVE};
cvar_t scr_conalpha = {"scr_conalpha", "0.8", CVAR_ARCHIVE};
cvar_t scr_conbrightness = {"scr_conbrightness", "1", CVAR_ARCHIVE};

static nitrocachepic_t	pic_cache[WEB_MAX_CACHED_PICS];
static int		pic_cache_count;

static int	charset_texture, smallfont_texture, bigfont_texture;
static int	console_texture, backtile_texture, ramp_texture;
static int	ramp_white_index;
static float	ramp_white_scale[3] = { 1.0f, 1.0f, 1.0f };

static byte	player_pixels[MAX_PLAYER_CLASS][WEB_PLAYER_WIDTH * WEB_PLAYER_HEIGHT];
static float	character_alpha = 1.0f;
/* Origin of the current 2D canvas, in screen pixels (see GL_SetCanvas). */
static int	canvas_x, canvas_y;

/*
=============================================================================

	the frame arena

	Quads accumulate here for the whole frame and are uploaded once.  The
	run list is coalesced as it is built, so a string of characters, a row
	of status-bar digits or a screenful of console text each cost a single
	draw.

=============================================================================
*/

#define NITRO_UI_INITIAL_VERTS	8192
#define NITRO_UI_INITIAL_RUNS	256

static wgpuui_vertex_t	*ui_vertices;
static int		ui_vertex_count, ui_vertex_max;
static wgpubatch_t	*ui_runs;
static int		ui_run_count, ui_run_max;
static int		ui_scene_run_count;

static qboolean WGPUDraw_ReserveVertices (int count)
{
	if (ui_vertex_count + count <= ui_vertex_max)
		return true;

	while (ui_vertex_max < ui_vertex_count + count)
		ui_vertex_max = ui_vertex_max ? ui_vertex_max * 2 : NITRO_UI_INITIAL_VERTS;

	ui_vertices = (wgpuui_vertex_t *) realloc (ui_vertices,
			(size_t)ui_vertex_max * sizeof(wgpuui_vertex_t));
	if (!ui_vertices)
		Sys_Error ("WebGlideNitro: out of memory for %d 2D vertices", ui_vertex_max);
	return true;
}

static qboolean WGPUDraw_ReserveRun (void)
{
	if (ui_run_count + 1 <= ui_run_max)
		return true;

	ui_run_max = ui_run_max ? ui_run_max * 2 : NITRO_UI_INITIAL_RUNS;
	ui_runs = (wgpubatch_t *) realloc (ui_runs, (size_t)ui_run_max * sizeof(wgpubatch_t));
	if (!ui_runs)
		Sys_Error ("WebGlideNitro: out of memory for %d 2D runs", ui_run_max);
	return true;
}

static unsigned int WGPUDraw_PackColor (float r, float g, float b, float a)
{
	int	ir, ig, ib, ia;

	ir = (int)(r * 255.0f + 0.5f);
	ig = (int)(g * 255.0f + 0.5f);
	ib = (int)(b * 255.0f + 0.5f);
	ia = (int)(a * 255.0f + 0.5f);
	if (ir < 0) ir = 0; else if (ir > 255) ir = 255;
	if (ig < 0) ig = 0; else if (ig > 255) ig = 255;
	if (ib < 0) ib = 0; else if (ib > 255) ib = 255;
	if (ia < 0) ia = 0; else if (ia > 255) ia = 255;

	return (unsigned int)ir | ((unsigned int)ig << 8) |
		((unsigned int)ib << 16) | ((unsigned int)ia << 24);
}

static void Draw_Quad (int texture, float x, float y, float width, float height,
	float sl, float tl, float sh, float th, float r, float g, float b, float a)
{
	/* canvas coordinates -> screen coordinates */
	const float	x0 = x + canvas_x, y0 = y + canvas_y;
	const float	x1 = x0 + width, y1 = y0 + height;
	unsigned int	color;
	wgpuui_vertex_t	*out;

	if (texture <= 0 || width <= 0 || height <= 0)
		return;

	WGPUDraw_ReserveVertices (6);
	color = WGPUDraw_PackColor (r, g, b, a);
	out = &ui_vertices[ui_vertex_count];

	out[0].x = x0; out[0].y = y0; out[0].s = sl; out[0].t = tl;
	out[1].x = x1; out[1].y = y0; out[1].s = sh; out[1].t = tl;
	out[2].x = x1; out[2].y = y1; out[2].s = sh; out[2].t = th;
	out[3].x = x0; out[3].y = y0; out[3].s = sl; out[3].t = tl;
	out[4].x = x1; out[4].y = y1; out[4].s = sh; out[4].t = th;
	out[5].x = x0; out[5].y = y1; out[5].s = sl; out[5].t = th;
	out[0].color = out[1].color = out[2].color = color;
	out[3].color = out[4].color = out[5].color = color;

	if (ui_run_count > ui_scene_run_count &&
	    ui_runs[ui_run_count - 1].texture == texture)
	{
		ui_runs[ui_run_count - 1].count += 6;
	}
	else
	{
		WGPUDraw_ReserveRun ();
		ui_runs[ui_run_count].texture = texture;
		ui_runs[ui_run_count].first = ui_vertex_count;
		ui_runs[ui_run_count].count = 6;
		ui_runs[ui_run_count].entity = 0;
		ui_run_count++;
	}

	ui_vertex_count += 6;
}

/*
================
WGPUDraw_BeginScene

The shared screen code emits background clears before R_RenderView.  Preserve
that ordering even though Nitro uploads all 2D vertices together at frame end.
================
*/
void WGPUDraw_BeginScene (void)
{
	ui_scene_run_count = ui_run_count;
}

/*
================
WGPUDraw_EndFrame

One upload, one pass, and the arena is reset afterwards rather than at
frame start: a menu-only frame never enters R_RenderView, so there is no
reliable "beginning of the 2D layer" to clear at.
================
*/
void WGPUDraw_EndFrame (void)
{
	wgpuui_params_t	params;
	int		i;

	params.screen_width = (float)q_max(vid.width, 1);
	params.screen_height = (float)q_max(vid.height, 1);
	WGPU_GammaContrast (&params.gamma, &params.contrast);

	Nitro_EndFrame (&params, ui_vertices, ui_vertex_count, ui_runs, ui_run_count,
			ui_scene_run_count);

	for (i = 0; i < ui_run_count; i++)
		WebPerf_CountDraw (ui_runs[i].count / 3);
	if (ui_vertex_count > 0)
		WebPerf_CountUpload ((size_t)ui_vertex_count * sizeof(wgpuui_vertex_t));

	ui_vertex_count = 0;
	ui_run_count = 0;
	ui_scene_run_count = 0;
}

/*
=============================================================================

	textures

	Everything the 2D layer draws stays an 8-bit index; the shader does
	the palette lookup.  The flags say which index, if any, is a hole.

=============================================================================
*/

int WGPUDraw_LoadTexture (const char *name, const byte *pixels, int width,
	int height, unsigned int flags)
{
	return Nitro_CreateTexture (name, width, height, pixels, flags);
}

static int Draw_LoadPicTexture (const char *name, const byte *pixels, int width,
	int height, qboolean transparent, qboolean zero_transparent)
{
	unsigned int	flags = 0;

	if (transparent)
		flags |= NITROTEX_ALPHA;
	if (zero_transparent)
		flags |= NITROTEX_HOLEY;
	return Nitro_CreateTexture (name, width, height, pixels, flags);
}

/*
================
Draw_BuildRampTexture

A 256x1 identity ramp: texel i is palette index i.  Draw_Fill can then hit
the exact authored colour by sampling its own index instead of tinting an
approximation of white, and Draw_FillAlpha gets the nearest neutral index
plus the small correction that makes it behave like true white.
================
*/
static void Draw_BuildRampTexture (void)
{
	byte	ramp[256];
	int	i, best = -1, bestscore = -1;

	for (i = 0; i < 256; i++)
		ramp[i] = (byte)i;
	ramp_texture = Nitro_CreateTexture ("*nitro_ramp", 256, 1, ramp, 0);

	/* Nearest thing to pure white that is actually in the palette: the
	 * highest minimum channel, tie-broken on total brightness. */
	for (i = 0; i < 255; i++)
	{
		const byte	*rgba = (const byte *)&d_8to24table[i];
		int		lo = rgba[0];
		int		score;

		if (rgba[1] < lo) lo = rgba[1];
		if (rgba[2] < lo) lo = rgba[2];
		score = lo * 1024 + rgba[0] + rgba[1] + rgba[2];
		if (score > bestscore)
		{
			bestscore = score;
			best = i;
		}
	}

	if (best < 0)
		best = 254;
	ramp_white_index = best;
	{
		const byte	*rgba = (const byte *)&d_8to24table[best];

		for (i = 0; i < 3; i++)
			ramp_white_scale[i] = rgba[i] ? 255.0f / (float)rgba[i] : 1.0f;
	}
}

static void Draw_SetPic (qpic_t *pic, int texture)
{
	nitropic_t	value;

	value.texture = texture;
	memcpy (pic->data, &value, sizeof(value));
}

static int Draw_GetPic (const qpic_t *pic)
{
	nitropic_t	value;

	memcpy (&value, pic->data, sizeof(value));
	return value.texture;
}

static qpic_t *Draw_LoadPicFile (const char *path, qboolean no_transparency)
{
	qpic_t	*pic = (qpic_t *)FS_LoadHunkFile (path, NULL);

	if (!pic)
		return NULL;
	SwapPic (pic);
	Draw_SetPic (pic, Draw_LoadPicTexture (path, pic->data, pic->width,
			pic->height, !no_transparency, false));
	return pic;
}

qpic_t *Draw_PicFromFile (const char *name)
{
	return Draw_LoadPicFile (name, false);
}

qpic_t *Draw_PicFromWad (const char *name)
{
	qpic_t	*pic = W_GetLumpName (name);

	Draw_SetPic (pic, Draw_LoadPicTexture (name, pic->data, pic->width,
			pic->height, true, false));
	return pic;
}

static qpic_t *Draw_CachePicInternal (const char *path, qboolean no_transparency)
{
	qpic_t		*source;
	nitrocachepic_t	*entry;
	int		texture;
	int		i;

	for (i = 0; i < pic_cache_count; ++i)
	{
		if (!strcmp (pic_cache[i].name, path))
			return &pic_cache[i].pic;
	}
	if (pic_cache_count == WEB_MAX_CACHED_PICS)
		Sys_Error ("Web UI picture cache exhausted");

	source = (qpic_t *)FS_LoadTempFile (path, NULL);
	if (!source)
		Sys_Error ("Failed to load %s", path);
	SwapPic (source);
	texture = Draw_LoadPicTexture (path, source->data, source->width,
			source->height, !no_transparency, false);

	entry = &pic_cache[pic_cache_count++];
	q_strlcpy (entry->name, path, sizeof(entry->name));
	entry->pic.width = source->width;
	entry->pic.height = source->height;
	Draw_SetPic (&entry->pic, texture);
	if (!strncmp (path, "gfx/menu/netp", 13) &&
		path[13] >= '1' && path[13] <= '0' + MAX_PLAYER_CLASS)
	{
		memcpy (player_pixels[path[13] - '1'], source->data,
			WEB_PLAYER_WIDTH * WEB_PLAYER_HEIGHT);
	}
	return &entry->pic;
}

qpic_t *Draw_CachePic (const char *path) { return Draw_CachePicInternal (path, false); }
qpic_t *Draw_CachePicNoTrans (const char *path) { return Draw_CachePicInternal (path, true); }
qpic_t *Draw_CacheLoadingPic (void) { return Draw_CachePic ("gfx/menu/loading.lmp"); }

void Draw_Init (void)
{
	qpic_t	*pic;
	byte	*pixels;

	if (!draw_reinit)
	{
		Cvar_RegisterVariable (&scr_sbarscale);
		Cvar_RegisterVariable (&scr_menuscale);
		Cvar_RegisterVariable (&scr_crosshairscale);
		Cvar_RegisterVariable (&scr_conalpha);
		Cvar_RegisterVariable (&scr_conbrightness);
	}

	Draw_BuildRampTexture ();

	pixels = FS_LoadTempFile ("gfx/menu/conchars.lmp", NULL);
	if (!pixels || fs_filesize < 256 * 128)
		Sys_Error ("Invalid gfx/menu/conchars.lmp");
	charset_texture = Draw_LoadPicTexture ("conchars", pixels, 256, 128, true, true);

	pixels = W_GetLumpName ("tinyfont");
	smallfont_texture = Draw_LoadPicTexture ("tinyfont", pixels, 128, 32, true, true);

	pic = (qpic_t *)FS_LoadTempFile ("gfx/menu/bigfont2.lmp", NULL);
	if (!pic)
		pic = (qpic_t *)FS_LoadTempFile ("gfx/menu/bigfont.lmp", NULL);
	if (!pic)
		Sys_Error ("Missing big menu font");
	SwapPic (pic);
	bigfont_texture = Draw_LoadPicTexture ("bigfont", pic->data, pic->width,
			pic->height, true, true);

	pic = (qpic_t *)FS_LoadTempFile ("gfx/menu/conback.lmp", NULL);
	if (!pic)
		Sys_Error ("Missing console background");
	SwapPic (pic);
	console_texture = Draw_LoadPicTexture ("conback", pic->data, pic->width,
			pic->height, false, false);

	pic = (qpic_t *)FS_LoadTempFile ("gfx/menu/backtile.lmp", NULL);
	if (!pic)
		Sys_Error ("Missing background tile");
	SwapPic (pic);
	backtile_texture = Nitro_CreateTexture ("backtile", pic->width, pic->height,
			pic->data, NITROTEX_WRAP);

	GL_SetCanvas (CANVAS_DEFAULT);
}

void Draw_ReInit (void) { draw_reinit = true; Draw_Init (); draw_reinit = false; }

/*
================
GL_SetCanvas

Places one of the fixed-size logical UI canvases on the screen. Draw_Quad
translates every 2D vertex by the canvas origin, which is what anchors the
status bar to the bottom of the screen and centres the menus; sbar.c and
menu.c draw in canvas coordinates. The UI scale is 1 here -- the software
renderer's canvases must land in the same place, and it cannot scale 2D.
================
*/
void GL_SetCanvas (canvastype canvas)
{
	switch (canvas)
	{
	case CANVAS_SBAR:
		canvas_x = (vid.width - UI_CANVAS_WIDTH) / 2;
		canvas_y = vid.height - UI_SBAR_CANVAS_HEIGHT;
		break;
	case CANVAS_MENU:
		canvas_x = (vid.width - UI_CANVAS_WIDTH) / 2;
		canvas_y = 0;
		break;
	default:
		canvas_x = 0;
		canvas_y = 0;
		break;
	}

	if (canvas_x < 0)
		canvas_x = 0;
	if (canvas_y < 0)
		canvas_y = 0;
}

void Draw_FlushCharBatch (void) {}

/* The web 2D layers do not scale the UI canvases -- all of them place them
 * at scale 1 -- so report that honestly. menu.c derives its mouse
 * hit-testing from this value, and a scale the canvas does not actually
 * apply would move the hit boxes away from the glyphs. */
float SCR_CalcUIScale (cvar_t *user) { (void)user; return 1.0f; }

void Draw_Pic (int x, int y, qpic_t *pic)
{
	Draw_Quad (Draw_GetPic(pic), x, y, pic->width, pic->height, 0, 0, 1, 1, 1, 1, 1, 1);
}
void Draw_TransPic (int x, int y, qpic_t *pic) { Draw_Pic (x, y, pic); }
void Draw_AlphaPic (int x, int y, qpic_t *pic, float alpha)
{
	Draw_Quad (Draw_GetPic(pic), x, y, pic->width, pic->height, 0, 0, 1, 1,
		1, 1, 1, alpha);
}
void Draw_PicCropped (int x, int y, qpic_t *pic) { Draw_Pic (x, y, pic); }
void Draw_TransPicCropped (int x, int y, qpic_t *pic) { Draw_Pic (x, y, pic); }
void Draw_SubPicCropped (int x, int y, int height, qpic_t *pic)
{
	float	ratio = q_min(height, pic->height) / (float)pic->height;

	Draw_Quad (Draw_GetPic(pic), x, y, pic->width, pic->height * ratio,
		0, 0, 1, ratio, 1, 1, 1, 1);
}
void Draw_SubPic (int x, int y, qpic_t *pic, int sx, int sy, int width, int height)
{
	Draw_Quad (Draw_GetPic(pic), x, y, width, height,
		sx / (float)pic->width, sy / (float)pic->height,
		(sx + width) / (float)pic->width, (sy + height) / (float)pic->height,
		1, 1, 1, 1);
}

void Draw_Character (int x, int y, unsigned int num)
{
	float	sl, tl;

	if (num == 32)
		return;
	num &= 511;
	sl = (num & 31) / 32.0f;
	tl = (num >> 5) / 16.0f;
	Draw_Quad (charset_texture, x, y, 8, 8, sl, tl, sl + 1.0f / 32.0f,
		tl + 1.0f / 16.0f, 1, 1, 1, character_alpha);
}

void Draw_SetCharacterAlpha (float alpha) { character_alpha = alpha; }
void Draw_String (int x, int y, const char *text)
{
	while (*text) { Draw_Character (x, y, (byte)*text++); x += 8; }
}
void Draw_RedString (int x, int y, const char *text)
{
	while (*text) { Draw_Character (x, y, ((byte)*text++) + 256); x += 8; }
}
void Draw_SmallCharacter (int x, int y, int num)
{
	int	row, col;
	float	sl, tl;

	/* The tinyfont atlas is 128x32: 16 columns of 8x8 over 4 rows, and it
	 * only covers ' '..'_'.  Lower case folds onto upper case.  Same
	 * folding as gl_draw.c:968 and draw.c. */
	if (num < 32)
		num = 0;
	else if (num >= 'a' && num <= 'z')
		num -= 64;
	else if (num > '_')
		num = 0;
	else
		num -= 32;

	if (num == 0)
		return;

	row = num >> 4;
	col = num & 15;
	sl = col / 16.0f;
	tl = row / 4.0f;
	Draw_Quad (smallfont_texture, x, y, 8, 8, sl, tl, sl + 1.0f / 16.0f,
		tl + 1.0f / 4.0f, 1, 1, 1, character_alpha);
}
void Draw_SmallString (int x, int y, const char *text)
{
	while (*text) { Draw_SmallCharacter (x, y, (byte)*text++); x += 6; }
}
void Draw_BigCharacter (int x, int y, int num)
{
	float	sl, tl;

	/* num is a glyph index, not an ASCII code: M_DrawBigCharacter has
	 * already mapped '/' to 26 and 'A'..'Z' to 0..25, and rejects
	 * anything else.  The bigfont atlas is 160x80, 8 columns of 20x20
	 * over 4 rows.  Same contract as gl_draw.c:1029 and draw.c. */
	sl = (num % 8) / 8.0f;
	tl = (num / 8) / 4.0f;
	Draw_Quad (bigfont_texture, x, y, 20, 20, sl, tl, sl + 0.125f,
		tl + 0.25f, 1, 1, 1, 1);
}

void Draw_FillAlpha (int x, int y, int w, int h, float r, float g, float b, float a)
{
	float	s = (ramp_white_index + 0.5f) / 256.0f;

	Draw_Quad (ramp_texture, x, y, w, h, s, 0.5f, s, 0.5f,
		r * ramp_white_scale[0], g * ramp_white_scale[1],
		b * ramp_white_scale[2], a);
}
void Draw_Fill (int x, int y, int w, int h, int color)
{
	float	s = ((color & 255) + 0.5f) / 256.0f;

	Draw_Quad (ramp_texture, x, y, w, h, s, 0.5f, s, 0.5f, 1, 1, 1, 1);
}
void Draw_FadeScreen (void) { Draw_FillAlpha (0, 0, vid.width, vid.height, 0, 0, 0, 0.65f); }
void Draw_MenuBackdrop (void) { Draw_ConsoleBackground (vid.height); }
void Draw_ConsoleBackground (int lines)
{
	Draw_Quad (console_texture, 0, 0, vid.width, lines, 0, 0, 1, 1,
		scr_conbrightness.value, scr_conbrightness.value, scr_conbrightness.value,
		scr_conalpha.value);
}
void Draw_TileClear (int x, int y, int w, int h)
{
	Draw_Quad (backtile_texture, x, y, w, h, 0, 0, w / 64.0f, h / 64.0f, 1, 1, 1, 1);
}
void Draw_Crosshair (void)
{
	Draw_Character (vid.width / 2 - 4, vid.height / 2 - 4, '+');
}
void Draw_IntermissionPic (qpic_t *pic)
{
	Draw_Quad (Draw_GetPic(pic), 0, 0, vid.width, vid.height, 0, 0, 1, 1, 1, 1, 1, 1);
}
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation, int p_class)
{
	(void)translation;
	(void)p_class;
	(void)player_pixels;
	Draw_TransPic (x, y, pic);
}
