/*
 * web_perf.c -- frame instrumentation and the on-screen performance overlay
 * for the web platform.  See web_perf.h and docs/web/PERF_OVERLAY.md.
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

#include "quakedef.h"
#include "web_perf.h"

/* 0 off, 1 summary, 2 + stage breakdown and counters, 3 + frame graph.
 * Archived: both renderer bundles share one config.cfg, and the cvar means
 * the same thing in both, so there is no cross-renderer conflict. */
static cvar_t	scr_perf = {"scr_perf", "0", CVAR_ARCHIVE};

webperf_counters_t	webperf;

#define PERF_HISTORY	128		/* frames kept for the graph and the lows */
#define PERF_GRAPH_H	32		/* graph height, in canvas pixels */
#define PERF_HITCH_MS	1000.0		/* longer than this is a load stall, not a frame */

typedef struct
{
	double	interval_ms;			/* wall time between presented frames */
	double	stage_ms[WEBPERF_STAGE_COUNT];
	double	other_ms;			/* interval minus the measured stages */
	webperf_counters_t counters;
} webperf_frame_t;

static struct
{
	qboolean	active;			/* latched at frame start */
	double		frame_begin;		/* Sys_DoubleTime at BeginFrame */
	double		prev_begin;
	double		stage_begin[WEBPERF_STAGE_COUNT];
	double		stage_ms[WEBPERF_STAGE_COUNT];

	webperf_frame_t	shown;			/* last completed frame */
	float		history[PERF_HISTORY];	/* interval_ms ring */
	int		history_count;
	int		history_next;
} perf;

void WebPerf_Init (void)
{
	Cvar_RegisterVariable (&scr_perf);
}

void WebPerf_BeginFrame (void)
{
	double	now = Sys_DoubleTime ();
	int	i;

	perf.active = (scr_perf.integer > 0);
	perf.frame_begin = now;

	for (i = 0; i < WEBPERF_STAGE_COUNT; i++)
	{
		perf.stage_ms[i] = 0;
		perf.stage_begin[i] = 0;
	}
	memset (&webperf, 0, sizeof(webperf));
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
	double	interval, measured;
	int	i;

	if (!perf.active)
	{	/* keep the reference point fresh so switching the overlay on
		 * does not report one absurd interval */
		perf.prev_begin = perf.frame_begin;
		return;
	}

	interval = (perf.prev_begin > 0) ?
			(perf.frame_begin - perf.prev_begin) * 1000.0 : 0;
	perf.prev_begin = perf.frame_begin;

	measured = 0;
	for (i = 0; i < WEBPERF_STAGE_COUNT; i++)
	{
		perf.shown.stage_ms[i] = perf.stage_ms[i];
		measured += perf.stage_ms[i];
	}

	/* The interval is measured between frame starts, so it belongs to the
	 * previous frame's work; the stages below are this frame's.  They are
	 * shown side by side because on a steady frame rate they describe the
	 * same frame, and the difference is exactly the point of "other". */
	perf.shown.interval_ms = interval;
	perf.shown.other_ms = (interval > measured) ? interval - measured : 0;
	perf.shown.counters = webperf;

	if (interval > 0 && interval < PERF_HITCH_MS)
	{
		perf.history[perf.history_next] = (float) interval;
		perf.history_next = (perf.history_next + 1) % PERF_HISTORY;
		if (perf.history_count < PERF_HISTORY)
			perf.history_count++;
	}
}

/*
 * Worst frame and the 1%-low (99th percentile) frame time over the history
 * window.  Both are what you actually feel; the average hides hitches.
 */
static void WebPerf_HistoryStats (double *worst_ms, double *low_ms, double *avg_ms)
{
	float	sorted[PERF_HISTORY];
	double	total = 0;
	int	i, j, n = perf.history_count;

	*worst_ms = *low_ms = *avg_ms = 0;
	if (n <= 0)
		return;

	for (i = 0; i < n; i++)
	{
		sorted[i] = perf.history[i];
		total += sorted[i];
	}

	/* insertion sort: n <= 128, and only while the overlay is visible */
	for (i = 1; i < n; i++)
	{
		float	v = sorted[i];
		for (j = i - 1; j >= 0 && sorted[j] > v; j--)
			sorted[j + 1] = sorted[j];
		sorted[j + 1] = v;
	}

	*avg_ms = total / n;
	*worst_ms = sorted[n - 1];
	*low_ms = sorted[(n * 99) / 100 >= n ? n - 1 : (n * 99) / 100];
}

static const char *WebPerf_RendererName (void)
{
#if defined(WEBGL2QUAKE)
	return "webglide";
#else
	return "software";
#endif
}

static void WebPerf_DrawGraph (int x, int y, int w)
{
	const double	target_ms = 1000.0 / 60.0;
	int		i, count = perf.history_count;
	int		first;

	if (count <= 0)
		return;
	if (count > w)
		count = w;
	first = (perf.history_next - count + PERF_HISTORY) % PERF_HISTORY;

	Draw_FillAlpha (x, y, w, PERF_GRAPH_H, 0.0f, 0.0f, 0.0f, 0.5f);

	for (i = 0; i < count; i++)
	{
		double	ms = perf.history[(first + i) % PERF_HISTORY];
		int	h = (int) ((ms / (target_ms * 2.0)) * PERF_GRAPH_H);
		float	r = 0.3f, g = 1.0f, b = 0.3f;

		if (h < 1)
			h = 1;
		if (h > PERF_GRAPH_H)
			h = PERF_GRAPH_H;
		if (ms > target_ms * 1.5)
		{	/* missed even a 40 Hz frame */
			r = 1.0f; g = 0.25f; b = 0.25f;
		}
		else if (ms > target_ms)
		{
			r = 1.0f; g = 0.85f; b = 0.2f;
		}
		Draw_FillAlpha (x + i, y + PERF_GRAPH_H - h, 1, h, r, g, b, 0.9f);
	}

	/* the 60 Hz budget line */
	Draw_FillAlpha (x, y + PERF_GRAPH_H / 2, w, 1, 1.0f, 1.0f, 1.0f, 0.35f);
}

void WebPerf_Draw (void)
{
	char	line[64];
	double	worst_ms, low_ms, avg_ms, fps;
	int	level = scr_perf.integer;
	int	x, y, w, h, lines;

	if (level <= 0)
		return;

	GL_SetCanvas (CANVAS_DEFAULT);

	WebPerf_HistoryStats (&worst_ms, &low_ms, &avg_ms);
	fps = (avg_ms > 0) ? 1000.0 / avg_ms : 0;

	lines = 1;
	if (level >= 2)
		lines += 3;

	x = 8;
	y = 8;
	w = 8 * 30 + 8;
	h = lines * 8 + 8 + ((level >= 3) ? PERF_GRAPH_H + 4 : 0);
	if (w > vid.width - 16)
		w = vid.width - 16;

	Draw_FillAlpha (x - 4, y - 4, w, h, 0.0f, 0.0f, 0.0f, 0.55f);

	q_snprintf (line, sizeof(line), "%s %3dx%-3d %5.1f fps %5.1f ms",
		WebPerf_RendererName (), vid.width, vid.height, fps,
		perf.shown.interval_ms);
	Draw_String (x, y, line);
	y += 8;

	if (level >= 2)
	{
		q_snprintf (line, sizeof(line), "view%5.1f 2d%4.1f pre%4.1f oth%5.1f",
			perf.shown.stage_ms[WEBPERF_REFRESH],
			perf.shown.stage_ms[WEBPERF_2D],
			perf.shown.stage_ms[WEBPERF_PRESENT],
			perf.shown.other_ms);
		Draw_String (x, y, line);
		y += 8;

		if (perf.shown.counters.drawcalls > 0)
			q_snprintf (line, sizeof(line), "draws%4d  tris%7d",
				perf.shown.counters.drawcalls,
				perf.shown.counters.tris);
		else
			q_snprintf (line, sizeof(line), "uploads%3d   %6d KB",
				perf.shown.counters.uploads,
				perf.shown.counters.uploadkb);
		Draw_String (x, y, line);
		y += 8;

		q_snprintf (line, sizeof(line), "worst%6.1f ms  1%% low%5.1f fps",
			worst_ms, (low_ms > 0) ? 1000.0 / low_ms : 0);
		Draw_String (x, y, line);
		y += 8;
	}

	if (level >= 3)
	{
		WebPerf_DrawGraph (x, y + 2, (w > 8) ? w - 8 : w);
		y += PERF_GRAPH_H + 4;
	}
}
