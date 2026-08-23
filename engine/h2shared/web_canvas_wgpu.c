/*
 * web_canvas_wgpu.c -- WebGPU presenter for the 8bpp software renderer.
 *
 * The launcher acquires WebGPU asynchronously before the engine starts.  The
 * JavaScript library linked into this bundle owns the WebGPU objects; this
 * file keeps the synchronous web_canvas.h contract used by vid_soft_web.c.
 */

#include "quakedef.h"
#include "web_canvas.h"

#include <emscripten/emscripten.h>

extern int WebGPUCanvas_Init (void);
extern void WebGPUCanvas_Shutdown (void);
extern void WebGPUCanvas_SetSource (int width, int height);
extern void WebGPUCanvas_SetPalette (const byte *palette);
extern void WebGPUCanvas_SetFilter (int smooth);
extern void WebGPUCanvas_Present (const byte *pixels, int rowbytes,
			int canvas_w, int canvas_h,
			int dst_x, int dst_y, int dst_w, int dst_h);

void WebCanvas_Init (void)
{
	if (!WebGPUCanvas_Init ())
		Sys_Error ("WebGPU presenter was not initialized by the launcher");
}

void WebCanvas_Shutdown (void)
{
	WebGPUCanvas_Shutdown ();
}

const char *WebCanvas_BackendName (void)
{
	return "webgpu";
}

void WebCanvas_SetSource (int width, int height)
{
	WebGPUCanvas_SetSource (width, height);
}

void WebCanvas_SetPalette (const byte *palette)
{
	WebGPUCanvas_SetPalette (palette);
}

void WebCanvas_SetFilter (qboolean smooth)
{
	WebGPUCanvas_SetFilter (smooth ? 1 : 0);
}

void WebCanvas_Present (const byte *pixels, int rowbytes,
			int canvas_w, int canvas_h,
			int dst_x, int dst_y, int dst_w, int dst_h)
{
	WebGPUCanvas_Present (pixels, rowbytes, canvas_w, canvas_h,
			dst_x, dst_y, dst_w, dst_h);
}
