/*
 * wgpu_lightvol.c -- coarse shared irradiance for WebGlideNitro entities.
 *
 * The authored BSP lightmaps remain the authority for world surfaces.  This
 * volume samples that same authored lighting in six directions so aliases,
 * brush-adjacent actors and the view weapon inhabit the world's atmosphere
 * instead of using one downward trace and a fixed light vector.
 */

#include "quakedef.h"
#include "wgpu_nitro.h"

#define NITRO_LIGHTVOL_MAX_BYTES	(2 * 1024 * 1024)
#define NITRO_LIGHTVOL_MIN_CELL		32
#define NITRO_LIGHTVOL_MAX_CELL		512

static wgpulightcell_t	*lightvol_cells;
static byte		*lightvol_valid;
static vec3_t		lightvol_mins;
static int		lightvol_dims[3];
static int		lightvol_total;
static int		lightvol_cellsize;
static int		lightvol_budget;
static int		lightvol_refresh_cursor;
static float		lightvol_trace_distance;

static int WGPULightVol_Index (int x, int y, int z)
{
	return (z * lightvol_dims[1] + y) * lightvol_dims[0] + x;
}

static int WGPULightVol_SnapCellSize (float requested)
{
	int	size = NITRO_LIGHTVOL_MIN_CELL;

	if (requested > (float)NITRO_LIGHTVOL_MIN_CELL)
	{
		while (size < (int)requested && size < NITRO_LIGHTVOL_MAX_CELL)
			size <<= 1;
	}
	return CLAMP(NITRO_LIGHTVOL_MIN_CELL, size, NITRO_LIGHTVOL_MAX_CELL);
}

static qboolean WGPULightVol_Layout (qmodel_t *world, int cellsize)
{
	double	total;
	int	i;

	for (i = 0; i < 3; i++)
	{
		double extent = (double)world->maxs[i] - (double)world->mins[i];

		lightvol_mins[i] = world->mins[i] - cellsize;
		lightvol_dims[i] = (int)ceil (extent / cellsize) + 2;
		if (lightvol_dims[i] < 2)
			lightvol_dims[i] = 2;
	}

	total = (double)lightvol_dims[0] * lightvol_dims[1] * lightvol_dims[2];
	if (total <= 0 || total > INT_MAX)
		return false;
	if (total * (sizeof(wgpulightcell_t) + sizeof(byte)) >
	    NITRO_LIGHTVOL_MAX_BYTES)
		return false;

	lightvol_total = (int)total;
	return true;
}

void WGPULightVol_Shutdown (void)
{
	free (lightvol_cells);
	free (lightvol_valid);
	lightvol_cells = NULL;
	lightvol_valid = NULL;
	memset (lightvol_dims, 0, sizeof(lightvol_dims));
	VectorClear (lightvol_mins);
	lightvol_total = 0;
	lightvol_cellsize = 0;
	lightvol_budget = 0;
	lightvol_refresh_cursor = 0;
	lightvol_trace_distance = 0;
}

void WGPULightVol_NewMap (void)
{
	qmodel_t	*world = cl.worldmodel;
	int		cellsize, i;

	WGPULightVol_Shutdown ();
	if (!world || !world->nodes || !world->lightdata)
		return;

	cellsize = WGPULightVol_SnapCellSize (r_nitro_lightvol_cell.value);
	while (!WGPULightVol_Layout (world, cellsize) &&
	       cellsize < NITRO_LIGHTVOL_MAX_CELL)
		cellsize <<= 1;
	if (!lightvol_total)
	{
		Con_DPrintf ("WebGlideNitro: light volume exceeds its 2 MB budget\n");
		return;
	}

	lightvol_cells = (wgpulightcell_t *) calloc ((size_t)lightvol_total,
						    sizeof(*lightvol_cells));
	lightvol_valid = (byte *) calloc ((size_t)lightvol_total, sizeof(*lightvol_valid));
	if (!lightvol_cells || !lightvol_valid)
	{
		Con_DPrintf ("WebGlideNitro: could not allocate light volume\n");
		WGPULightVol_Shutdown ();
		return;
	}

	lightvol_cellsize = cellsize;
	for (i = 0; i < 3; i++)
	{
		float extent = world->maxs[i] - world->mins[i] + cellsize * 2.0f;

		if (extent > lightvol_trace_distance)
			lightvol_trace_distance = extent;
	}
}

void WGPULightVol_BeginFrame (void)
{
	int	invalidate, i;

	lightvol_budget = CLAMP(1, r_nitro_lightvol_budget.integer, 4096);
	if (!lightvol_valid || !lightvol_total)
		return;

	/* Light styles are part of a resolved probe.  Invalidating a small
	 * round-robin slice lets visible cells pick up animation lazily without
	 * rebuilding the map-sized field on the WASM thread. */
	invalidate = q_max(lightvol_budget / 4, 1);
	for (i = 0; i < invalidate; i++)
	{
		lightvol_valid[lightvol_refresh_cursor] = 0;
		lightvol_refresh_cursor++;
		if (lightvol_refresh_cursor == lightvol_total)
			lightvol_refresh_cursor = 0;
	}
}

static qboolean WGPULightVol_Resolve (int x, int y, int z)
{
	static const float axes[6][3] =
	{
		{ 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
		{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
	};
	wgpulightcell_t	*cell;
	vec3_t		point, end, direction;
	float		samples[6], sum, length;
	int		index, axis, i;

	index = WGPULightVol_Index (x, y, z);
	if (lightvol_valid[index])
		return true;
	if (lightvol_budget <= 0)
		return false;
	lightvol_budget--;

	for (i = 0; i < 3; i++)
		point[i] = lightvol_mins[i] +
			((i == 0 ? x : (i == 1 ? y : z)) + 0.5f) * lightvol_cellsize;

	sum = 0;
	for (axis = 0; axis < 6; axis++)
	{
		VectorMA (point, lightvol_trace_distance, axes[axis], end);
		samples[axis] = (float)WGPUWorld_TraceLight (point, end, NULL);
		if (samples[axis] < 0)
			samples[axis] = 0;
		sum += samples[axis];
	}

	/* Store light travel, from source towards the sample.  The alias
	 * rasteriser brightens normals whose dot product with this vector is
	 * negative, matching its historical convention. */
	if (sum > 0.0f)
	{
		direction[0] = (samples[1] - samples[0]) / sum;
		direction[1] = (samples[3] - samples[2]) / sum;
		direction[2] = (samples[5] - samples[4]) / sum;
		length = VectorLength (direction);
		if (length > 1.0f)
			VectorScale (direction, 1.0f / length, direction);
	}
	else
	{
		VectorClear (direction);
	}

	cell = &lightvol_cells[index];
	for (i = 0; i < 3; i++)
		cell->direction[i] = (byte)CLAMP(0,
			(int)((direction[i] * 0.5f + 0.5f) * 255.0f + 0.5f), 255);
	cell->ambient = (byte)CLAMP(0, (int)(sum / 6.0f + 0.5f), 255);
	lightvol_valid[index] = 1;
	return true;
}

static void WGPULightVol_Dynamic (const vec3_t point, wgpulightsample_t *sample)
{
	vec3_t	weighted, delta;
	int	i;

	VectorScale (sample->direction, sample->shade, weighted);
	if (r_dynamic.integer)
	{
		for (i = 0; i < MAX_DLIGHTS; i++)
		{
			const dlight_t	*light = &cl_dlights[i];
			float		distance, add;

			if (light->die < cl.time || !light->radius)
				continue;
			VectorSubtract (point, light->origin, delta);
			distance = VectorLengthFast (delta);
			add = light->radius - distance;
			if (add <= 0)
				continue;
			if (light->dark)
			{
				sample->ambient -= add;
				continue;
			}
			sample->ambient += add;
			if (distance > 0.001f)
				VectorMA (weighted, add / distance, delta, weighted);
		}
	}

	sample->shade = VectorLength (weighted);
	if (sample->shade > 0.001f)
		VectorScale (weighted, 1.0f / sample->shade, sample->direction);
	else
		VectorClear (sample->direction);
	if (sample->ambient < 0)
		sample->ambient = 0;
}

qboolean WGPULightVol_Sample (const vec3_t point, wgpulightsample_t *sample)
{
	vec3_t	grid, direction;
	float	ambient, weight;
	int	base[3], x, y, z, i;

	memset (sample, 0, sizeof(*sample));
	if (!r_nitro_lightvol.integer || !lightvol_cells || !lightvol_valid)
		return false;

	for (i = 0; i < 3; i++)
	{
		grid[i] = (point[i] - lightvol_mins[i]) / lightvol_cellsize - 0.5f;
		base[i] = (int)floor (grid[i]);
		if (base[i] < 0 || base[i] >= lightvol_dims[i] - 1)
			return false;
		grid[i] -= base[i];
	}

	ambient = 0;
	VectorClear (direction);
	for (z = 0; z < 2; z++)
	for (y = 0; y < 2; y++)
	for (x = 0; x < 2; x++)
	{
		const wgpulightcell_t	*cell;
		vec3_t			cell_direction;
		int			index;

		if (!WGPULightVol_Resolve (base[0] + x, base[1] + y, base[2] + z))
			return false;
		index = WGPULightVol_Index (base[0] + x, base[1] + y, base[2] + z);
		cell = &lightvol_cells[index];
		weight = (x ? grid[0] : 1.0f - grid[0]) *
			 (y ? grid[1] : 1.0f - grid[1]) *
			 (z ? grid[2] : 1.0f - grid[2]);
		ambient += cell->ambient * weight;
		for (i = 0; i < 3; i++)
			cell_direction[i] = cell->direction[i] * (2.0f / 255.0f) - 1.0f;
		VectorMA (direction, weight * cell->ambient, cell_direction, direction);
	}

	sample->ambient = ambient;
	sample->shade = VectorLength (direction);
	if (sample->shade > 0.001f)
		VectorScale (direction, 1.0f / sample->shade, sample->direction);
	WGPULightVol_Dynamic (point, sample);
	sample->resolved = true;
	return true;
}

void WGPULightVol_Stats (int *cells, int *resolved, int *cellsize)
{
	int	count = 0, i;

	for (i = 0; lightvol_valid && i < lightvol_total; i++)
		count += lightvol_valid[i] != 0;
	if (cells)
		*cells = lightvol_total;
	if (resolved)
		*resolved = count;
	if (cellsize)
		*cellsize = lightvol_cellsize;
}
