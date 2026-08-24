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
#define NITRO_LIGHTVOL_USED_MAX		4096

static wgpulightcell_t	*lightvol_cells;
static byte		*lightvol_valid;
static byte		*lightvol_used;
static byte		lightvol_styles[MAX_LIGHTSTYLES];
static int		lightvol_used_indices[NITRO_LIGHTVOL_USED_MAX];
static int		lightvol_used_count;
static vec3_t		lightvol_mins;
static int		lightvol_dims[3];
static int		lightvol_total;
static int		lightvol_cellsize;
static int		lightvol_budget;
static int		lightvol_refresh_budget;
static unsigned int	lightvol_style_hash;

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
	if (total * (sizeof(wgpulightcell_t) + sizeof(byte) * 2) >
	    NITRO_LIGHTVOL_MAX_BYTES)
		return false;

	lightvol_total = (int)total;
	return true;
}

void WGPULightVol_Shutdown (void)
{
	free (lightvol_cells);
	free (lightvol_valid);
	free (lightvol_used);
	lightvol_cells = NULL;
	lightvol_valid = NULL;
	lightvol_used = NULL;
	memset (lightvol_dims, 0, sizeof(lightvol_dims));
	VectorClear (lightvol_mins);
	lightvol_total = 0;
	lightvol_cellsize = 0;
	lightvol_budget = 0;
	lightvol_refresh_budget = 0;
	memset (lightvol_styles, 0, sizeof(lightvol_styles));
	lightvol_used_count = 0;
	lightvol_style_hash = 0;
}

void WGPULightVol_NewMap (void)
{
	qmodel_t	*world = cl.worldmodel;
	int		cellsize, surfnum, map;

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
	lightvol_used = (byte *) calloc ((size_t)lightvol_total, sizeof(*lightvol_used));
	if (!lightvol_cells || !lightvol_valid || !lightvol_used)
	{
		Con_DPrintf ("WebGlideNitro: could not allocate light volume\n");
		WGPULightVol_Shutdown ();
		return;
	}

	lightvol_cellsize = cellsize;
	for (surfnum = 0; surfnum < world->numsurfaces; surfnum++)
		for (map = 0; map < MAXLIGHTMAPS &&
		     world->surfaces[surfnum].styles[map] != 255; map++)
			lightvol_styles[world->surfaces[surfnum].styles[map]] = 1;
}

void WGPULightVol_BeginFrame (void)
{
	unsigned int	hash = 2166136261u;
	int		i;

	lightvol_budget = CLAMP(1, r_nitro_lightvol_budget.integer, 4096);
	lightvol_refresh_budget = lightvol_budget / 2;
	if (!lightvol_valid || !lightvol_used || !lightvol_total)
		return;

	for (i = 0; i < MAX_LIGHTSTYLES; i++)
	{
		if (!lightvol_styles[i])
			continue;
		hash ^= (unsigned int)d_lightstylevalue[i];
		hash *= 16777619u;
	}
	if (lightvol_style_hash && hash != lightvol_style_hash)
		for (i = 0; i < lightvol_used_count; i++)
			if (lightvol_valid[lightvol_used_indices[i]] == 1)
				lightvol_valid[lightvol_used_indices[i]] = 3;
	lightvol_style_hash = hash;
	for (i = 0; i < lightvol_used_count; i++)
		lightvol_used[lightvol_used_indices[i]] = 0;
	lightvol_used_count = 0;
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
	if (lightvol_valid[index] == 1 || lightvol_valid[index] == 2)
		return true;
	if (lightvol_valid[index] == 3 && lightvol_refresh_budget <= 0)
		return true;
	if (lightvol_budget <= 0)
		return lightvol_valid[index] == 3;
	lightvol_budget--;
	if (lightvol_valid[index] == 3)
		lightvol_refresh_budget--;

	for (i = 0; i < 3; i++)
		point[i] = lightvol_mins[i] +
			((i == 0 ? x : (i == 1 ? y : z)) + 0.5f) * lightvol_cellsize;
	if (Mod_PointInLeaf (point, cl.worldmodel)->contents == CONTENTS_SOLID)
	{
		lightvol_valid[index] = 2;
		return true;
	}

	sum = 0;
	for (axis = 0; axis < 6; axis++)
	{
		VectorMA (point, 2048.0f, axes[axis], end);
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
	cell->ambient = (byte)CLAMP(0, WGPUWorld_LightPoint (point, NULL), 255);
	lightvol_valid[index] = 1;
	return true;
}

void WGPULightVol_ApplyDynamic (const vec3_t point, wgpulightsample_t *sample)
{
	vec3_t	weighted, delta;
	float	static_shade;
	int	i;

	static_shade = sample->shade;
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
			if (!WGPUWorld_LineVisible (light->origin, point))
				continue;
			if (light->dark)
			{
				sample->ambient -= add;
				sample->color[0] -= add;
				sample->color[1] -= add;
				sample->color[2] -= add;
				continue;
			}
			sample->ambient += add;
			{
				vec3_t	color;
				float	luminance;

				VectorCopy (light->color, color);
				luminance = color[0] * 0.2126f + color[1] * 0.7152f +
					color[2] * 0.0722f;
				if (luminance <= 0.001f)
					color[0] = color[1] = color[2] = 1.0f;
				else
					VectorScale (color, 1.0f / luminance, color);
				VectorMA (sample->color, add, color, sample->color);
			}
			if (distance > 0.001f)
				VectorMA (weighted, add / distance, delta, weighted);
		}
	}

	sample->shade = VectorLength (weighted);
	if (sample->shade > 0.001f)
		VectorScale (weighted, 1.0f / sample->shade, sample->direction);
	else
		VectorClear (sample->direction);
	if (sample->shade < static_shade)
		sample->shade = static_shade;
	if (sample->ambient < 0)
		sample->ambient = 0;
	for (i = 0; i < 3; i++)
		if (sample->color[i] < 0)
			sample->color[i] = 0;
}

qboolean WGPULightVol_Sample (const vec3_t point, wgpulightsample_t *sample)
{
	vec3_t	grid, direction, sample_point;
	mleaf_t	*point_leaf;
	float	ambient, weight, weight_sum;
	int	base[3], x, y, z, i;

	memset (sample, 0, sizeof(*sample));
	if (!r_nitro_lightvol.integer || !lightvol_cells || !lightvol_valid ||
	    !lightvol_used)
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
	weight_sum = 0;
	VectorClear (direction);
	VectorCopy (point, sample_point);
	point_leaf = Mod_PointInLeaf (sample_point, cl.worldmodel);
	for (z = 0; z < 2; z++)
	for (y = 0; y < 2; y++)
	for (x = 0; x < 2; x++)
	{
		const wgpulightcell_t	*cell;
		vec3_t			cell_direction, cell_point;
		int			index;

		if (!WGPULightVol_Resolve (base[0] + x, base[1] + y, base[2] + z))
			continue;
		index = WGPULightVol_Index (base[0] + x, base[1] + y, base[2] + z);
		if (lightvol_valid[index] == 2)
			continue;
		for (i = 0; i < 3; i++)
			cell_point[i] = lightvol_mins[i] +
				((i == 0 ? base[0] + x :
				  (i == 1 ? base[1] + y : base[2] + z)) + 0.5f) *
				lightvol_cellsize;
		if (point_leaf != Mod_PointInLeaf (cell_point, cl.worldmodel) &&
		    !WGPUWorld_LineVisible (point, cell_point))
			continue;
		cell = &lightvol_cells[index];
		weight = (x ? grid[0] : 1.0f - grid[0]) *
			 (y ? grid[1] : 1.0f - grid[1]) *
			 (z ? grid[2] : 1.0f - grid[2]);
		weight_sum += weight;
		ambient += cell->ambient * weight;
		for (i = 0; i < 3; i++)
			cell_direction[i] = cell->direction[i] * (2.0f / 255.0f) - 1.0f;
		VectorMA (direction, weight * cell->ambient, cell_direction, direction);
		if (!lightvol_used[index] &&
		    lightvol_used_count < NITRO_LIGHTVOL_USED_MAX)
		{
			lightvol_used[index] = 1;
			lightvol_used_indices[lightvol_used_count++] = index;
		}
	}

	if (weight_sum <= 0.001f)
		return false;
	sample->ambient = ambient / weight_sum;
	sample->color[0] = sample->ambient;
	sample->color[1] = sample->ambient;
	sample->color[2] = sample->ambient;
	VectorScale (direction, 1.0f / weight_sum, direction);
	sample->shade = VectorLength (direction);
	if (sample->shade > 0.02f)
	{
		VectorScale (direction, 1.0f / sample->shade, sample->direction);
		sample->shade = sample->ambient;
	}
	else
	{
		sample->shade = 0.0f;
		VectorClear (sample->direction);
	}
	WGPULightVol_ApplyDynamic (point, sample);
	return true;
}

qboolean WGPULightVol_Active (void)
{
	return r_nitro_lightvol.integer && lightvol_cells && lightvol_valid;
}

void WGPULightVol_Stats (int *cells, int *resolved, int *cellsize)
{
	int	count = 0, i;

	for (i = 0; lightvol_valid && i < lightvol_total; i++)
		count += lightvol_valid[i] == 1 || lightvol_valid[i] == 3;
	if (cells)
		*cells = lightvol_total;
	if (resolved)
		*resolved = count;
	if (cellsize)
		*cellsize = lightvol_cellsize;
}
