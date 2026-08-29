/*
 * vid_webgpu.c -- the video layer for WebGlideNitro.
 *
 * There is no context to create here. The launcher already requested the
 * adapter, configured
 * the canvas and left the device on Module.hexenwailWebGPU, so all this file
 * does is the two things that are genuinely the video layer's job: decide
 * the two decoupled sizes, and hand the renderer the indexed-colour
 * constants -- the palette and the authored colormap.
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

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

viddef_t vid;
modestate_t modestate = MS_FULLSCREEN;
qboolean in_mode_set;

byte globalcolormap[VID_GRADES * 256];
byte lastglobalcolor;
byte *lastsourcecolormap;
unsigned short d_8to16table[256];
unsigned int d_8to24table[256];
unsigned int d_8to24TranslucentTable[256];
float RTint[256], GTint[256], BTint[256];
unsigned char *inverse_pal;

const int ColorIndex[16] = {
	0, 31, 47, 63, 79, 95, 111, 127, 143, 159, 175, 191, 199, 207, 223, 231
};
const unsigned int ColorPercent[16] = {
	25, 51, 76, 102, 114, 127, 140, 153, 165, 178, 191, 204, 216, 229, 237, 247
};

/*
 * Two sizes, deliberately decoupled from the panel.
 *
 *   glwidth/glheight  the canvas, in real device pixels.
 *   vid.width/height  the UI space the 320-wide status bar and menus are
 *                     laid out in.  A 2732px iPad panel would otherwise
 *                     render the HUD at about a tenth of the screen.
 */
static const int vid_ui_heights[] = { 240, 300, 384, 480, 600, 720 };
#define NUM_UI_HEIGHTS	((int)(sizeof(vid_ui_heights) / sizeof(vid_ui_heights[0])))

cvar_t vid_ui_height = {"vid_ui_height", "0", CVAR_ARCHIVE};

static void VID_PickUISize (int gw, int gh, int *out_w, int *out_h)
{
	float	aspect = (gh > 0) ? ((float)gw / (float)gh) : (4.0f / 3.0f);
	int	i, height, width;

	if (vid_ui_height.integer > 0)
	{
		height = vid_ui_height.integer;
	}
	else
	{
		height = vid_ui_heights[0];
		for (i = 0; i < NUM_UI_HEIGHTS; i++)
		{
			if (vid_ui_heights[i] * 2 <= gh)
				height = vid_ui_heights[i];
		}
	}

	if (height > gh)
		height = gh;
	if (height < 200)
		height = 200;
	else if (height > 1024)
		height = 1024;

	width = (int)(height * aspect + 0.5f);
	if (width < 320)
		width = 320;
	else if (width > 1280)
		width = 1280;

	*out_w = width;
	*out_h = height;
}

static void VID_SetSize (int width, int height)
{
	int	ui_width, ui_height;

	if (width < 320) width = 320;
	if (height < 200) height = 200;
	emscripten_set_canvas_element_size("#canvas", width, height);

	glwidth = width;
	glheight = height;
	glx = gly = 0;

	VID_PickUISize(width, height, &ui_width, &ui_height);
	vid.width = vid.conwidth = ui_width;
	vid.height = vid.conheight = ui_height;
	vid.rowbytes = vid.conrowbytes = ui_width;
	vid.aspect = ((float)height / (float)width) * (320.0f / 240.0f);
	vid.numpages = 2;
	vid.recalc_refdef = 1;
	WebGPU_Resize(width, height);
}

/*
================
VID_BuildTintTables

Hexen II's 16x16 colour-shade table: sixteen palette hues at sixteen
translucency levels.  The QC uses it for every tinted effect in the game
-- the spell glows, the powered-up weapons, the artifact auras -- and
without it entity_t::colorshade does nothing.
================
*/
static void VID_BuildTintTables (const unsigned char *palette)
{
	int	i, p, c, r, g, b;
	unsigned int	*table = d_8to24TranslucentTable;

	for (i = 0; i < 16; i++)
	{
		c = ColorIndex[i] * 3;
		r = palette[c];
		g = palette[c + 1];
		b = palette[c + 2];

		for (p = 0; p < 16; p++)
		{
			unsigned int	a = ColorPercent[15 - p];

			*table++ = (a << 24) | ((unsigned int)b << 16) |
					((unsigned int)g << 8) | (unsigned int)r;

			RTint[i * 16 + p] = ((float)r) / ((float)a);
			GTint[i * 16 + p] = ((float)g) / ((float)a);
			BTint[i * 16 + p] = ((float)b) / ((float)a);
		}
	}
}

EMSCRIPTEN_KEEPALIVE void Web_ResizeCanvas (int css_width, int css_height)
{
	double ratio = emscripten_get_device_pixel_ratio();
	int width = (int)(css_width * ratio);
	int height = (int)(css_height * ratio);
	VID_SetSize(width, height);
}

void VID_Init (const unsigned char *palette)
{
	double width = 0, height = 0;
	int i;

	Cvar_RegisterVariable(&vid_ui_height);

	for (i = 0; i < 256; ++i)
	{
		d_8to24table[i] = palette[i * 3] |
			(palette[i * 3 + 1] << 8) |
			(palette[i * 3 + 2] << 16) | 0xff000000u;
	}
	d_8to24table[255] &= 0x00ffffffu;
	VID_BuildTintTables(palette);
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
	/* The colormap is content; the fullbright split has to be sane before
	 * anything builds self-lit masks from it. */
	if (vid.fullbright < 1 || vid.fullbright > 256)
		vid.fullbright = 224;

	emscripten_get_element_css_size("#canvas", &width, &height);
	if (width <= 0 || height <= 0)
	{
		width = 960;
		height = 540;
	}
	VID_SetSize((int)(width * emscripten_get_device_pixel_ratio()),
		(int)(height * emscripten_get_device_pixel_ratio()));

	/* WebGPU_Init() is what actually opens the device, so everything that
	 * touches it -- the canvas size and the two indexed constants -- has to
	 * follow it, not VID_SetSize. */
	WebGPU_Init();
	Nitro_Resize(glwidth, glheight);
	Nitro_SetPalette(palette);
	Nitro_SetColormap(host_colormap, VID_GRADES);
}

void VID_Shutdown (void)
{
	WebGPU_Shutdown();
}

void VID_Update (vrect_t *rects) { (void)rects; WebGPU_EndFrame(); }
void VID_SetPalette (const unsigned char *palette) { (void)palette; }
void VID_ShiftPalette (const unsigned char *palette) { (void)palette; }
void VID_LockBuffer (void) {}
void VID_UnlockBuffer (void) {}
void VID_HandlePause (qboolean paused) { (void)paused; }
void VID_ToggleFullscreen (void) {}
void D_ShowLoadingSize (void) {}
qboolean VID_HasMouseOrInputFocus (void) { return true; }
qboolean VID_IsMinimized (void)
{
	EmscriptenVisibilityChangeEvent status;
	return !emscripten_get_visibility_status(&status) && status.hidden;
}
void VID_InitMouseCursors (void) {}
void VID_SetMouseCursor (mousecursor_t cursor) { (void)cursor; }

void VID_ChangeConsize (int dir) { (void)dir; }
float VID_ReportConsize (void) { return (float)vid.conwidth; }
void VID_MenuInit (void) {}
qboolean VID_MenuNeedApply (void) { return false; }
void VID_MenuApply (void) {}
void VID_MenuReset (void) {}
const char *VID_MenuGetResolution (qboolean *is_current)
{
	static char size[32];
	if (is_current) *is_current = true;
	q_snprintf(size, sizeof(size), "%dx%d", vid.width, vid.height);
	return size;
}
const char *VID_MenuGetAspect (void) { return "Device"; }
int VID_MenuGetWindowMode (void) { return 1; }
int VID_MenuGetMultisample (qboolean *is_current, qboolean *available)
{
	if (is_current) *is_current = true;
	if (available) *available = false;
	return 0;
}
int VID_MenuGetVSync (void) { return 1; }
qboolean VID_MenuGetTexFilter (void) { return true; }
int VID_MenuGetAnisotropy (qboolean *available)
{
	if (available) *available = false;
	return 1;
}
void VID_MenuAdjustWindowMode (int dir) { (void)dir; }
void VID_MenuAdjustAspect (int dir) { (void)dir; }
void VID_MenuAdjustResolution (int dir) { (void)dir; }
void VID_MenuAdjustMultisample (int dir) { (void)dir; }
void VID_MenuAdjustVSync (int dir) { (void)dir; }
void VID_MenuAdjustTexFilter (void) {}
void VID_MenuAdjustAnisotropy (int dir) { (void)dir; }
