/*
 * wgpu_world.c -- the static world and the brush entities for WebGlideNitro.
 *
 * This is where "real polygons" actually happens.  At load time the whole
 * map becomes one immutable vertex buffer and one lightmap texture array;
 * per frame the only thing that moves is a list of 32-bit indices and a
 * handful of batch descriptors, and the GPU sees exactly one drawIndexed
 * per visible texture.
 *
 * Brush entities -- doors, platforms, breakables, the moving water -- ride
 * that same immutable buffer.  Every BSP submodel shares the world's
 * surface, vertex and edge tables, so a door needs no geometry of its own:
 * it is a surface walk, a copy of prebuilt indices, and a 256-byte uniform
 * block bound with a dynamic offset.  No per-entity buffer is created, and
 * nothing is re-uploaded when a door opens.
 *
 * The scene preparation -- PVS marking, the BSP walk, the lightmap atlas
 * packing, the texture coordinate maths -- follows gl2_world.c, because
 * those are facts about Hexen II's BSP format rather than facts about
 * OpenGL.  Nothing below translates a GL call.
 *
 * Shading is the software renderer's, not GL's: the diffuse texture stays
 * an 8-bit index, the lightmap picks an authored colormap row, and the
 * palette is applied last.  See the WGSL in engine/web/webgpu_nitro.js.
 *
 * What is still not drawn is listed in WGPUWorld_ReportGaps() and printed
 * at every map load.
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

/* position(3) + diffuse uv(2) + lightmap s,t,layer(3) == WORLD_STRIDE / 4 */
#define NITRO_VERTEX_FLOATS	8

/* The slack a brush entity's face has to be on the wrong side of the eye
 * before it is dropped; the same value gl_rsurf.c has always used. */
#define NITRO_BACKFACE_EPSILON	0.01f

typedef struct
{
	int	firstvert;
	int	numverts;
	int	firstindex;
	int	numindices;
	int	texindex;	/* index into the map's texture table, or -1 */
	int	lightmap;	/* atlas layer, or -1 when the surface is unlit */
	int	chain;		/* next surface in this frame's texture chain */
	short	light_s, light_t;
	short	smax, tmax;
} nitrosurf_t;

static nitrosurf_t	*nitro_surfaces;
static int		nitro_numsurfaces;

/* One texture id per entry of the map's texture table, plus the chain heads
 * the BSP walk pushes onto. */
static int	*nitro_texture_ids;
static int	*nitro_texchain;
static int	*nitro_animcache;	/* [frame][texture], -1 = not resolved yet */
static int	nitro_numtextures;

static unsigned int	*nitro_surf_indices;	/* built once, never changes */
static unsigned int	*nitro_frame_indices;	/* this frame's visible subset */
static int		nitro_frame_capacity;
static int		nitro_frame_cursor;
static wgpubatch_t	*nitro_batches;
static int		nitro_batch_capacity;
static int		nitro_batch_count;
static int		nitro_opaque_batches;
static int		nitro_total_indices;

/* The per-entity uniform arena.  Block 0 is the world's own: an identity
 * transform, opaque, lightmapped.  Brush entities append to it. */
static wgpuentity_t	*nitro_entities;
static int		nitro_entity_capacity;
static int		nitro_entity_count;

static byte	*nitro_lightmap_data[NITRO_MAX_LIGHTMAPS];
static short	nitro_lightmap_allocated[NITRO_MAX_LIGHTMAPS][NITRO_LIGHTMAP_SIZE];
static int	nitro_numlightmaps;

static qboolean	nitro_world_ready;
static qboolean	nitro_reported;

/*
=============================================================================

	lightmap atlas

	One byte per texel.  An RGBA atlas would keep RGB for coloured light and
	put the neutral intensity in alpha; Nitro only ever consumes that
	intensity, because the colormap row it selects is what makes the result
	look like the software renderer instead of like GLQuake.  So the atlas is
	r8unorm and a quarter the size.

=============================================================================
*/

static int WGPUWorld_AllocLightmapBlock (int w, int h, short *x, short *y)
{
	int	layer, i, j, best, best2;

	if (w > NITRO_LIGHTMAP_SIZE || h > NITRO_LIGHTMAP_SIZE)
		return -1;

	for (layer = 0; layer < NITRO_MAX_LIGHTMAPS; layer++)
	{
		best = NITRO_LIGHTMAP_SIZE + 1;

		for (i = 0; i <= NITRO_LIGHTMAP_SIZE - w; i++)
		{
			best2 = 0;

			for (j = 0; j < w; j++)
			{
				if (nitro_lightmap_allocated[layer][i + j] >= best)
					break;
				if (nitro_lightmap_allocated[layer][i + j] > best2)
					best2 = nitro_lightmap_allocated[layer][i + j];
			}
			if (j == w)
			{
				*x = (short)i;
				*y = (short)best2;
				best = best2;
			}
		}

		if (best > NITRO_LIGHTMAP_SIZE || best + h > NITRO_LIGHTMAP_SIZE)
			continue;

		if (!nitro_lightmap_data[layer])
		{
			nitro_lightmap_data[layer] = (byte *)
				calloc (NITRO_LIGHTMAP_SIZE * NITRO_LIGHTMAP_SIZE, 1);
			if (!nitro_lightmap_data[layer])
				return -1;
		}

		for (i = 0; i < w; i++)
			nitro_lightmap_allocated[layer][*x + i] = (short)(best + h);

		if (layer >= nitro_numlightmaps)
			nitro_numlightmaps = layer + 1;
		return layer;
	}
	return -1;
}

/*
================
WGPUWorld_BuildLightmapBlock

r_surf.c's arithmetic, once, at load time.  Values accumulate in 8.8 fixed
point exactly as the software renderer's blocklights do, then collapse to
the one byte the shader turns back into a colormap row.

Light styles are frozen at the value R_NewMap seeds d_lightstylevalue with,
and dynamic lights are not accumulated at all: the slice has no per-frame
lightmap upload path.  Both gaps are reported at map load.
================
*/
static void WGPUWorld_BuildLightmapBlock (const msurface_t *surf, const nitrosurf_t *info)
{
	int		smax = info->smax;
	int		tmax = info->tmax;
	int		size = smax * tmax;
	int		i, s, maps;
	const byte	*samples = surf->samples;
	byte		*dest;
	static int	blocklights[NITRO_LIGHTMAP_SIZE * NITRO_LIGHTMAP_SIZE];

	if (info->lightmap < 0 || !nitro_lightmap_data[info->lightmap])
		return;
	if (size <= 0 || size > NITRO_LIGHTMAP_SIZE * NITRO_LIGHTMAP_SIZE)
		return;

	if (r_fullbright.integer || !cl.worldmodel->lightdata || !samples)
	{
		for (i = 0; i < size; i++)
			blocklights[i] = 255 * 256;
	}
	else
	{
		/* Seed with r_ambient before accumulating styles, the same order
		 * as r_surf.c:202.  One unit of r_ambient is 256 here. */
		int	ambient = r_ambient.integer * 256;

		if (ambient < 0)
			ambient = 0;
		for (i = 0; i < size; i++)
			blocklights[i] = ambient;

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			int	scale = d_lightstylevalue[surf->styles[maps]];

			for (i = 0; i < size; i++)
				blocklights[i] += samples[i] * scale;
			samples += size;
		}
	}

	dest = nitro_lightmap_data[info->lightmap] +
		(size_t)info->light_t * NITRO_LIGHTMAP_SIZE + info->light_s;

	for (i = 0; i < tmax; i++)
	{
		byte	*row = dest + (size_t)i * NITRO_LIGHTMAP_SIZE;

		for (s = 0; s < smax; s++)
		{
			int	value = blocklights[i * smax + s] >> 8;

			if (value < 0)
				value = 0;
			else if (value > 255)
				value = 255;
			row[s] = (byte)value;
		}
	}
}

/*
=============================================================================

	textures

=============================================================================
*/

/*
================
WGPUWorld_LoadSkyTexture

A sky texture is 256x128: the left half is the cloud overlay with palette
index 0 punched out, the right half the solid background (r_sky.c).  The
slice draws the solid half only, unscrolled -- see WGPUWorld_ReportGaps.
================
*/
static int WGPUWorld_LoadSkyTexture (const texture_t *texture)
{
	const byte	*src = (const byte *)texture + texture->offsets[0];
	int		width = (int)texture->width;
	int		height = (int)texture->height;
	int		half = width / 2;
	byte		*solid;
	int		id, y;

	if (half < 1 || height < 1)
		return 0;

	solid = (byte *) malloc ((size_t)half * height);
	if (!solid)
		return 0;
	for (y = 0; y < height; y++)
		memcpy (solid + (size_t)y * half, src + (size_t)y * width + half, half);

	id = Nitro_CreateTexture (texture->name, half, height, solid, NITROTEX_WRAP);
	free (solid);
	return id;
}

static void WGPUWorld_LoadTextures (qmodel_t *world)
{
	int	i;

	nitro_numtextures = world->numtextures;
	nitro_texture_ids = (int *) calloc ((size_t)q_max(nitro_numtextures, 1), sizeof(int));
	nitro_texchain = (int *) malloc ((size_t)q_max(nitro_numtextures, 1) * sizeof(int));
	nitro_animcache = (int *) malloc ((size_t)q_max(nitro_numtextures, 1) * 2 * sizeof(int));
	if (!nitro_texture_ids || !nitro_texchain || !nitro_animcache)
		Sys_Error ("WebGlideNitro: out of memory for world textures");

	for (i = 0; i < nitro_numtextures; i++)
	{
		texture_t	*texture = world->textures[i];
		unsigned int	flags = NITROTEX_WRAP;

		nitro_texchain[i] = -1;
		if (!texture || !texture->offsets[0])
			continue;

		if (!strncmp (texture->name, "sky", 3))
		{
			nitro_texture_ids[i] = WGPUWorld_LoadSkyTexture (texture);
			continue;
		}
		if (texture->name[0] == '{')
			flags |= NITROTEX_HOLEY;

		nitro_texture_ids[i] = Nitro_CreateTexture (texture->name,
				(int)texture->width, (int)texture->height,
				(const byte *)texture + texture->offsets[0], flags);
	}
}

/*
=============================================================================

	geometry

=============================================================================
*/

static qboolean WGPUWorld_SurfaceIsLit (const msurface_t *surf)
{
	return !(surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED));
}

static int WGPUWorld_TextureIndex (const qmodel_t *world, const msurface_t *surf)
{
	int	i;

	if (!surf->texinfo || !surf->texinfo->texture)
		return -1;
	for (i = 0; i < world->numtextures; i++)
	{
		if (world->textures[i] == surf->texinfo->texture)
			return i;
	}
	return -1;
}

/*
================
WGPUWorld_BuildBuffers

One pass over every surface in the map: allocate its lightmap rectangle,
emit its vertices, and emit the triangle-fan indices that never change.
The result is handed to Nitro_CreateWorld, which writes it into a buffer
that is never given COPY_DST -- immutable for the lifetime of the map.
================
*/
static void WGPUWorld_BuildBuffers (qmodel_t *world)
{
	int	surfnum, totalverts = 0, totalindices = 0;
	int	vertexcursor = 0, indexcursor = 0;
	float	*vertices;

	for (surfnum = 0; surfnum < world->numsurfaces; surfnum++)
	{
		int	numverts = world->surfaces[surfnum].numedges;

		if (numverts < 3)
			continue;
		totalverts += numverts;
		totalindices += (numverts - 2) * 3;
	}

	if (!totalverts || !totalindices)
		return;

	vertices = (float *) malloc ((size_t)totalverts * NITRO_VERTEX_FLOATS * sizeof(float));
	nitro_surf_indices = (unsigned int *) malloc ((size_t)totalindices * sizeof(unsigned int));
	nitro_frame_indices = (unsigned int *) malloc ((size_t)totalindices * sizeof(unsigned int));
	if (!vertices || !nitro_surf_indices || !nitro_frame_indices)
		Sys_Error ("WebGlideNitro: out of memory for world geometry");
	nitro_total_indices = totalindices;
	nitro_frame_capacity = totalindices;

	for (surfnum = 0; surfnum < world->numsurfaces; surfnum++)
	{
		msurface_t		*surf = &world->surfaces[surfnum];
		nitrosurf_t		*info = &nitro_surfaces[surfnum];
		const mtexinfo_t	*tex = surf->texinfo;
		int			i, numverts = surf->numedges;

		info->lightmap = -1;
		info->chain = -1;
		info->texindex = WGPUWorld_TextureIndex (world, surf);
		if (numverts < 3)
			continue;

		if (WGPUWorld_SurfaceIsLit (surf))
		{
			info->smax = (short)((surf->extents[0] >> 4) + 1);
			info->tmax = (short)((surf->extents[1] >> 4) + 1);
			info->lightmap = WGPUWorld_AllocLightmapBlock (info->smax, info->tmax,
						&info->light_s, &info->light_t);
		}

		info->firstvert = vertexcursor;
		info->numverts = numverts;
		info->firstindex = indexcursor;
		info->numindices = (numverts - 2) * 3;

		for (i = 0; i < numverts; i++)
		{
			int		lindex = world->surfedges[surf->firstedge + i];
			float		*out = &vertices[(size_t)(vertexcursor + i) * NITRO_VERTEX_FLOATS];
			const float	*position;
			float		s, t;

			if (lindex > 0)
				position = world->vertexes[world->edges[lindex].v[0]].position;
			else
				position = world->vertexes[world->edges[-lindex].v[1]].position;

			out[0] = position[0];
			out[1] = position[1];
			out[2] = position[2];

			s = DotProduct (position, tex->vecs[0]) + tex->vecs[0][3];
			t = DotProduct (position, tex->vecs[1]) + tex->vecs[1][3];

			if (surf->flags & SURF_DRAWSKY)
			{
				/* The solid half is 128 wide, not 256. */
				out[3] = s / 128.0f;
				out[4] = t / 128.0f;
			}
			else if (tex->texture && tex->texture->width && tex->texture->height)
			{
				out[3] = s / (float)tex->texture->width;
				out[4] = t / (float)tex->texture->height;
			}
			else
			{
				out[3] = out[4] = 0.0f;
			}

			if (info->lightmap >= 0)
			{
				float	ls = s - surf->texturemins[0];
				float	lt = t - surf->texturemins[1];

				ls += info->light_s * 16 + 8;
				lt += info->light_t * 16 + 8;
				out[5] = ls / (NITRO_LIGHTMAP_SIZE * 16);
				out[6] = lt / (NITRO_LIGHTMAP_SIZE * 16);
				out[7] = (float)info->lightmap;
			}
			else
			{
				out[5] = out[6] = 0.0f;
				out[7] = -1.0f;
			}
		}

		for (i = 0; i < numverts - 2; i++)
		{
			nitro_surf_indices[indexcursor + i * 3 + 0] = (unsigned int)vertexcursor;
			nitro_surf_indices[indexcursor + i * 3 + 1] = (unsigned int)(vertexcursor + i + 1);
			nitro_surf_indices[indexcursor + i * 3 + 2] = (unsigned int)(vertexcursor + i + 2);
		}

		if (info->lightmap >= 0)
			WGPUWorld_BuildLightmapBlock (surf, info);

		vertexcursor += numverts;
		indexcursor += info->numindices;
	}

	if (!Nitro_CreateWorld (vertices, totalverts, totalindices,
				nitro_numlightmaps, NITRO_LIGHTMAP_SIZE))
	{
		free (vertices);
		return;
	}
	WebPerf_CountUpload ((size_t)totalverts * NITRO_VERTEX_FLOATS * sizeof(float));

	for (surfnum = 0; surfnum < nitro_numlightmaps; surfnum++)
	{
		if (!nitro_lightmap_data[surfnum])
			continue;
		Nitro_UploadLightmap (surfnum, nitro_lightmap_data[surfnum]);
		WebPerf_CountUpload (NITRO_LIGHTMAP_SIZE * NITRO_LIGHTMAP_SIZE);
	}

	free (vertices);
	nitro_world_ready = true;
}

/*
=============================================================================

	visibility

=============================================================================
*/

static void WGPUWorld_MarkLeaves (void)
{
	static mleaf_t	*oldviewleaf;
	static int	oldnovis = -1;
	byte		*vis;
	mnode_t		*node;
	int		i;

	if (wgpu_viewleaf == oldviewleaf && r_novis.integer == oldnovis)
		return;
	oldviewleaf = wgpu_viewleaf;
	oldnovis = r_novis.integer;
	wgpu_visframecount++;

	if (r_novis.integer || !wgpu_viewleaf || !cl.worldmodel->visdata)
	{
		for (i = 0; i < cl.worldmodel->numleafs; i++)
		{
			node = (mnode_t *)&cl.worldmodel->leafs[i + 1];
			do
			{
				if (node->visframe == wgpu_visframecount)
					break;
				node->visframe = wgpu_visframecount;
				node = node->parent;
			} while (node);
		}
		return;
	}

	vis = Mod_LeafPVS (wgpu_viewleaf, cl.worldmodel);
	for (i = 0; i < cl.worldmodel->numleafs; i++)
	{
		if (!(vis[i >> 3] & (1 << (i & 7))))
			continue;
		node = (mnode_t *)&cl.worldmodel->leafs[i + 1];
		do
		{
			if (node->visframe == wgpu_visframecount)
				break;
			node->visframe = wgpu_visframecount;
			node = node->parent;
		} while (node);
	}
}

static void WGPUWorld_ResetChains (void)
{
	int	i;

	for (i = 0; i < nitro_numtextures; i++)
		nitro_texchain[i] = -1;
}

/*
 * Everything chains by texture, including sky and liquids.  Those two are
 * approximations in this slice rather than separate passes (see
 * WGPUWorld_ReportGaps), and chaining them with everything else is what
 * keeps the frame at one draw call per visible texture.
 */
static void WGPUWorld_ChainSurface (int surfnum)
{
	nitrosurf_t	*info = &nitro_surfaces[surfnum];

	if (!info->numindices || info->texindex < 0)
		return;

	info->chain = nitro_texchain[info->texindex];
	nitro_texchain[info->texindex] = surfnum;
}

static void WGPUWorld_RecursiveWorldNode (mnode_t *node)
{
	int		c, side, surfnum;
	mplane_t	*plane;
	msurface_t	*surf;
	float		dot;

	while (1)
	{
		if (node->contents == CONTENTS_SOLID)
			return;
		if (node->visframe != wgpu_visframecount)
			return;
		if (WGPU_CullBox (node->minmaxs, node->minmaxs + 3))
			return;

		if (node->contents < 0)
		{
			mleaf_t		*leaf = (mleaf_t *)node;
			msurface_t	**mark = leaf->firstmarksurface;

			c = leaf->nummarksurfaces;
			while (c--)
			{
				(*mark)->visframe = wgpu_visframecount;
				mark++;
			}
			if (leaf->efrags)
				R_StoreEfrags (&leaf->efrags);
			return;
		}

		plane = node->plane;
		switch (plane->type)
		{
		case PLANE_X:
			dot = wgpu_modelorg[0] - plane->dist;
			break;
		case PLANE_Y:
			dot = wgpu_modelorg[1] - plane->dist;
			break;
		case PLANE_Z:
			dot = wgpu_modelorg[2] - plane->dist;
			break;
		default:
			dot = DotProduct (wgpu_modelorg, plane->normal) - plane->dist;
			break;
		}
		side = (dot >= 0) ? 0 : 1;

		WGPUWorld_RecursiveWorldNode (node->children[side]);

		surfnum = node->firstsurface;
		surf = cl.worldmodel->surfaces + surfnum;
		for (c = node->numsurfaces; c; c--, surf++, surfnum++)
		{
			if (surf->visframe != wgpu_visframecount)
				continue;
			if (((surf->flags & SURF_PLANEBACK) != 0) != (side != 0))
				continue;
			WGPUWorld_ChainSurface (surfnum);
		}

		node = node->children[!side];
	}
}

/*
=============================================================================

	drawing

	Everything the scene pass draws out of the world's vertex buffer --
	the world itself and every brush entity -- accumulates into three
	per-frame arenas: the index arena, the batch list, and the entity
	uniform arena.  They are handed to WebGPU once, at the end of the
	frame, by WGPUWorld_SubmitScene.

=============================================================================
*/

static void WGPUWorld_ReserveIndices (int count)
{
	int		capacity = nitro_frame_capacity;
	unsigned int	*grown;

	if (nitro_frame_cursor + count <= capacity)
		return;

	while (capacity < nitro_frame_cursor + count)
		capacity = capacity ? capacity * 2 : 4096;
	grown = (unsigned int *) realloc (nitro_frame_indices,
			(size_t)capacity * sizeof(unsigned int));
	if (!grown)
		Sys_Error ("WebGlideNitro: out of memory for %d indices", capacity);
	nitro_frame_indices = grown;
	nitro_frame_capacity = capacity;
}

static wgpubatch_t *WGPUWorld_NewBatch (void)
{
	if (nitro_batch_count == nitro_batch_capacity)
	{
		int		capacity = nitro_batch_capacity ? nitro_batch_capacity * 2 : 256;
		wgpubatch_t	*grown = (wgpubatch_t *) realloc (nitro_batches,
					(size_t)capacity * sizeof(wgpubatch_t));

		if (!grown)
			Sys_Error ("WebGlideNitro: out of memory for %d batches", capacity);
		nitro_batches = grown;
		nitro_batch_capacity = capacity;
	}
	return &nitro_batches[nitro_batch_count++];
}

/*
================
WGPUWorld_NewEntityBlock

Appends a 256-byte uniform block and returns its index.  The alignment is
WebGPU's minimum dynamic uniform offset, so the arena can be written with
one writeBuffer and every block addressed by offset alone.
================
*/
static int WGPUWorld_NewEntityBlock (const wgpumatrix_t *mvp, float alpha, float light)
{
	wgpuentity_t	*block;

	if (nitro_entity_count == nitro_entity_capacity)
	{
		int		capacity = nitro_entity_capacity ? nitro_entity_capacity * 2 : 64;
		wgpuentity_t	*grown = (wgpuentity_t *) realloc (nitro_entities,
					(size_t)capacity * sizeof(wgpuentity_t));

		if (!grown)
			Sys_Error ("WebGlideNitro: out of memory for %d entities", capacity);
		nitro_entities = grown;
		nitro_entity_capacity = capacity;
	}

	block = &nitro_entities[nitro_entity_count];
	memset (block, 0, sizeof(*block));
	memcpy (block->mvp, mvp->m, sizeof(block->mvp));
	block->alpha = alpha;
	block->light = light;
	return nitro_entity_count++;
}

/*
================
WGPUWorld_GatherChain

Copies one texture chain's prebuilt indices into the frame's index arena
and appends a single batch descriptor.  Everything a texture contributes to
the frame becomes one drawIndexed, however many surfaces it covers.
================
*/
static void WGPUWorld_GatherChain (int head, int textureid, int entity)
{
	int		first = nitro_frame_cursor;
	int		surfnum = head;
	wgpubatch_t	*batch;

	if (head < 0 || textureid <= 0)
		return;

	while (surfnum >= 0)
	{
		const nitrosurf_t	*info = &nitro_surfaces[surfnum];

		if (info->numindices > 0)
		{
			WGPUWorld_ReserveIndices (info->numindices);
			memcpy (nitro_frame_indices + nitro_frame_cursor,
				nitro_surf_indices + info->firstindex,
				(size_t)info->numindices * sizeof(unsigned int));
			nitro_frame_cursor += info->numindices;
			wgpu_frame_polys += info->numindices / 3;
			c_brush_polys += info->numverts - 2;
		}
		surfnum = info->chain;
	}

	if (nitro_frame_cursor == first)
		return;

	batch = WGPUWorld_NewBatch ();
	batch->texture = textureid;
	batch->first = first;
	batch->count = nitro_frame_cursor - first;
	batch->entity = entity;
	WebPerf_CountDraw (batch->count / 3);
}

/*
================
WGPUWorld_TextureAnimation

Walks the animation chain for the current time, honouring the alternate
sequence a brush entity's frame selects.  Only the texture the batch binds
changes -- the chain, and therefore the draw call count, does not.
================
*/
static int WGPUWorld_TextureAnimation (int texindex, int frame)
{
	texture_t	*base;
	int		relative, count = 0, i;
	int		slot, resolved;

	if (texindex < 0 || texindex >= nitro_numtextures)
		return texindex;
	base = cl.worldmodel->textures[texindex];
	if (!base)
		return texindex;

	/* Resolving an animated texture back to its table index is a linear
	 * search, and a map full of doors would repeat it per entity.  The
	 * answer only depends on the texture and the entity's frame parity
	 * within a frame, so memoise both. */
	slot = (frame ? nitro_numtextures : 0) + texindex;
	resolved = nitro_animcache[slot];
	if (resolved >= 0)
		return resolved;

	if (frame && base->alternate_anims)
		base = base->alternate_anims;
	if (!base->anim_total)
	{
		if (base == cl.worldmodel->textures[texindex])
			return texindex;
	}
	else
	{
		relative = (int)(cl.time * 10) % base->anim_total;
		while (base->anim_min > relative || base->anim_max <= relative)
		{
			base = base->anim_next;
			if (!base || ++count > 100)
				return texindex;
		}
	}

	for (i = 0; i < nitro_numtextures; i++)
	{
		if (cl.worldmodel->textures[i] == base)
		{
			nitro_animcache[slot] = i;
			return i;
		}
	}
	return texindex;
}

static void WGPUWorld_DrawChains (int frame, int entity)
{
	int	i;

	for (i = 0; i < nitro_numtextures; i++)
	{
		int	animated, textureid;

		if (nitro_texchain[i] < 0)
			continue;
		animated = WGPUWorld_TextureAnimation (i, frame);
		textureid = nitro_texture_ids[animated];
		if (textureid <= 0)
			textureid = nitro_texture_ids[i];

		WGPUWorld_GatherChain (nitro_texchain[i], textureid, entity);
	}
}

/*
================
WGPUWorld_BeginScene

Resets the three per-frame arenas and lays down entity block 0: the world's
own transform.  Binding it for the world too means the world and every
brush entity share one pipeline and one bind group layout.
================
*/
void WGPUWorld_BeginScene (void)
{
	int	i;

	nitro_frame_cursor = 0;
	nitro_batch_count = 0;
	nitro_opaque_batches = 0;
	nitro_entity_count = 0;

	for (i = 0; i < nitro_numtextures * 2; i++)
		nitro_animcache[i] = -1;

	WGPUWorld_NewEntityBlock (&wgpu_view_projection, 1.0f, -1.0f);
}

void WGPUWorld_DrawWorld (void)
{
	if (!nitro_world_ready || !cl.worldmodel)
		return;

	VectorCopy (r_origin, wgpu_modelorg);

	WGPUWorld_MarkLeaves ();
	WGPUWorld_ResetChains ();
	WGPUWorld_RecursiveWorldNode (cl.worldmodel->nodes);

	/* Sky and liquids ride the same chains and the same pipeline: the sky
	 * is its solid layer, unscrolled, and liquids are opaque and unwarped.
	 * Both are reported as approximations. */
	WGPUWorld_DrawChains (0, 0);
}

/*
================
WGPUWorld_DrawBrushEntity

A submodel indexes the same vertex buffer as the world, so the only extra
work is one uniform block and its own backface walk.
================
*/
void WGPUWorld_DrawBrushEntity (entity_t *entity)
{
	qmodel_t	*model = entity->model;
	wgpumatrix_t	translate, rotate, temp, local, mvp;
	vec3_t		mins, maxs;
	msurface_t	*surf;
	float		alpha = 1.0f, light = -1.0f;
	int		i, surfnum, mls, block;
	qboolean	rotated;

	if (!nitro_world_ready || !model || model->type != mod_brush)
		return;
	/* Every brush entity in a map is a submodel of the world BSP and so
	 * shares its surface table.  Anything else has no vertices here. */
	if (!nitro_surfaces || model->surfaces != cl.worldmodel->surfaces)
		return;
	if (model->nummodelsurfaces <= 0)
		return;

	if (entity->angles[0] || entity->angles[1] || entity->angles[2])
	{
		rotated = true;
		for (i = 0; i < 3; i++)
		{
			mins[i] = entity->origin[i] - model->radius;
			maxs[i] = entity->origin[i] + model->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd (entity->origin, model->mins, mins);
		VectorAdd (entity->origin, model->maxs, maxs);
	}

	if (WGPU_CullBox (mins, maxs))
		return;

	VectorSubtract (r_origin, entity->origin, wgpu_modelorg);
	if (rotated)
	{
		vec3_t	forward, right, up, offset;

		VectorCopy (wgpu_modelorg, offset);
		AngleVectors (entity->angles, forward, right, up);
		wgpu_modelorg[0] = DotProduct (offset, forward);
		wgpu_modelorg[1] = -DotProduct (offset, right);
		wgpu_modelorg[2] = DotProduct (offset, up);
	}

	if (entity->drawflags & DRF_TRANSLUCENT)
		alpha = r_wateralpha.value;
	else if (entity->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(entity->alpha))
		alpha = ENTALPHA_DECODE(entity->alpha);
	alpha = CLAMP(0.0f, alpha, 1.0f);

	/* Hexen II's model light styles: an absolute light value, or the
	 * pre-baked style the flag names.  Surfaces lit this way have no
	 * lightmap samples to look up -- that is what turns bit pillars and
	 * lifts pure black -- so they select a flat colormap row instead. */
	mls = entity->drawflags & MLS_MASKIN;
	if (mls == MLS_ABSLIGHT)
		light = (float)entity->abslight / 255.0f;
	else if (mls != MLS_NONE)
		light = (float)d_lightstylevalue[24 + mls] / 255.0f;
	if (light >= 0.0f)
		light = CLAMP(0.0f, light, 1.0f);

	/* GLQuake's order: translate, yaw about Z, -pitch about Y, roll
	 * about X.  The view projection is applied on the CPU so the shader
	 * has one matrix to multiply by, not two. */
	WGPU_MatrixTranslate (&translate, entity->origin[0], entity->origin[1],
				entity->origin[2]);
	WGPU_MatrixRotate (&rotate, entity->angles[1], 0, 0, 1);
	WGPU_MatrixMultiply (&temp, &translate, &rotate);
	WGPU_MatrixRotate (&rotate, -entity->angles[0], 0, 1, 0);
	WGPU_MatrixMultiply (&local, &temp, &rotate);
	WGPU_MatrixRotate (&rotate, entity->angles[2], 1, 0, 0);
	WGPU_MatrixMultiply (&temp, &local, &rotate);
	WGPU_MatrixMultiply (&mvp, &wgpu_view_projection, &temp);

	block = WGPUWorld_NewEntityBlock (&mvp, alpha, light);

	WGPUWorld_ResetChains ();

	surfnum = model->firstmodelsurface;
	surf = &model->surfaces[surfnum];
	for (i = 0; i < model->nummodelsurfaces; i++, surf++, surfnum++)
	{
		mplane_t	*plane = surf->plane;
		float		dot;

		if (surfnum < 0 || surfnum >= nitro_numsurfaces || !plane)
			continue;
		dot = DotProduct (wgpu_modelorg, plane->normal) - plane->dist;
		if (((surf->flags & SURF_PLANEBACK) && dot < -NITRO_BACKFACE_EPSILON) ||
		    (!(surf->flags & SURF_PLANEBACK) && dot > NITRO_BACKFACE_EPSILON))
			WGPUWorld_ChainSurface (surfnum);
	}

	WGPUWorld_DrawChains (entity->frame, block);
}

/*
================
WGPUWorld_EndOpaque

Marks where the batch list stops being opaque.  Everything appended after
this point is blended, in visedict order.
================
*/
void WGPUWorld_EndOpaque (void)
{
	nitro_opaque_batches = nitro_batch_count;
}

/*
================
WGPUWorld_SubmitScene

The one call that hands a frame's whole scene to WebGPU: world and brush
entity geometry, the entity uniform arena, the model vertex arena, and the
particles.  Nothing else in the frame talks to the backend.
================
*/
void WGPUWorld_SubmitScene (wgpuscene_t *scene)
{
	wgpuscenedata_t	data;

	memset (&data, 0, sizeof(data));

	data.indices = nitro_frame_indices;
	data.indexcount = nitro_frame_cursor;
	data.batches = nitro_batches;
	data.batchcount = nitro_batch_count;
	data.opaquebatches = nitro_opaque_batches;
	data.entities = nitro_entities;
	data.entitycount = nitro_entity_count;
	data.particles = wgpu_particles;
	data.particlecount = wgpu_particle_count;
	WGPUEntity_SceneData (&data);

	wgpu_frame_batches = nitro_batch_count + data.modelbatchcount;
	Nitro_DrawScene (scene, &data);

	if (nitro_frame_cursor > 0)
		WebPerf_CountUpload ((size_t)nitro_frame_cursor * sizeof(unsigned int));
	if (nitro_entity_count > 0)
		WebPerf_CountUpload ((size_t)nitro_entity_count * sizeof(wgpuentity_t));
}

/*
================
WGPUWorld_LightPoint

r_light.c's RecursiveLightPoint, kept here because the software renderer's
copy is not compiled into this build.  It returns the same scalar intensity
the software renderer feeds into a colormap row, so a model standing in a
dark corner is exactly as dark as it is under the rasteriser.

Light styles are frozen at the value R_NewMap seeds d_lightstylevalue with,
so the sum below is static for a given map -- which is also why a flickering
torch does not make the monster beside it flicker.  That gap is reported.
================
*/
static int WGPUWorld_RecursiveLightPoint (mnode_t *node, const vec3_t start, const vec3_t end)
{
	float		front, back, frac;
	int		side, i, r, maps;
	mplane_t	*plane;
	vec3_t		mid;
	msurface_t	*surf;

loc0:
	if (!node || node->contents < 0)
		return -1;

	plane = node->plane;
	if (plane->type < 3)
	{
		front = start[plane->type] - plane->dist;
		back = end[plane->type] - plane->dist;
	}
	else
	{
		front = DotProduct (start, plane->normal) - plane->dist;
		back = DotProduct (end, plane->normal) - plane->dist;
	}
	side = front < 0;

	if ((back < 0) == side)
	{
		node = node->children[side];
		goto loc0;
	}

	frac = front / (front - back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	r = WGPUWorld_RecursiveLightPoint (node->children[side], start, mid);
	if (r >= 0)
		return r;

	if ((back < 0) == side)
		return -1;

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < (int)node->numsurfaces; i++, surf++)
	{
		const mtexinfo_t	*tex;
		const byte		*lightmap;
		int			s, t, ds, dt;

		if (surf->flags & SURF_DRAWTILED)
			continue;

		tex = surf->texinfo;
		s = (int)(DotProductDBL(mid, tex->vecs[0]) + (double)tex->vecs[0][3]);
		t = (int)(DotProductDBL(mid, tex->vecs[1]) + (double)tex->vecs[1][3]);

		if (s < surf->texturemins[0] || t < surf->texturemins[1])
			continue;
		ds = s - surf->texturemins[0];
		dt = t - surf->texturemins[1];
		if (ds > surf->extents[0] || dt > surf->extents[1])
			continue;

		if (!surf->samples)
			return 0;

		ds >>= 4;
		dt >>= 4;
		lightmap = surf->samples + dt * ((surf->extents[0] >> 4) + 1) + ds;
		r = 0;
		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			r += *lightmap * d_lightstylevalue[surf->styles[maps]];
			lightmap += ((surf->extents[0] >> 4) + 1) *
					((surf->extents[1] >> 4) + 1);
		}
		return r >> 8;
	}

	node = node->children[!side];
	VectorCopy (start, mid);
	goto loc0;
}

int WGPUWorld_LightPoint (const vec3_t point)
{
	vec3_t	start, end;
	int	r;

	if (!cl.worldmodel || !cl.worldmodel->lightdata)
		return 255;

	VectorCopy (point, start);
	end[0] = point[0];
	end[1] = point[1];
	end[2] = point[2] - 2048;

	r = WGPUWorld_RecursiveLightPoint (cl.worldmodel->nodes, start, end);
	if (r < 0)
		r = 0;
	if (r < (int)r_ambient.value)
		r = (int)r_ambient.value;
	return r;
}

qboolean WGPUWorld_Ready (void)
{
	return nitro_world_ready;
}

/*
=============================================================================

	lifecycle

=============================================================================
*/

void WGPUWorld_Shutdown (void)
{
	int	i;

	Nitro_DestroyWorld ();

	free (nitro_surfaces);
	free (nitro_texture_ids);
	free (nitro_texchain);
	free (nitro_animcache);
	free (nitro_surf_indices);
	free (nitro_frame_indices);
	free (nitro_batches);
	free (nitro_entities);
	nitro_surfaces = NULL;
	nitro_texture_ids = NULL;
	nitro_texchain = NULL;
	nitro_animcache = NULL;
	nitro_surf_indices = NULL;
	nitro_frame_indices = NULL;
	nitro_batches = NULL;
	nitro_entities = NULL;

	for (i = 0; i < NITRO_MAX_LIGHTMAPS; i++)
	{
		free (nitro_lightmap_data[i]);
		nitro_lightmap_data[i] = NULL;
	}
	memset (nitro_lightmap_allocated, 0, sizeof(nitro_lightmap_allocated));

	nitro_numsurfaces = 0;
	nitro_numtextures = 0;
	nitro_numlightmaps = 0;
	nitro_total_indices = 0;
	nitro_frame_capacity = 0;
	nitro_frame_cursor = 0;
	nitro_batch_capacity = 0;
	nitro_batch_count = 0;
	nitro_opaque_batches = 0;
	nitro_entity_capacity = 0;
	nitro_entity_count = 0;
	nitro_world_ready = false;
}

void WGPUWorld_NewMap (void)
{
	qmodel_t	*world = cl.worldmodel;

	WGPUWorld_Shutdown ();
	nitro_reported = false;

	if (!world || !world->numsurfaces)
		return;

	nitro_numsurfaces = world->numsurfaces;
	nitro_surfaces = (nitrosurf_t *) calloc ((size_t)nitro_numsurfaces, sizeof(nitrosurf_t));
	if (!nitro_surfaces)
		Sys_Error ("WebGlideNitro: out of memory for %d surfaces", nitro_numsurfaces);

	WGPUWorld_LoadTextures (world);
	WGPUWorld_BuildBuffers (world);
}

/*
================
WGPUWorld_ReportGaps

The renderer still has deliberate content gaps.  They are stated out loud
once per map rather than left for the player to discover by walking into an
invisible monster.  Silence it with r_nitro_report 0.
================
*/
void WGPUWorld_ReportGaps (void)
{
	if (nitro_reported || !r_nitro_report.integer)
		return;
	nitro_reported = true;

	Con_Printf ("WebGlideNitro: world, entities and particles (%d surfaces, %d lightmap pages)\n",
			nitro_numsurfaces, nitro_numlightmaps);
	Con_Printf ("  not drawn: shadows, model glows and fullbright skin pixels\n");
	Con_Printf ("  not applied: animated light styles, world dynamic lights, fog\n");
	Con_Printf ("  approximated: model frames step rather than interpolate\n");
	Con_Printf ("  approximated: sky is its solid layer unscrolled; liquids are opaque and unwarped\n");
}
