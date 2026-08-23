/*
 * wgpu_world.c -- the static world for WebGlideNitro.
 *
 * This is where "real polygons" actually happens.  At load time the whole
 * map becomes one immutable vertex buffer and one lightmap texture array;
 * per frame the only thing that moves is a list of 32-bit indices and a
 * handful of batch descriptors, and the GPU sees exactly one drawIndexed
 * per visible texture.
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
 * What the first slice does NOT draw is listed in WGPUWorld_ReportGaps()
 * and printed at every map load.
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
static int	nitro_numtextures;

static unsigned int	*nitro_surf_indices;	/* built once, never changes */
static unsigned int	*nitro_frame_indices;	/* this frame's visible subset */
static wgpubatch_t	*nitro_batches;
static int		nitro_total_indices;

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
	if (!nitro_texture_ids || !nitro_texchain)
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
	nitro_batches = (wgpubatch_t *) malloc ((size_t)q_max(world->numtextures + 2, 2) *
				sizeof(wgpubatch_t));
	if (!vertices || !nitro_surf_indices || !nitro_frame_indices || !nitro_batches)
		Sys_Error ("WebGlideNitro: out of memory for world geometry");
	nitro_total_indices = totalindices;

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

=============================================================================
*/

/*
================
WGPUWorld_GatherChain

Copies one texture chain's prebuilt indices into the frame's index arena
and appends a single batch descriptor.  Everything a texture contributes to
the frame becomes one drawIndexed, however many surfaces it covers.
================
*/
static int WGPUWorld_GatherChain (int head, int textureid, int *cursor, int batchcount)
{
	int	first = *cursor;
	int	surfnum = head;

	if (head < 0 || textureid <= 0)
		return batchcount;

	while (surfnum >= 0)
	{
		const nitrosurf_t	*info = &nitro_surfaces[surfnum];

		if (info->numindices > 0 && *cursor + info->numindices <= nitro_total_indices)
		{
			memcpy (nitro_frame_indices + *cursor,
				nitro_surf_indices + info->firstindex,
				(size_t)info->numindices * sizeof(unsigned int));
			*cursor += info->numindices;
			wgpu_frame_polys += info->numindices / 3;
			c_brush_polys += info->numverts - 2;
		}
		surfnum = info->chain;
	}

	if (*cursor == first)
		return batchcount;

	nitro_batches[batchcount].texture = textureid;
	nitro_batches[batchcount].first = first;
	nitro_batches[batchcount].count = *cursor - first;
	nitro_batches[batchcount].reserved = 0;
	WebPerf_CountDraw ((*cursor - first) / 3);
	return batchcount + 1;
}

/*
================
WGPUWorld_TextureAnimation

Walks the animation chain for the current time.  Only the texture the batch
binds changes -- the chain, and therefore the draw call count, does not.
The world has no entity frames, so the alternate sequence never applies.
================
*/
static int WGPUWorld_TextureAnimation (int texindex)
{
	texture_t	*base;
	int		relative, count = 0, i;

	if (texindex < 0 || texindex >= nitro_numtextures)
		return texindex;
	base = cl.worldmodel->textures[texindex];
	if (!base || !base->anim_total)
		return texindex;

	relative = (int)(cl.time * 10) % base->anim_total;
	while (base->anim_min > relative || base->anim_max <= relative)
	{
		base = base->anim_next;
		if (!base || ++count > 100)
			return texindex;
	}

	for (i = 0; i < nitro_numtextures; i++)
	{
		if (cl.worldmodel->textures[i] == base)
			return i;
	}
	return texindex;
}

void WGPUWorld_DrawWorld (wgpuscene_t *scene)
{
	int	i, cursor = 0, batchcount = 0;

	if (!nitro_world_ready || !cl.worldmodel)
	{
		Nitro_DrawScene (scene, NULL, 0, NULL, 0);
		return;
	}

	VectorCopy (r_origin, wgpu_modelorg);

	WGPUWorld_MarkLeaves ();
	WGPUWorld_ResetChains ();
	WGPUWorld_RecursiveWorldNode (cl.worldmodel->nodes);

	for (i = 0; i < nitro_numtextures; i++)
	{
		int	frame, textureid;

		if (nitro_texchain[i] < 0)
			continue;
		frame = WGPUWorld_TextureAnimation (i);
		textureid = nitro_texture_ids[frame];
		if (textureid <= 0)
			textureid = nitro_texture_ids[i];

		batchcount = WGPUWorld_GatherChain (nitro_texchain[i], textureid,
					&cursor, batchcount);
	}

	/* Sky and liquids ride the same chains and the same pipeline in this
	 * slice: the sky is its solid layer, unscrolled, and liquids are
	 * opaque and unwarped.  Both are reported as approximations. */

	wgpu_frame_batches = batchcount;
	Nitro_DrawScene (scene, nitro_frame_indices, cursor, nitro_batches, batchcount);
	if (cursor > 0)
		WebPerf_CountUpload ((size_t)cursor * sizeof(unsigned int));
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
	free (nitro_surf_indices);
	free (nitro_frame_indices);
	free (nitro_batches);
	nitro_surfaces = NULL;
	nitro_texture_ids = NULL;
	nitro_texchain = NULL;
	nitro_surf_indices = NULL;
	nitro_frame_indices = NULL;
	nitro_batches = NULL;

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

The first slice draws the static world and nothing else.  That is a
deliberate scope, not an accident, so it is stated out loud once per map
rather than left for the player to discover by walking into an invisible
monster.  Silence it with r_nitro_report 0.
================
*/
void WGPUWorld_ReportGaps (void)
{
	if (nitro_reported || !r_nitro_report.integer)
		return;
	nitro_reported = true;

	Con_Printf ("WebGlideNitro: static world only (%d surfaces, %d lightmap pages)\n",
			nitro_numsurfaces, nitro_numlightmaps);
	Con_Printf ("  not drawn: brush entities, alias models, sprites, particles\n");
	Con_Printf ("  not applied: dynamic lights, animated light styles, fog\n");
	Con_Printf ("  approximated: sky is its solid layer unscrolled; liquids are opaque and unwarped\n");
}
