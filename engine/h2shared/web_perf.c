/*
 * web_perf.c -- low-overhead frame capture for the web platform.
 *
 * Copyright (C) 2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "web_perf.h"

#include <emscripten/emscripten.h>

static cvar_t	scr_perf = {"scr_perf", "0", CVAR_ARCHIVE};

webperf_counters_t	webperf;

#define PERF_CAPTURE_FRAMES	128
#define PERF_REPORT_BYTES	24576
#define PERF_HITCH_MS		1000.0

typedef struct
{
	unsigned int	sequence;
	double		interval_ms;
	double		host_ms;
	double		wait_ms;
	double		stage_ms[WEBPERF_STAGE_COUNT];
	double		other_ms;
	webperf_counters_t counters;
} webperf_frame_t;

static struct
{
	qboolean	active;
	qboolean	host_complete;
	double		host_begin;
	double		prev_host_begin;
	double		host_ms;
	double		stage_begin[WEBPERF_STAGE_COUNT];
	double		stage_ms[WEBPERF_STAGE_COUNT];
	unsigned int	sequence;
	webperf_frame_t	frames[PERF_CAPTURE_FRAMES];
	int		frame_count;
} perf;

static const char *WebPerf_RendererName (void)
{
#if defined(WEBGL2QUAKE)
	return "webglide";
#else
	return "software";
#endif
}

static void WebPerf_EmitReport (void)
{
	static char	report[PERF_REPORT_BYTES];
	size_t		used;
	int		i;

	q_snprintf (report, sizeof(report),
		"hexenwail_perf_v1\n"
		"renderer=%s\n"
		"resolution=%dx%d\n"
		"frames=%d\n"
		"frame,interval_ms,host_ms,callback_wait_ms,view_ms,ui_ms,"
		"present_ms,engine_other_ms,draws,tris,uploads,upload_kb\n",
		WebPerf_RendererName (), vid.width, vid.height, perf.frame_count);
	used = strlen (report);

	for (i = 0; i < perf.frame_count && used < sizeof(report) - 1; i++)
	{
		const webperf_frame_t *frame = &perf.frames[i];

		q_snprintf (report + used, sizeof(report) - used,
			"%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d\n",
			frame->sequence, frame->interval_ms, frame->host_ms,
			frame->wait_ms, frame->stage_ms[WEBPERF_REFRESH],
			frame->stage_ms[WEBPERF_2D],
			frame->stage_ms[WEBPERF_PRESENT], frame->other_ms,
			frame->counters.drawcalls, frame->counters.tris,
			frame->counters.uploads, frame->counters.uploadkb);
		used = strlen (report);
	}

	EM_ASM({
		dispatchEvent(new CustomEvent('hexenwailperf',
			{ detail: UTF8ToString($0) }));
	}, report);
	perf.frame_count = 0;
}

static void WebPerf_StorePreviousFrame (double now)
{
	webperf_frame_t	*frame;
	double		interval, measured;
	int		i;

	if (!perf.active || !perf.host_complete || perf.prev_host_begin <= 0)
		return;

	interval = (now - perf.prev_host_begin) * 1000.0;
	if (interval <= 0 || interval >= PERF_HITCH_MS)
		return;

	frame = &perf.frames[perf.frame_count++];
	frame->sequence = ++perf.sequence;
	frame->interval_ms = interval;
	frame->host_ms = perf.host_ms;
	frame->wait_ms = (interval > perf.host_ms) ? interval - perf.host_ms : 0;
	frame->counters = webperf;

	measured = 0;
	for (i = 0; i < WEBPERF_STAGE_COUNT; i++)
	{
		frame->stage_ms[i] = perf.stage_ms[i];
		measured += perf.stage_ms[i];
	}
	frame->other_ms = (perf.host_ms > measured) ? perf.host_ms - measured : 0;

	if (perf.frame_count == PERF_CAPTURE_FRAMES)
		WebPerf_EmitReport ();
}

void WebPerf_Init (void)
{
	Cvar_RegisterVariable (&scr_perf);
}

void WebPerf_BeginHostFrame (void)
{
	double	now = Sys_DoubleTime ();
	int	i;

	WebPerf_StorePreviousFrame (now);
	perf.active = (scr_perf.integer > 0);
	perf.host_begin = now;
	perf.prev_host_begin = now;
	perf.host_complete = false;
	perf.host_ms = 0;
	for (i = 0; i < WEBPERF_STAGE_COUNT; i++)
	{
		perf.stage_begin[i] = 0;
		perf.stage_ms[i] = 0;
	}
	memset (&webperf, 0, sizeof(webperf));
}

void WebPerf_EndHostFrame (void)
{
	if (!perf.active)
		return;
	perf.host_ms = (Sys_DoubleTime () - perf.host_begin) * 1000.0;
	perf.host_complete = true;
}

void WebPerf_BeginFrame (void)
{
}

void WebPerf_BeginStage (webperf_stage_t stage)
{
	if (!perf.active)
		return;
	perf.stage_begin[stage] = Sys_DoubleTime ();
}

void WebPerf_EndStage (webperf_stage_t stage)
{
	if (!perf.active || perf.stage_begin[stage] == 0)
		return;
	perf.stage_ms[stage] += (Sys_DoubleTime () - perf.stage_begin[stage]) * 1000.0;
	perf.stage_begin[stage] = 0;
}

void WebPerf_EndFrame (void)
{
}
