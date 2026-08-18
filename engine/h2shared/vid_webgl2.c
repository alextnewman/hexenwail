#include "quakedef.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

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

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE webgl_context;

static void VID_SetSize (int width, int height)
{
	if (width < 320) width = 320;
	if (height < 200) height = 200;
	emscripten_set_canvas_element_size("#canvas", width, height);
	vid.width = vid.conwidth = width;
	vid.height = vid.conheight = height;
	vid.rowbytes = vid.conrowbytes = width;
	vid.aspect = ((float)height / (float)width) * (320.0f / 240.0f);
	vid.numpages = 2;
	vid.recalc_refdef = 1;
	WebGL2_Resize(width, height);
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
	EmscriptenWebGLContextAttributes attributes;
	double width = 0, height = 0;
	int i;

	emscripten_webgl_init_context_attributes(&attributes);
	attributes.alpha = false;
	attributes.depth = true;
	attributes.stencil = false;
	attributes.antialias = false;
	attributes.premultipliedAlpha = false;
	attributes.preserveDrawingBuffer = false;
	attributes.majorVersion = 2;
	attributes.minorVersion = 0;
	attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
	webgl_context = emscripten_webgl_create_context("#canvas", &attributes);
	if (webgl_context <= 0)
		Sys_Error("Unable to create an iOS WebGL2 context");
	emscripten_webgl_make_context_current(webgl_context);

	for (i = 0; i < 256; ++i)
	{
		d_8to24table[i] = palette[i * 3] |
			(palette[i * 3 + 1] << 8) |
			(palette[i * 3 + 2] << 16) | 0xff000000u;
	}
	d_8to24table[255] &= 0x00ffffffu;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
	emscripten_get_element_css_size("#canvas", &width, &height);
	if (width <= 0 || height <= 0)
	{
		width = 960;
		height = 540;
	}
	VID_SetSize((int)(width * emscripten_get_device_pixel_ratio()),
		(int)(height * emscripten_get_device_pixel_ratio()));
	WebGL2_Init();
}

void VID_Shutdown (void)
{
	WebGL2_Shutdown();
	if (webgl_context > 0)
		emscripten_webgl_destroy_context(webgl_context);
	webgl_context = 0;
}

void VID_Update (vrect_t *rects) { (void)rects; WebGL2_EndFrame(); }
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
