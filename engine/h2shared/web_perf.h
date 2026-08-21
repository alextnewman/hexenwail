/*
 * web_perf.h -- low-overhead frame capture for the web platform.
 *
 * The engine runs on one WASM thread, so the interesting question is always
 * "which stage of this frame spent the milliseconds": the 3D refresh, the 2D
 * layer, presentation, other engine work, or time between rAF callbacks.
 *
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

#ifndef __HX2_WEB_PERF_H
#define __HX2_WEB_PERF_H

typedef enum
{
	WEBPERF_REFRESH = 0,	/* V_RenderView: the 3D scene */
	WEBPERF_2D,		/* HUD, menus and console */
	WEBPERF_PRESENT,	/* VID_Update: scan-out / framebuffer upload */
	WEBPERF_STAGE_COUNT
} webperf_stage_t;

/* Per-frame counters.  Not every field is meaningful in every renderer. */
typedef struct
{
	int	drawcalls;	/* GPU draw calls (WebGlide) */
	int	tris;		/* triangles submitted (WebGlide) */
	int	uploads;	/* texture/buffer uploads issued */
	int	uploadkb;	/* kilobytes those uploads carried */
} webperf_counters_t;

extern webperf_counters_t	webperf;

#define WebPerf_CountDraw(numtris) \
	do { webperf.drawcalls++; webperf.tris += (numtris); } while (0)
#define WebPerf_CountUpload(numbytes) \
	do { webperf.uploads++; webperf.uploadkb += (int)(((numbytes) + 1023) / 1024); } while (0)

void WebPerf_Init (void);		/* registers scr_perf; call from SCR_Init */

void WebPerf_BeginHostFrame (void);
void WebPerf_EndHostFrame (void);
void WebPerf_BeginStage (webperf_stage_t stage);
void WebPerf_EndStage (webperf_stage_t stage);

#endif	/* __HX2_WEB_PERF_H */
