/*
 * vid_soft_web.c -- video backend for the 8bpp software renderer on the
 * web platform.
 *
 * Owns the palettized software framebuffer, the software resolution ladder
 * and the aspect policy; hands the finished frame to the accelerated
 * presentation canvas (web_canvas.h).  See docs/web/SOFTWARE_RENDERER.md.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "web_perf.h"
#include "d_local.h"
#include "web_canvas.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

viddef_t	vid;			// global video state
modestate_t	modestate = MS_FULLSCREEN;
qboolean	in_mode_set;

byte		globalcolormap[VID_GRADES * 256];
byte		lastglobalcolor = 0;
byte		*lastsourcecolormap = NULL;
unsigned short	d_8to16table[256];
unsigned int	d_8to24table[256];

/*
 * Software resolution ladder.
 *
 * Every entry is 4:3 (683x512 is 4:3 to within half a pixel) and stays
 * inside the software rasterizer's MAXWIDTH/MAXHEIGHT limits (1280x1024,
 * see r_shared.h).  The list is deliberately short: these are render
 * resolutions, not display modes -- the presentation canvas always runs at
 * the panel's full device resolution and scales this image up on the GPU.
 *
 * A rung is a *budget*, not a fixed shape: with vid_soft_widescreen set
 * (the default) the entry's short side is kept and the long side is stretched
 * to the canvas aspect, so the image fills the panel edge to edge instead of
 * pillarboxing.  See VID_ModeSize().
 *
 * Entries flagged as "panel aligned" divide an M1 iPad Pro panel exactly:
 *   683x512  x4 = 2732x2048  (12.9-inch, exact)
 *   1024x768 x2 = 2048x1536  (fits both panels with a small border)
 */
typedef struct
{
	int		width;
	int		height;
	const char	*desc;
} softmode_t;

static const softmode_t softmodes[] = {
	{  320, 240, "320 x 240" },
	{  400, 300, "400 x 300" },
	{  512, 384, "512 x 384" },
	{  640, 480, "640 x 480" },
	{  683, 512, "683 x 512" },
	{  800, 600, "800 x 600" },
	{  960, 720, "960 x 720" },
	{ 1024, 768, "1024 x 768" },
	{ 1152, 864, "1152 x 864" },
	{ 1280, 960, "1280 x 960" },
};

#define NUM_SOFTMODES	((int)(sizeof(softmodes) / sizeof(softmodes[0])))

/* Auto mode never picks anything heavier than this. The software rasterizer
 * is single-threaded on the main WASM thread, so pixel count is the frame
 * budget. 1024x768 is the highest rung an M1 class core sustains at 60 Hz
 * with room left for the server, sound and QuakeC. */
#define AUTO_MAX_PIXELS		(1024 * 768)
/* Auto mode refuses to upscale by less than this: below it the software
 * image is so close to the panel resolution that it costs a lot and looks
 * no better than the rung below. */
#define AUTO_MIN_SCALE		2.0f

/* 0 = pick automatically from the canvas size, 1..NUM_SOFTMODES = ladder index. */
static cvar_t	vid_soft_mode = {"vid_soft_mode", "0", CVAR_ARCHIVE};
/* 0 = nearest neighbour (classic crunchy pixels), 1 = pixel-art antialiasing. */
static cvar_t	vid_soft_filter = {"vid_soft_filter", "0", CVAR_ARCHIVE};
/* 1 = render at the canvas aspect so play is edge to edge, 0 = classic 4:3. */
static cvar_t	vid_soft_widescreen = {"vid_soft_widescreen", "1", CVAR_ARCHIVE};
/* Only consulted with vid_soft_widescreen 0:
 * 0 = keep the 4:3 render aspect and pillarbox, 1 = stretch to fill. */
static cvar_t	vid_soft_stretch = {"vid_soft_stretch", "0", CVAR_ARCHIVE};

static byte	*vid_framebuffer;
static int	vid_framebuffer_size;
static byte	*vid_surfcache;
static int	vid_surfcachesize;
static int	vid_highhunkmark;
static int	canvas_width = 960, canvas_height = 720;
static int	vid_current_mode = -1;
static int	vid_menu_mode;
static int	lockcount;
static byte	vid_curpal[768];
static qboolean	vid_initialized;

/*
================
VID_DestRect

Where the software image lands on the canvas, in device pixels. Keeps the
render aspect unless vid_soft_stretch is set, and centres what is left.
================
*/
static void VID_DestRect (int src_w, int src_h, int *x, int *y, int *w, int *h)
{
	float	src_aspect;

	if (vid_soft_stretch.integer && !vid_soft_widescreen.integer)
	{
		*x = *y = 0;
		*w = canvas_width;
		*h = canvas_height;
		return;
	}

	src_aspect = (float)src_w / (float)src_h;
	if ((float)canvas_width / (float)canvas_height > src_aspect)
	{
		*h = canvas_height;
		*w = (int)(canvas_height * src_aspect + 0.5f);
	}
	else
	{
		*w = canvas_width;
		*h = (int)(canvas_width / src_aspect + 0.5f);
	}
	if (*w < 1) *w = 1;
	if (*h < 1) *h = 1;
	/* A widescreen render size is a whole number of pixels, so it can only
	 * match the canvas aspect to within a rounding error. Absorb bars
	 * thinner than half a percent rather than leave a seam of dead pixels
	 * down the edge of the panel; the resulting stretch is not visible. */
	if (canvas_width - *w <= canvas_width / 200)
		*w = canvas_width;
	if (canvas_height - *h <= canvas_height / 200)
		*h = canvas_height;
	*x = (canvas_width - *w) / 2;
	*y = (canvas_height - *h) / 2;
}

/*
================
VID_ModeSize

The render size for a ladder rung on the current canvas.

Classic mode hands back the rung as authored (4:3). Widescreen mode keeps
the rung's short side -- which is what actually costs frame time -- and
extends the long side to the canvas aspect, so the image fills the panel
with no bars. Hexen II's "Hor+" FOV adaptation (fov_adapt, on by default)
turns the extra width into extra view rather than a stretched one, and the
2D canvases are centred by GL_SetCanvas, so the HUD and menus do not care.

The rasterizer's MAXWIDTH/MAXHEIGHT ceilings still bind: an extreme canvas
aspect gives up size on the long side rather than exceed them.
================
*/
static void VID_ModeSize (int mode, int *width, int *height)
{
	int	w, h;
	float	aspect;

	if (mode < 0 || mode >= NUM_SOFTMODES)
		mode = 0;
	w = softmodes[mode].width;
	h = softmodes[mode].height;

	if (vid_soft_widescreen.integer && canvas_width > 0 && canvas_height > 0)
	{
		aspect = (float)canvas_width / (float)canvas_height;
		if (aspect >= 4.0f / 3.0f)
		{
			w = (int)(h * aspect + 0.5f);
			if (w > MAXWIDTH)
			{
				w = MAXWIDTH;
				h = (int)(w / aspect + 0.5f);
			}
		}
		else	/* taller than 4:3: keep the width, show more vertically */
		{
			h = (int)(w / aspect + 0.5f);
			if (h > MAXHEIGHT)
			{
				h = MAXHEIGHT;
				w = (int)(h * aspect + 0.5f);
			}
		}
	}

	if (w < 320) w = 320;
	if (h < 200) h = 200;
	if (w > MAXWIDTH) w = MAXWIDTH;
	if (h > MAXHEIGHT) h = MAXHEIGHT;
	*width = w;
	*height = h;
}

/*
================
VID_ModeDesc

Human-readable size of a rung on this canvas, which is not the authored 4:3
size once widescreen is in play.
================
*/
static const char *VID_ModeDesc (int mode, char *buf, size_t bufsize)
{
	int	w, h;

	VID_ModeSize (mode, &w, &h);
	if (mode >= 0 && mode < NUM_SOFTMODES &&
	    w == softmodes[mode].width && h == softmodes[mode].height)
		q_strlcpy (buf, softmodes[mode].desc, bufsize);
	else
		q_snprintf (buf, bufsize, "%d x %d", w, h);
	return buf;
}

/*
================
VID_UpdateAspect

vid.aspect is the pixel aspect *as presented*: the ratio of the horizontal
and vertical scale factors the presenter applies. Filling and letterboxing
both preserve the render aspect, so it is 1 (square pixels); only
vid_soft_stretch makes it anything else. R_ViewChanged reads it when
fov_adapt is off, and it has to track the canvas, not just the mode.
================
*/
static void VID_UpdateAspect (void)
{
	int	dx, dy, dw, dh;
	float	aspect;

	if (vid.width <= 0 || vid.height <= 0)
		return;

	VID_DestRect (vid.width, vid.height, &dx, &dy, &dw, &dh);
	if (dw <= 0 || dh <= 0)
		return;

	aspect = ((float)dw * (float)vid.height) / ((float)dh * (float)vid.width);
	if (aspect != vid.aspect)
	{
		vid.aspect = aspect;
		vid.recalc_refdef = 1;
	}
}

/*
================
VID_AutoMode

Picks the ladder rung that upscales most cleanly onto the current canvas.
An exact integer upscale is always preferred because it is the only way to
keep the software renderer's pixels perfectly square and crisp; otherwise
the largest affordable rung wins.

"Exact" is tested by division, not by a tolerance: a scale of 6.95 is not an
integer upscale, and treating it as one used to hand a 2.5 K panel the
320 x 240 rung on the strength of a rounding fudge.
================
*/
static int VID_AutoMode (void)
{
	int	i, best = -1, largest = -1;

	for (i = 0; i < NUM_SOFTMODES; ++i)
	{
		int	w, h;
		int	dx, dy, dw, dh;

		/* the widened size is what the rasterizer actually pays for */
		VID_ModeSize (i, &w, &h);
		if (w * h > AUTO_MAX_PIXELS)
			continue;

		VID_DestRect (w, h, &dx, &dy, &dw, &dh);
		if (dh < h * AUTO_MIN_SCALE)
			continue;

		largest = i;

		if (dh % h == 0 && dw % w == 0)
			best = i;	/* exact integer upscale */
	}

	if (best >= 0)
		return best;
	if (largest >= 0)
		return largest;
	return 0;
}

static int VID_WantedMode (void)
{
	int	mode = vid_soft_mode.integer;

	if (mode >= 1 && mode <= NUM_SOFTMODES)
		return mode - 1;
	return VID_AutoMode ();
}

/*
================
VID_AllocBuffers

Framebuffer, z-buffer and surface cache for one software resolution. The
z-buffer and surface cache live on the high hunk exactly like the classic
backends so a mode change can release them in one shot.
================
*/
static void VID_AllocBuffers (int width, int height)
{
	int	zbuffersize, cachesize, pixels;

	pixels = width * height;
	zbuffersize = pixels * (int)sizeof(*d_pzbuffer);
	cachesize = D_SurfaceCacheForRes (width, height);

	if (vid_framebuffer_size < pixels)
	{
		byte *buf = (byte *) realloc (vid_framebuffer, pixels);
		if (!buf)
			Sys_Error ("Not enough memory for a %dx%d software framebuffer", width, height);
		vid_framebuffer = buf;
		vid_framebuffer_size = pixels;
	}
	memset (vid_framebuffer, 0, pixels);

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (vid_highhunkmark);
		d_pzbuffer = NULL;
	}

	vid_highhunkmark = Hunk_HighMark ();
	d_pzbuffer = (short *) Hunk_HighAllocName (zbuffersize + cachesize, "video");
	vid_surfcache = (byte *)d_pzbuffer + zbuffersize;
	vid_surfcachesize = cachesize;
}

/*
================
VID_SetSoftMode
================
*/
static void VID_SetSoftMode (int mode)
{
	int	width, height;

	if (mode < 0 || mode >= NUM_SOFTMODES)
		mode = 0;

	VID_ModeSize (mode, &width, &height);

	in_mode_set = true;
	VID_AllocBuffers (width, height);

	vid.width = vid.conwidth = width;
	vid.height = vid.conheight = height;
	vid.rowbytes = vid.conrowbytes = width;
	vid.buffer = vid.conbuffer = vid.direct = vid_framebuffer;
	vid.numpages = 1;
	vid.recalc_refdef = 1;
	VID_UpdateAspect ();

	D_InitCaches (vid_surfcache, vid_surfcachesize);
	WebCanvas_SetSource (width, height);

	/* Shared client code (menu.c) sizes its canvas from these. */
	glwidth = width;
	glheight = height;

	vid_current_mode = mode;
	vid_menu_mode = mode;
	in_mode_set = false;
}

static void VID_CheckMode (void)
{
	int	wanted = VID_WantedMode ();
	int	width, height;

	/* A rung's size is a function of the canvas aspect too, so an unchanged
	 * rung index does not mean an unchanged framebuffer. */
	VID_ModeSize (wanted, &width, &height);

	if (wanted != vid_current_mode || width != vid.width || height != vid.height)
	{
		char	desc[48];

		VID_SetSoftMode (wanted);
		Con_DPrintf ("software resolution: %s (canvas %dx%d, %s)\n",
			VID_ModeDesc (vid_current_mode, desc, sizeof(desc)),
			canvas_width, canvas_height, WebCanvas_BackendName ());
	}
	else
	{
		/* Same framebuffer, but the canvas (or the aspect policy) may have
		 * changed how it is presented. */
		VID_UpdateAspect ();
	}
}

/*
================
Web_ResizeCanvas

Called from the PWA launcher whenever the visual viewport changes. Sizes are
CSS pixels; the canvas backing store runs at full device resolution so the
GPU upscale of the software image stays as sharp as the panel allows.
================
*/
EMSCRIPTEN_KEEPALIVE void Web_ResizeCanvas (int css_width, int css_height)
{
	double	ratio = emscripten_get_device_pixel_ratio ();
	int	width, height;

	if (ratio <= 0.0)
		ratio = 1.0;
	width = (int)(css_width * ratio + 0.5);
	height = (int)(css_height * ratio + 0.5);
	if (width < 64) width = 64;
	if (height < 64) height = 64;

	emscripten_set_canvas_element_size ("#canvas", width, height);
	canvas_width = width;
	canvas_height = height;

	if (vid_initialized)
		VID_CheckMode ();
}

static void VID_QueryCanvasSize (void)
{
	double	css_width = 0, css_height = 0;
	double	ratio = emscripten_get_device_pixel_ratio ();

	if (ratio <= 0.0)
		ratio = 1.0;
	emscripten_get_element_css_size ("#canvas", &css_width, &css_height);
	if (css_width <= 0 || css_height <= 0)
	{
		css_width = 1024;
		css_height = 768;
	}
	canvas_width = (int)(css_width * ratio + 0.5);
	canvas_height = (int)(css_height * ratio + 0.5);
	emscripten_set_canvas_element_size ("#canvas", canvas_width, canvas_height);
}

static void VID_SoftMode_f (void)
{
	char	desc[48];
	int	i;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("software resolutions (vid_soft_mode):\n");
		Con_Printf ("  0 : auto (currently %s)\n",
			VID_ModeDesc (VID_AutoMode (), desc, sizeof(desc)));
		for (i = 0; i < NUM_SOFTMODES; ++i)
			Con_Printf ("%s%2d : %s\n", (i == vid_current_mode) ? "* " : "  ",
				i + 1, VID_ModeDesc (i, desc, sizeof(desc)));
		return;
	}
	Cvar_SetValueQuick (&vid_soft_mode, atoi (Cmd_Argv (1)));
	VID_CheckMode ();
}

void VID_Init (const unsigned char *palette)
{
	char	desc[48];

	Cvar_RegisterVariable (&vid_soft_mode);
	Cvar_RegisterVariable (&vid_soft_filter);
	Cvar_RegisterVariable (&vid_soft_widescreen);
	Cvar_RegisterVariable (&vid_soft_stretch);
	Cmd_AddCommand ("vid_softmode", VID_SoftMode_f);

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	VID_QueryCanvasSize ();
	WebCanvas_Init ();
	WebCanvas_SetFilter (vid_soft_filter.integer != 0);
	VID_SetPalette (palette);
	VID_SetSoftMode (VID_WantedMode ());

	vid_initialized = true;
	Con_SafePrintf ("Software renderer: %s on %s canvas %dx%d\n",
		VID_ModeDesc (vid_current_mode, desc, sizeof(desc)),
		WebCanvas_BackendName (), canvas_width, canvas_height);
}

void VID_Shutdown (void)
{
	vid_initialized = false;
	WebCanvas_Shutdown ();
	free (vid_framebuffer);
	vid_framebuffer = NULL;
	vid_framebuffer_size = 0;
	vid.buffer = vid.conbuffer = vid.direct = NULL;
}

void VID_Update (vrect_t *rects)
{
	int	dx, dy, dw, dh;
	static int last_filter = -1;

	(void) rects;	/* the whole frame is uploaded: GPU scaling is per-frame anyway */

	if (!vid_initialized || !vid_framebuffer)
		return;

	if (last_filter != vid_soft_filter.integer)
	{
		last_filter = vid_soft_filter.integer;
		WebCanvas_SetFilter (last_filter != 0);
	}

	VID_CheckMode ();
	VID_DestRect (vid.width, vid.height, &dx, &dy, &dw, &dh);
	WebCanvas_Present (vid_framebuffer, vid.rowbytes,
		canvas_width, canvas_height, dx, dy, dw, dh);
	WebPerf_CountUpload ((size_t)vid.rowbytes * vid.height);
}

void VID_SetPalette (const unsigned char *palette)
{
	int	i;

	if (!memcmp (vid_curpal, palette, sizeof(vid_curpal)))
		return;
	memcpy (vid_curpal, palette, sizeof(vid_curpal));

	for (i = 0; i < 256; ++i)
	{
		d_8to24table[i] = palette[i * 3] | (palette[i * 3 + 1] << 8) |
			(palette[i * 3 + 2] << 16) | 0xff000000u;
		d_8to16table[i] = (unsigned short)
			(((palette[i * 3] >> 3) << 11) |
			 ((palette[i * 3 + 1] >> 2) << 5) |
			  (palette[i * 3 + 2] >> 3));
	}
	d_8to24table[255] &= 0x00ffffffu;

	WebCanvas_SetPalette (palette);
}

void VID_ShiftPalette (const unsigned char *palette)
{
	VID_SetPalette (palette);
}

void VID_LockBuffer (void)
{
	if (++lockcount > 1)
		return;

	vid.buffer = vid.conbuffer = vid.direct = vid_framebuffer;
	vid.rowbytes = vid.conrowbytes = vid.width;

	if (r_dowarp)
	{
		d_viewbuffer = r_warpbuffer;
		screenwidth = WARP_WIDTH;
	}
	else
	{
		d_viewbuffer = vid.buffer;
		screenwidth = vid.rowbytes;
	}
}

void VID_UnlockBuffer (void)
{
	--lockcount;
	if (lockcount < 0)
		Sys_Error ("Unbalanced unlock");
}

void VID_HandlePause (qboolean paused) { (void)paused; }
void VID_ToggleFullscreen (void) {}
void D_ShowLoadingSize (void) {}
void VID_InitMouseCursors (void) {}
void VID_SetMouseCursor (mousecursor_t cursor) { (void)cursor; }

qboolean VID_HasMouseOrInputFocus (void) { return true; }

qboolean VID_IsMinimized (void)
{
	EmscriptenVisibilityChangeEvent status;
	return !emscripten_get_visibility_status (&status) && status.hidden;
}

/* The console shares the software framebuffer, so there is no separate
 * console size to scale -- these exist for the shared menu code. */
void VID_ChangeConsize (int dir) { (void)dir; }
float VID_ReportConsize (void) { return (float)vid.conwidth; }

void VID_MenuInit (void) { vid_menu_mode = vid_current_mode; }
qboolean VID_MenuNeedApply (void) { return false; }
void VID_MenuApply (void) {}
void VID_MenuReset (void) { vid_menu_mode = vid_current_mode; }

const char *VID_MenuGetResolution (qboolean *is_current)
{
	static char	desc[64];
	char		size[48];

	if (is_current)
		*is_current = true;
	VID_ModeDesc (vid_current_mode, size, sizeof(size));
	if (vid_soft_mode.integer == 0)
		q_snprintf (desc, sizeof(desc), "auto (%s)", size);
	else
		q_strlcpy (desc, size, sizeof(desc));
	return desc;
}

const char *VID_MenuGetAspect (void)
{
	if (vid_soft_widescreen.integer)
		return "Fill";
	return vid_soft_stretch.integer ? "Stretch" : "4:3";
}

int VID_MenuGetWindowMode (void) { return 1; }

int VID_MenuGetMultisample (qboolean *is_current, qboolean *available)
{
	if (is_current) *is_current = true;
	if (available) *available = false;
	return 0;
}

int VID_MenuGetVSync (void) { return 1; }

qboolean VID_MenuGetTexFilter (void) { return vid_soft_filter.integer != 0; }

int VID_MenuGetAnisotropy (qboolean *available)
{
	if (available) *available = false;
	return 1;
}

void VID_MenuAdjustWindowMode (int dir) { (void)dir; }

void VID_MenuAdjustAspect (int dir)
{
	/* Fill -> 4:3 -> Stretch, in that order both ways: three states over
	 * two archived cvars, so an old config keeps its meaning. */
	int	state = vid_soft_widescreen.integer ? 0 : (vid_soft_stretch.integer ? 2 : 1);

	state += (dir > 0) ? 1 : -1;
	if (state < 0)
		state = 2;
	else if (state > 2)
		state = 0;

	Cvar_SetValueQuick (&vid_soft_widescreen, (state == 0) ? 1 : 0);
	Cvar_SetValueQuick (&vid_soft_stretch, (state == 2) ? 1 : 0);
	VID_CheckMode ();
	vid.recalc_refdef = 1;
}

void VID_MenuAdjustResolution (int dir)
{
	int	mode = vid_soft_mode.integer + (dir > 0 ? 1 : -1);

	if (mode < 0)
		mode = NUM_SOFTMODES;
	else if (mode > NUM_SOFTMODES)
		mode = 0;
	Cvar_SetValueQuick (&vid_soft_mode, mode);
	VID_CheckMode ();
}

void VID_MenuAdjustMultisample (int dir) { (void)dir; }
void VID_MenuAdjustVSync (int dir) { (void)dir; }

void VID_MenuAdjustTexFilter (void)
{
	Cvar_SetValueQuick (&vid_soft_filter, vid_soft_filter.integer ? 0 : 1);
}

void VID_MenuAdjustAnisotropy (int dir) { (void)dir; }
