/*
 * gl2_world.c -- WebGlide world geometry, lightmaps and sky.
 *
 * The world is the one thing WebGlide keeps on the GPU: its geometry never
 * changes, so it is built into a static vertex buffer at load time and
 * drawn as per-texture index batches.  Lightmaps live in 128x128 RGBA
 * atlases and are only re-uploaded for surfaces a dynamic light touched.
 *
 * Colour comes from two places, both of them already in the game data:
 *
 *   - .lit files, the 1997 LordHavoc coloured-lighting standard, loaded
 *     alongside the map and used in place of the BSP's greyscale samples.
 *   - cl_dlights[].color, which Hexen II fills in for torches, missiles,
 *     ice and anything the QC gave glow_settings.
 *
 * The 8bpp software renderer can show neither.  This is the payoff.
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
#include "gl2_glide.h"

#define LMBLOCK_WIDTH		128
#define LMBLOCK_HEIGHT		128
#define GL2_VERTEX_FLOATS	7	/* xyz, st, lmst */
#define GL2_BACKFACE_EPSILON	0.01f

typedef struct
{
	int	firstvert;
	int	numverts;
	int	firstindex;
	int	numindices;
	int	texindex;		/* slot in the map's texture table */
	int	chain;			/* next surface in this texture's chain */
	int	lmchain;		/* next surface in this atlas' sub-chain */
	int	lightmap;		/* atlas index, -1 when unlit */
	short	light_s, light_t;
	short	smax, tmax;
	int	cached_dlight;
	int	cached_style[MAXLIGHTMAPS];
} gl2surf_t;

static gl2surf_t	*gl2_surfaces;
static int		gl2_numsurfaces;

static GLuint		gl2_world_vbo;
static GLuint		gl2_world_ibo;
static GLuint		gl2_world_vao;

static GLuint		*gl2_surf_indices;	/* static, one run per surface */
static int		gl2_total_indices;
static GLuint		*gl2_frame_indices;	/* staging for one batch */

static int		*gl2_texchain;		/* head per world texture */
static int		gl2_numtexchains;
static gl2texture_t	**gl2_textures_by_index;
static int		gl2_skychain;
static int		gl2_waterchain;
static int		gl2_lmhead[WEB_MAX_LIGHTMAPS];

static GLuint		gl2_lightmap_textures[WEB_MAX_LIGHTMAPS];
static byte		*gl2_lightmap_data[WEB_MAX_LIGHTMAPS];
static int		gl2_lightmap_allocated[WEB_MAX_LIGHTMAPS][LMBLOCK_WIDTH];
static int		gl2_numlightmaps;

static float		gl2_blocklights[LMBLOCK_WIDTH * LMBLOCK_HEIGHT * 3];

static byte		*gl2_litdata;		/* .lit samples, or NULL */
static qboolean		gl2_lit_loaded;
static int		gl2_lightmap_shift;
static int		gl2_lightmap_overbright;

static gl2texture_t	*gl2_solidsky;
static gl2texture_t	*gl2_alphasky;

int			gl2_frame_polys;
int			gl2_frame_batches;

/*
=============================================================================

	.lit -- 1997 coloured lighting

=============================================================================
*/

/*
================
GL2_LoadLitFile

Reads maps/<name>.lit next to the map.  The format is the four magic bytes
"QLIT", a version long, then three bytes per lightmap sample in exactly the
order of the BSP's greyscale lump.
================
*/
static void GL2_LoadLitFile (qmodel_t *world)
{
	char		litname[MAX_QPATH];
	unsigned int	path_id = 0;
	byte		*data;
	int		version;

	gl2_litdata = NULL;
	gl2_lit_loaded = false;

	if (!world->lightdata || !gl_coloredlight.integer)
		return;

	COM_StripExtension (world->name, litname, sizeof(litname));
	q_strlcat (litname, ".lit", sizeof(litname));

	data = (byte *) FS_LoadHunkFile (litname, &path_id);
	if (!data)
		return;
	/* Only trust a .lit from the map's own gamedir or better, the same
	 * rule the desktop GL renderer uses. */
	if (path_id < world->path_id)
	{
		Con_DPrintf ("WebGlide: ignoring %s from a lower priority gamedir\n", litname);
		return;
	}
	if (data[0] != 'Q' || data[1] != 'L' || data[2] != 'I' || data[3] != 'T')
	{
		Con_Printf ("WebGlide: %s is not a .lit file\n", litname);
		return;
	}
	version = LittleLong (((int *)data)[1]);
	if (version != 1)
	{
		Con_Printf ("WebGlide: %s is .lit version %d, expected 1\n", litname, version);
		return;
	}

	gl2_litdata = data + 8;
	gl2_lit_loaded = true;
	Con_Printf ("WebGlide: coloured lighting from %s\n", litname);
}

/*
================
GL2_SurfaceSamples

A surface's colour samples, or NULL when there is no .lit and the caller
should fall back to the BSP's one byte per sample.
================
*/
static const byte *GL2_SurfaceSamples (const msurface_t *surf)
{
	ptrdiff_t	offset;

	if (!gl2_lit_loaded || !surf->samples || !cl.worldmodel->lightdata)
		return NULL;
	offset = surf->samples - cl.worldmodel->lightdata;
	if (offset < 0)
		return NULL;
	return gl2_litdata + offset * 3;
}

/*
=============================================================================

	lightmap atlases

=============================================================================
*/

static int GL2_AllocLightmapBlock (int w, int h, short *x, short *y)
{
	int	texnum, i, j, best, best2;

	if (w > LMBLOCK_WIDTH || h > LMBLOCK_HEIGHT)
		return -1;

	for (texnum = 0; texnum < WEB_MAX_LIGHTMAPS; texnum++)
	{
		best = LMBLOCK_HEIGHT + 1;

		for (i = 0; i <= LMBLOCK_WIDTH - w; i++)
		{
			best2 = 0;

			for (j = 0; j < w; j++)
			{
				if (gl2_lightmap_allocated[texnum][i + j] >= best)
					break;
				if (gl2_lightmap_allocated[texnum][i + j] > best2)
					best2 = gl2_lightmap_allocated[texnum][i + j];
			}
			if (j == w)
			{
				*x = (short)i;
				*y = (short)best2;
				best = best2;
			}
		}

		if (best > LMBLOCK_HEIGHT || best + h > LMBLOCK_HEIGHT)
			continue;

		if (!gl2_lightmap_data[texnum])
		{
			gl2_lightmap_data[texnum] = (byte *)
				calloc (LMBLOCK_WIDTH * LMBLOCK_HEIGHT, 4);
			if (!gl2_lightmap_data[texnum])
				return -1;
		}

		for (i = 0; i < w; i++)
			gl2_lightmap_allocated[texnum][*x + i] = best + h;

		if (texnum >= gl2_numlightmaps)
			gl2_numlightmaps = texnum + 1;
		return texnum;
	}

	Con_Printf ("WebGlide: out of lightmap atlases\n");
	return -1;
}

/*
================
GL2_AddDynamicLights

Hexen II's dynamic lights carry a colour, so this is where a torch stops
being merely brighter and starts being warmer.
================
*/
static void GL2_AddDynamicLights (const msurface_t *surf, const gl2surf_t *info)
{
	int			lnum, s, t, sd, td, i;
	float			dist, rad, minlight;
	vec3_t			impact, local;
	const mtexinfo_t	*tex = surf->texinfo;
	int			smax = info->smax;
	int			tmax = info->tmax;

	for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
	{
		const dlight_t	*light = &cl_dlights[lnum];
		float		sign, color[3];

		if (!(surf->dlightbits & (1u << lnum)))
			continue;

		rad = light->radius;
		dist = DotProduct (light->origin, surf->plane->normal) - surf->plane->dist;
		rad -= fabs (dist);
		minlight = light->minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i = 0; i < 3; i++)
			impact[i] = light->origin[i] - surf->plane->normal[i] * dist;

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];
		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];

		/* A light with no colour set is a white light, not a black one. */
		if (light->color[0] <= 0.0f && light->color[1] <= 0.0f && light->color[2] <= 0.0f)
		{
			color[0] = color[1] = color[2] = 1.0f;
		}
		else
		{
			for (i = 0; i < 3; i++)
				color[i] = light->color[i];
		}
		sign = light->dark ? -1.0f : 1.0f;

		for (t = 0; t < tmax; t++)
		{
			td = (int)(local[1] - t * 16);
			if (td < 0)
				td = -td;
			for (s = 0; s < smax; s++)
			{
				float	*bl;
				float	add;

				sd = (int)(local[0] - s * 16);
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = (float)(sd + (td >> 1));
				else
					dist = (float)(td + (sd >> 1));
				if (dist >= minlight)
					continue;

				add = (minlight - dist) * 256.0f * sign;
				bl = &gl2_blocklights[(t * smax + s) * 3];
				bl[0] += add * color[0];
				bl[1] += add * color[1];
				bl[2] += add * color[2];
			}
		}
	}
}

/*
================
GL2_BuildLightmapBlock

Accumulates every light style and dynamic light for one surface and writes
the result into its rectangle of the atlas.
================
*/
static void GL2_BuildLightmapBlock (msurface_t *surf, gl2surf_t *info)
{
	int		smax = info->smax;
	int		tmax = info->tmax;
	int		size = smax * tmax;
	int		i, maps;
	const byte	*colored = GL2_SurfaceSamples (surf);
	const byte	*white = surf->samples;
	byte		*dest;

	if (info->lightmap < 0 || !gl2_lightmap_data[info->lightmap])
		return;
	if (size <= 0 || size > LMBLOCK_WIDTH * LMBLOCK_HEIGHT)
		return;

	memset (gl2_blocklights, 0, (size_t)size * 3 * sizeof(float));
	info->cached_dlight = 0;

	if (r_fullbright.integer || !cl.worldmodel->lightdata)
	{
		for (i = 0; i < size * 3; i++)
			gl2_blocklights[i] = 255.0f * 256.0f;
	}
	else
	{
		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			float	scale = (float)d_lightstylevalue[surf->styles[maps]];

			if (colored)
			{
				for (i = 0; i < size; i++)
				{
					gl2_blocklights[i * 3 + 0] += colored[i * 3 + 0] * scale;
					gl2_blocklights[i * 3 + 1] += colored[i * 3 + 1] * scale;
					gl2_blocklights[i * 3 + 2] += colored[i * 3 + 2] * scale;
				}
				colored += (size_t)size * 3;
			}
			else if (white)
			{
				for (i = 0; i < size; i++)
				{
					float	v = white[i] * scale;

					gl2_blocklights[i * 3 + 0] += v;
					gl2_blocklights[i * 3 + 1] += v;
					gl2_blocklights[i * 3 + 2] += v;
				}
				white += size;
			}
			info->cached_style[maps] = d_lightstylevalue[surf->styles[maps]];
		}

		if (surf->dlightframe == gl2_dlightframecount && r_dynamic.integer)
		{
			GL2_AddDynamicLights (surf, info);
			info->cached_dlight = 1;
		}
	}

	/* gl_overbright halves what we store and the shader doubles it back,
	 * which is what the period's multitexture combiners did and buys a
	 * stop of headroom for bright dynamic lights. */
	dest = gl2_lightmap_data[info->lightmap] +
		((size_t)info->light_t * LMBLOCK_WIDTH + info->light_s) * 4;

	for (i = 0; i < tmax; i++)
	{
		byte	*row = dest + (size_t)i * LMBLOCK_WIDTH * 4;
		int	s;

		for (s = 0; s < smax; s++)
		{
			int	c;

			for (c = 0; c < 3; c++)
			{
				int	value = (int)gl2_blocklights[(i * smax + s) * 3 + c];

				value >>= gl2_lightmap_shift;
				if (value < 0)
					value = 0;
				else if (value > 255)
					value = 255;
				row[s * 4 + c] = (byte)value;
			}
			row[s * 4 + 3] = 255;
		}
	}
}

static void GL2_UploadLightmapBlock (const gl2surf_t *info)
{
	const byte	*base;

	if (info->lightmap < 0 || !gl2_lightmap_data[info->lightmap])
		return;

	base = gl2_lightmap_data[info->lightmap] +
		((size_t)info->light_t * LMBLOCK_WIDTH + info->light_s) * 4;

	GL2_BindName (1, gl2_lightmap_textures[info->lightmap]);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, LMBLOCK_WIDTH);
	glTexSubImage2D (GL_TEXTURE_2D, 0, info->light_s, info->light_t,
			info->smax, info->tmax, GL_RGBA, GL_UNSIGNED_BYTE, base);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
}

/*
================
GL2_UpdateLightmap

Cheap test first: there is nothing to do unless a light style moved or a
dynamic light is on this surface -- or was, last frame.
================
*/
static void GL2_UpdateLightmap (msurface_t *surf, gl2surf_t *info)
{
	int	maps;

	if (info->lightmap < 0)
		return;

	for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
	{
		if (info->cached_style[maps] != d_lightstylevalue[surf->styles[maps]])
			goto rebuild;
	}
	if (!info->cached_dlight &&
	    !(surf->dlightframe == gl2_dlightframecount && r_dynamic.integer))
		return;

rebuild:
	GL2_BuildLightmapBlock (surf, info);
	GL2_UploadLightmapBlock (info);
}

/*
================
GL2_BuildLightmaps

Runs once per map, and again whenever gl_overbright changes.
================
*/
void GL2_BuildLightmaps (void)
{
	int	i, surfnum;

	if (!gl2_surfaces || !cl.worldmodel)
		return;

	gl2_lightmap_overbright = gl_overbright.integer ? 1 : 0;
	gl2_lightmap_shift = 7 + gl2_lightmap_overbright;

	for (surfnum = 0; surfnum < gl2_numsurfaces; surfnum++)
	{
		gl2surf_t	*info = &gl2_surfaces[surfnum];

		if (info->lightmap < 0)
			continue;
		for (i = 0; i < MAXLIGHTMAPS; i++)
			info->cached_style[i] = -1;
		GL2_BuildLightmapBlock (&cl.worldmodel->surfaces[surfnum], info);
	}

	for (i = 0; i < gl2_numlightmaps; i++)
	{
		if (!gl2_lightmap_data[i])
			continue;
		if (!gl2_lightmap_textures[i])
			glGenTextures (1, &gl2_lightmap_textures[i]);
		GL2_BindName (1, gl2_lightmap_textures[i]);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, LMBLOCK_WIDTH, LMBLOCK_HEIGHT, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, gl2_lightmap_data[i]);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	GL2_InvalidateBindings ();
}

/*
=============================================================================

	geometry

=============================================================================
*/

static qboolean GL2_SurfaceIsLit (const msurface_t *surf)
{
	return !(surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED));
}

/*
================
GL2_SkyNewMap

Quake's sky layout: the right half of the 256x128 texture is the solid
back layer, the left half the cloud layer with palette index 0 punched out.
================
*/
void GL2_SkyNewMap (texture_t *texture)
{
	byte	*solid, *clouds;
	const byte *src;
	int	i, j;

	gl2_solidsky = gl2_alphasky = NULL;

	if (!texture || texture->width != 256 || texture->height != 128)
		return;

	solid = (byte *) malloc (128 * 128);
	clouds = (byte *) malloc (128 * 128);
	if (!solid || !clouds)
	{
		free (solid);
		free (clouds);
		return;
	}

	src = (const byte *)texture + texture->offsets[0];
	for (i = 0; i < 128; i++)
	{
		for (j = 0; j < 128; j++)
		{
			solid[i * 128 + j] = src[i * 256 + j + 128];
			clouds[i * 128 + j] = src[i * 256 + j];
		}
	}

	gl2_solidsky = GL2_LoadTexture ("*skysolid", 128, 128, solid, 0);
	gl2_alphasky = GL2_LoadTexture ("*skyclouds", 128, 128, clouds, GL2TEX_HOLEY);

	free (solid);
	free (clouds);
}

/*
================
GL2_LoadWorldTextures

The map's miptex data straight from the BSP, one GL texture per entry.
================
*/
static void GL2_LoadWorldTextures (qmodel_t *world)
{
	int	i;

	gl2_numtexchains = world->numtextures;
	gl2_textures_by_index = (gl2texture_t **)
		calloc ((size_t)q_max(gl2_numtexchains, 1), sizeof(gl2texture_t *));
	gl2_texchain = (int *) malloc ((size_t)q_max(gl2_numtexchains, 1) * sizeof(int));
	if (!gl2_textures_by_index || !gl2_texchain)
		Sys_Error ("WebGlide: out of memory for world textures");

	for (i = 0; i < gl2_numtexchains; i++)
	{
		texture_t	*texture = world->textures[i];
		unsigned int	flags = GL2TEX_MIPMAP;

		gl2_texchain[i] = -1;
		if (!texture || !texture->offsets[0])
			continue;

		if (!strncmp (texture->name, "sky", 3))
		{
			GL2_SkyNewMap (texture);
			continue;
		}
		if (texture->name[0] == '{')
			flags |= GL2TEX_HOLEY;

		gl2_textures_by_index[i] = GL2_LoadTexture (texture->name,
				(int)texture->width, (int)texture->height,
				(const byte *)texture + texture->offsets[0], flags);
	}
}

/*
================
GL2_BuildWorldBuffers

One pass over every surface in the map: allocate its lightmap rectangle,
emit its vertices, and emit the triangle fan indices that never change.
================
*/
static void GL2_BuildWorldBuffers (qmodel_t *world)
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

	vertices = (float *) malloc ((size_t)totalverts * GL2_VERTEX_FLOATS * sizeof(float));
	gl2_surf_indices = (GLuint *) malloc ((size_t)totalindices * sizeof(GLuint));
	gl2_frame_indices = (GLuint *) malloc ((size_t)totalindices * sizeof(GLuint));
	if (!vertices || !gl2_surf_indices || !gl2_frame_indices)
		Sys_Error ("WebGlide: out of memory for world geometry");
	gl2_total_indices = totalindices;

	for (surfnum = 0; surfnum < world->numsurfaces; surfnum++)
	{
		msurface_t		*surf = &world->surfaces[surfnum];
		gl2surf_t		*info = &gl2_surfaces[surfnum];
		const mtexinfo_t	*tex = surf->texinfo;
		int			i, numverts = surf->numedges;
		qboolean		turb = (surf->flags & SURF_DRAWTURB) != 0;

		if (numverts < 3)
			continue;

		if (GL2_SurfaceIsLit (surf))
		{
			info->smax = (short)((surf->extents[0] >> 4) + 1);
			info->tmax = (short)((surf->extents[1] >> 4) + 1);
			info->lightmap = GL2_AllocLightmapBlock (info->smax, info->tmax,
						&info->light_s, &info->light_t);
		}

		info->firstvert = vertexcursor;
		info->numverts = numverts;
		info->firstindex = indexcursor;
		info->numindices = (numverts - 2) * 3;

		for (i = 0; i < numverts; i++)
		{
			int		lindex = world->surfedges[surf->firstedge + i];
			float		*out = &vertices[(size_t)(vertexcursor + i) * GL2_VERTEX_FLOATS];
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

			if (turb)
			{
				/* Liquids warp in texel space; the shader
				 * normalises with u_turbscale. */
				out[3] = s;
				out[4] = t;
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
				out[5] = ls / (LMBLOCK_WIDTH * 16);
				out[6] = lt / (LMBLOCK_HEIGHT * 16);
			}
			else
			{
				out[5] = out[6] = 0.0f;
			}
		}

		for (i = 0; i < numverts - 2; i++)
		{
			gl2_surf_indices[indexcursor + i * 3 + 0] = (GLuint)vertexcursor;
			gl2_surf_indices[indexcursor + i * 3 + 1] = (GLuint)(vertexcursor + i + 1);
			gl2_surf_indices[indexcursor + i * 3 + 2] = (GLuint)(vertexcursor + i + 2);
		}

		vertexcursor += numverts;
		indexcursor += info->numindices;
	}

	glGenVertexArrays (1, &gl2_world_vao);
	glBindVertexArray (gl2_world_vao);

	glGenBuffers (1, &gl2_world_vbo);
	glBindBuffer (GL_ARRAY_BUFFER, gl2_world_vbo);
	glBufferData (GL_ARRAY_BUFFER,
			(GLsizeiptr)totalverts * GL2_VERTEX_FLOATS * sizeof(float),
			vertices, GL_STATIC_DRAW);

	glGenBuffers (1, &gl2_world_ibo);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl2_world_ibo);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER,
			(GLsizeiptr)totalindices * sizeof(GLuint), NULL, GL_STREAM_DRAW);

	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE,
			GL2_VERTEX_FLOATS * sizeof(float), (const void *)0);
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
			GL2_VERTEX_FLOATS * sizeof(float),
			(const void *)(uintptr_t)(3 * sizeof(float)));
	glEnableVertexAttribArray (2);
	glVertexAttribPointer (2, 2, GL_FLOAT, GL_FALSE,
			GL2_VERTEX_FLOATS * sizeof(float),
			(const void *)(uintptr_t)(5 * sizeof(float)));

	glBindVertexArray (0);
	free (vertices);
}

/*
=============================================================================

	visibility

=============================================================================
*/

static void GL2_MarkLeaves (void)
{
	static mleaf_t	*oldviewleaf;
	static int	oldnovis = -1;
	byte		*vis;
	mnode_t		*node;
	int		i;

	if (gl2_viewleaf == oldviewleaf && r_novis.integer == oldnovis)
		return;
	oldviewleaf = gl2_viewleaf;
	oldnovis = r_novis.integer;
	gl2_visframecount++;

	if (r_novis.integer || !gl2_viewleaf || !cl.worldmodel->visdata)
	{
		for (i = 0; i < cl.worldmodel->numleafs; i++)
		{
			node = (mnode_t *)&cl.worldmodel->leafs[i + 1];
			do
			{
				if (node->visframe == gl2_visframecount)
					break;
				node->visframe = gl2_visframecount;
				node = node->parent;
			} while (node);
		}
		return;
	}

	vis = Mod_LeafPVS (gl2_viewleaf, cl.worldmodel);
	for (i = 0; i < cl.worldmodel->numleafs; i++)
	{
		if (!(vis[i >> 3] & (1 << (i & 7))))
			continue;
		node = (mnode_t *)&cl.worldmodel->leafs[i + 1];
		do
		{
			if (node->visframe == gl2_visframecount)
				break;
			node->visframe = gl2_visframecount;
			node = node->parent;
		} while (node);
	}
}

static void GL2_ChainSurface (int surfnum, const msurface_t *surf)
{
	gl2surf_t	*info = &gl2_surfaces[surfnum];

	if (!info->numindices)
		return;

	if (surf->flags & SURF_DRAWSKY)
	{
		info->chain = gl2_skychain;
		gl2_skychain = surfnum;
	}
	else if (surf->flags & SURF_DRAWTURB)
	{
		info->chain = gl2_waterchain;
		gl2_waterchain = surfnum;
	}
	else if (info->texindex >= 0)
	{
		info->chain = gl2_texchain[info->texindex];
		gl2_texchain[info->texindex] = surfnum;
	}
}

static void GL2_ResetChains (void)
{
	int	i;

	for (i = 0; i < gl2_numtexchains; i++)
		gl2_texchain[i] = -1;
	gl2_skychain = -1;
	gl2_waterchain = -1;
}

/*
================
GL2_TextureAnimation

Walks the animation chain for the current time, honouring the alternate
sequence an entity's frame selects.
================
*/
static int GL2_TextureAnimation (int texindex, int frame)
{
	texture_t	*base;
	int		relative, count = 0, i;

	if (texindex < 0 || texindex >= gl2_numtexchains)
		return texindex;
	base = cl.worldmodel->textures[texindex];
	if (!base)
		return texindex;

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

	for (i = 0; i < gl2_numtexchains; i++)
	{
		if (cl.worldmodel->textures[i] == base)
			return i;
	}
	return texindex;
}

static void GL2_RecursiveWorldNode (mnode_t *node)
{
	int		c, side, surfnum;
	mplane_t	*plane;
	msurface_t	*surf;
	float		dot;

	while (1)
	{
		if (node->contents == CONTENTS_SOLID)
			return;
		if (node->visframe != gl2_visframecount)
			return;
		if (GL2_CullBox (node->minmaxs, node->minmaxs + 3))
			return;

		if (node->contents < 0)
		{
			mleaf_t		*leaf = (mleaf_t *)node;
			msurface_t	**mark = leaf->firstmarksurface;

			c = leaf->nummarksurfaces;
			while (c--)
			{
				(*mark)->visframe = gl2_visframecount;
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
			dot = gl2_modelorg[0] - plane->dist;
			break;
		case PLANE_Y:
			dot = gl2_modelorg[1] - plane->dist;
			break;
		case PLANE_Z:
			dot = gl2_modelorg[2] - plane->dist;
			break;
		default:
			dot = DotProduct (gl2_modelorg, plane->normal) - plane->dist;
			break;
		}
		side = (dot >= 0) ? 0 : 1;

		GL2_RecursiveWorldNode (node->children[side]);

		surfnum = node->firstsurface;
		surf = cl.worldmodel->surfaces + surfnum;
		for (c = node->numsurfaces; c; c--, surf++, surfnum++)
		{
			if (surf->visframe != gl2_visframecount)
				continue;
			if (((surf->flags & SURF_PLANEBACK) != 0) != (side != 0))
				continue;
			GL2_ChainSurface (surfnum, surf);
		}

		node = node->children[!side];
	}
}

/*
=============================================================================

	drawing

=============================================================================
*/

static void GL2_DrawIndices (int count)
{
	if (count <= 0)
		return;
	glBufferSubData (GL_ELEMENT_ARRAY_BUFFER, 0,
			(GLsizeiptr)count * sizeof(GLuint), gl2_frame_indices);
	glDrawElements (GL_TRIANGLES, count, GL_UNSIGNED_INT, (const void *)0);
	gl2_frame_polys += count / 3;
	gl2_frame_batches++;
}

static int GL2_AppendSurface (int count, const gl2surf_t *info)
{
	if (count + info->numindices > gl2_total_indices)
		return count;
	memcpy (&gl2_frame_indices[count], &gl2_surf_indices[info->firstindex],
		(size_t)info->numindices * sizeof(GLuint));
	return count + info->numindices;
}

/*
================
GL2_DrawChainLightmapped

Draws one texture's chain, split into one batch per lightmap atlas so a
draw call never needs two lightmap textures.  The split is done by
bucketing the chain once rather than rescanning it per atlas.
================
*/
static void GL2_DrawChainLightmapped (int head, msurface_t *surfaces)
{
	int	surfnum, i;
	int	used[WEB_MAX_LIGHTMAPS];
	int	numused = 0;

	for (surfnum = head; surfnum >= 0; surfnum = gl2_surfaces[surfnum].chain)
	{
		gl2surf_t	*info = &gl2_surfaces[surfnum];
		int		lm = info->lightmap;

		if (lm < 0 || lm >= WEB_MAX_LIGHTMAPS)
			continue;
		if (gl2_lmhead[lm] < 0 && numused < WEB_MAX_LIGHTMAPS)
			used[numused++] = lm;
		info->lmchain = gl2_lmhead[lm];
		gl2_lmhead[lm] = surfnum;
	}

	for (i = 0; i < numused; i++)
	{
		int	lm = used[i];
		int	count = 0;

		for (surfnum = gl2_lmhead[lm]; surfnum >= 0;
		     surfnum = gl2_surfaces[surfnum].lmchain)
		{
			gl2surf_t	*info = &gl2_surfaces[surfnum];

			GL2_UpdateLightmap (&surfaces[surfnum], info);
			count = GL2_AppendSurface (count, info);
		}
		gl2_lmhead[lm] = -1;

		if (count)
		{
			GL2_BindName (1, gl2_lightmap_textures[lm]);
			GL2_DrawIndices (count);
		}
	}
}

/*
================
GL2_DrawTextureChains
================
*/
static void GL2_DrawTextureChains (void)
{
	const gl2program_t	*program = &gl2_world_program;
	int			i;

	GL2_UseProgram (program);
	glUniformMatrix4fv (program->u_mvp, 1, GL_FALSE, gl2_view_projection.m);
	glUniform2f (program->u_scroll, 0.0f, 0.0f);
	glUniform1f (program->u_overbright, gl2_lightmap_overbright ? 2.0f : 1.0f);
	glUniform1f (program->u_alpha, 1.0f);
	glUniform1f (program->u_turbtime, gl2_frametime);
	glUniform2f (program->u_turbscale, 1.0f, 1.0f);
	glUniform1i (program->u_flags, GL2_WORLDFLAG_LIGHTMAP);
	GL2_SetupSceneUniforms (program);

	for (i = 0; i < gl2_numtexchains; i++)
	{
		if (gl2_texchain[i] < 0)
			continue;
		GL2_Bind (0, gl2_textures_by_index[GL2_TextureAnimation (i, 0)]);
		GL2_DrawChainLightmapped (gl2_texchain[i], cl.worldmodel->surfaces);
	}
}

/*
================
GL2_DrawWaterChain

Liquids: warped in the shader, optionally translucent, batched per texture
so each gets the right warp scale.
================
*/
static void GL2_DrawWaterChain (msurface_t *surfaces)
{
	const gl2program_t	*program = &gl2_world_program;
	float			alpha;
	int			i;

	if (gl2_waterchain < 0)
		return;

	alpha = r_wateralpha.value;
	if (alpha < 0.0f)
		alpha = 0.0f;
	else if (alpha > 1.0f)
		alpha = 1.0f;

	GL2_UseProgram (program);
	glUniformMatrix4fv (program->u_mvp, 1, GL_FALSE, gl2_view_projection.m);
	glUniform2f (program->u_scroll, 0.0f, 0.0f);
	glUniform1f (program->u_overbright, 1.0f);
	glUniform1f (program->u_alpha, alpha);
	glUniform1f (program->u_turbtime, gl2_frametime);
	glUniform1i (program->u_flags, GL2_WORLDFLAG_TURB);
	GL2_SetupSceneUniforms (program);

	if (alpha < 1.0f)
	{
		glEnable (GL_BLEND);
		glDepthMask (GL_FALSE);
	}

	/* Re-bucket the chain by texture: the same lmchain field, reused. */
	for (i = 0; i < gl2_numtexchains; i++)
		gl2_texchain[i] = -1;

	for (i = gl2_waterchain; i >= 0; i = gl2_surfaces[i].chain)
	{
		gl2surf_t	*info = &gl2_surfaces[i];
		int		animated;

		if (info->texindex < 0)
			continue;
		animated = GL2_TextureAnimation (info->texindex, 0);
		info->lmchain = gl2_texchain[animated];
		gl2_texchain[animated] = i;
	}
	gl2_waterchain = -1;

	for (i = 0; i < gl2_numtexchains; i++)
	{
		const texture_t	*texture = cl.worldmodel->textures[i];
		int		count = 0;
		int		surfnum;

		if (gl2_texchain[i] < 0)
			continue;

		glUniform2f (program->u_turbscale,
			(texture && texture->width) ? 1.0f / (float)texture->width : 1.0f,
			(texture && texture->height) ? 1.0f / (float)texture->height : 1.0f);
		GL2_Bind (0, gl2_textures_by_index[i]);

		for (surfnum = gl2_texchain[i]; surfnum >= 0;
		     surfnum = gl2_surfaces[surfnum].lmchain)
			count = GL2_AppendSurface (count, &gl2_surfaces[surfnum]);
		gl2_texchain[i] = -1;

		GL2_DrawIndices (count);
	}

	(void)surfaces;

	if (alpha < 1.0f)
	{
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
	}
}

/*
================
GL2_DrawSkyChain

Two scrolling layers projected onto the view vector, the way GLQuake did
it, with the cloud layer alpha-blended over the solid one.
================
*/
static void GL2_DrawSkyChain (void)
{
	const gl2program_t	*program = &gl2_sky_program;
	int			count = 0, surfnum;

	if (gl2_skychain < 0)
		return;

	if (!gl2_solidsky || !gl2_alphasky)
	{
		gl2_skychain = -1;
		return;
	}

	for (surfnum = gl2_skychain; surfnum >= 0; surfnum = gl2_surfaces[surfnum].chain)
		count = GL2_AppendSurface (count, &gl2_surfaces[surfnum]);
	gl2_skychain = -1;
	if (!count)
		return;

	GL2_UseProgram (program);
	glUniformMatrix4fv (program->u_mvp, 1, GL_FALSE, gl2_view_projection.m);
	glUniform3f (program->u_eye, r_origin[0], r_origin[1], r_origin[2]);
	glUniform1f (program->u_time, gl2_frametime);
	GL2_SetupSceneUniforms (program);

	GL2_Bind (0, gl2_solidsky);
	GL2_Bind (2, gl2_alphasky);

	GL2_DrawIndices (count);
}

/*
================
GL2_DrawWorld
================
*/
void GL2_DrawWorld (void)
{
	if (!cl.worldmodel || !gl2_surfaces || !gl2_world_vao || !GL2_ShadersReady ())
		return;

	if (gl2_lightmap_overbright != (gl_overbright.integer ? 1 : 0))
		GL2_BuildLightmaps ();

	VectorCopy (r_origin, gl2_modelorg);
	GL2_ResetChains ();

	GL2_MarkLeaves ();
	GL2_RecursiveWorldNode (cl.worldmodel->nodes);

	glBindVertexArray (gl2_world_vao);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl2_world_ibo);

	glEnable (GL_DEPTH_TEST);
	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);

	GL2_DrawSkyChain ();
	GL2_DrawTextureChains ();
	GL2_DrawWaterChain (cl.worldmodel->surfaces);

	glBindVertexArray (0);
}

/*
================
GL2_DrawBrushEntity

Submodels index the same vertex buffer as the world -- every brush model
in a map shares cl.worldmodel->surfaces -- so the only extra work is a
model matrix and its own surface walk.
================
*/
void GL2_DrawBrushEntity (entity_t *entity)
{
	qmodel_t		*model = entity->model;
	const gl2program_t	*program = &gl2_world_program;
	gl2matrix_t		model_matrix, mvp;
	vec3_t			mins, maxs;
	msurface_t		*surf;
	int			i, surfnum;
	float			alpha = 1.0f;
	qboolean		rotated;

	if (!model || model->type != mod_brush || !gl2_world_vao || !gl2_surfaces)
		return;
	if (!GL2_ShadersReady ())
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

	if (GL2_CullBox (mins, maxs))
		return;

	VectorSubtract (r_origin, entity->origin, gl2_modelorg);
	if (rotated)
	{
		vec3_t	forward, right, up, temp;

		VectorCopy (gl2_modelorg, temp);
		AngleVectors (entity->angles, forward, right, up);
		gl2_modelorg[0] = DotProduct (temp, forward);
		gl2_modelorg[1] = -DotProduct (temp, right);
		gl2_modelorg[2] = DotProduct (temp, up);
	}

	if (entity->drawflags & DRF_TRANSLUCENT)
		alpha = r_wateralpha.value;

	GL2_RotateForEntity (&model_matrix, entity->origin, entity->angles);
	GL2_MatrixMultiply (&mvp, &gl2_view_projection, &model_matrix);

	GL2_ResetChains ();

	surfnum = model->firstmodelsurface;
	surf = &model->surfaces[surfnum];
	for (i = 0; i < model->nummodelsurfaces; i++, surf++, surfnum++)
	{
		mplane_t	*plane = surf->plane;
		float		dot = DotProduct (gl2_modelorg, plane->normal) - plane->dist;

		if (((surf->flags & SURF_PLANEBACK) && dot < -GL2_BACKFACE_EPSILON) ||
		    (!(surf->flags & SURF_PLANEBACK) && dot > GL2_BACKFACE_EPSILON))
		{
			gl2_surfaces[surfnum].chain = -1;
			GL2_ChainSurface (surfnum, surf);
		}
	}

	glBindVertexArray (gl2_world_vao);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, gl2_world_ibo);

	if (alpha < 1.0f)
	{
		glEnable (GL_BLEND);
		glDepthMask (GL_FALSE);
	}

	GL2_UseProgram (program);
	glUniformMatrix4fv (program->u_mvp, 1, GL_FALSE, mvp.m);
	glUniform2f (program->u_scroll, 0.0f, 0.0f);
	glUniform1f (program->u_overbright, gl2_lightmap_overbright ? 2.0f : 1.0f);
	glUniform1f (program->u_alpha, alpha);
	glUniform1f (program->u_turbtime, gl2_frametime);
	glUniform2f (program->u_turbscale, 1.0f, 1.0f);
	glUniform1i (program->u_flags, GL2_WORLDFLAG_LIGHTMAP);
	GL2_SetupSceneUniforms (program);

	for (i = 0; i < gl2_numtexchains; i++)
	{
		if (gl2_texchain[i] < 0)
			continue;
		GL2_Bind (0, gl2_textures_by_index[GL2_TextureAnimation (i, entity->frame)]);
		GL2_DrawChainLightmapped (gl2_texchain[i], model->surfaces);
	}

	if (gl2_waterchain >= 0)
	{
		/* The water pass rebinds u_mvp, so restore the entity's. */
		GL2_DrawWaterChain (model->surfaces);
		GL2_UseProgram (program);
		glUniformMatrix4fv (program->u_mvp, 1, GL_FALSE, mvp.m);
	}

	if (alpha < 1.0f)
	{
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
	}

	glBindVertexArray (0);
}

/*
=============================================================================

	point lighting for models

=============================================================================
*/

static float	gl2_lightspot_color[3];

static int GL2_RecursiveLightPoint (mnode_t *node, const vec3_t start, const vec3_t end)
{
	float		front, back, frac;
	int		side;
	mplane_t	*plane;
	vec3_t		mid;
	msurface_t	*surf;
	int		s, t, ds, dt, i;

	if (!node || node->contents < 0)
		return -1;

	plane = node->plane;
	front = DotProduct (start, plane->normal) - plane->dist;
	back = DotProduct (end, plane->normal) - plane->dist;
	side = front < 0;

	if ((back < 0) == side)
		return GL2_RecursiveLightPoint (node->children[side], start, end);

	frac = front / (front - back);
	mid[0] = start[0] + (end[0] - start[0]) * frac;
	mid[1] = start[1] + (end[1] - start[1]) * frac;
	mid[2] = start[2] + (end[2] - start[2]) * frac;

	i = GL2_RecursiveLightPoint (node->children[side], start, mid);
	if (i >= 0)
		return i;

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		const mtexinfo_t	*tex;
		const byte		*lightmap;
		const byte		*colored;
		int			maps, smax, tmax;
		float			accum[3];

		if (surf->flags & SURF_DRAWTILED)
			continue;

		tex = surf->texinfo;
		s = (int)(DotProduct (mid, tex->vecs[0]) + tex->vecs[0][3]);
		t = (int)(DotProduct (mid, tex->vecs[1]) + tex->vecs[1][3]);
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
		smax = (surf->extents[0] >> 4) + 1;
		tmax = (surf->extents[1] >> 4) + 1;

		accum[0] = accum[1] = accum[2] = 0.0f;
		lightmap = surf->samples + (size_t)dt * smax + ds;
		colored = GL2_SurfaceSamples (surf);
		if (colored)
			colored += ((size_t)dt * smax + ds) * 3;

		for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
		{
			float	scale = d_lightstylevalue[surf->styles[maps]] * (1.0f / 256.0f);

			if (colored)
			{
				accum[0] += colored[0] * scale;
				accum[1] += colored[1] * scale;
				accum[2] += colored[2] * scale;
				colored += (size_t)smax * tmax * 3;
			}
			else
			{
				float	v = lightmap[0] * scale;

				accum[0] += v;
				accum[1] += v;
				accum[2] += v;
			}
			lightmap += (size_t)smax * tmax;
		}

		gl2_lightspot_color[0] = accum[0];
		gl2_lightspot_color[1] = accum[1];
		gl2_lightspot_color[2] = accum[2];

		return (int)((accum[0] + accum[1] + accum[2]) * (1.0f / 3.0f));
	}

	return GL2_RecursiveLightPoint (node->children[!side], mid, end);
}

/*
================
GL2_LightPoint

Returns the average intensity at a point and, in colour, the RGB the
lightmap -- or the map's .lit -- says it should be.
================
*/
int GL2_LightPoint (const vec3_t point, vec3_t color)
{
	vec3_t	end;
	int	r;

	if (!cl.worldmodel || !cl.worldmodel->lightdata)
	{
		color[0] = color[1] = color[2] = 255.0f;
		return 255;
	}

	end[0] = point[0];
	end[1] = point[1];
	end[2] = point[2] - 2048;

	gl2_lightspot_color[0] = gl2_lightspot_color[1] = gl2_lightspot_color[2] = 0.0f;
	r = GL2_RecursiveLightPoint (cl.worldmodel->nodes, point, end);
	if (r < 0)
		r = 0;

	color[0] = gl2_lightspot_color[0];
	color[1] = gl2_lightspot_color[1];
	color[2] = gl2_lightspot_color[2];
	return r;
}

/*
=============================================================================

	dynamic light marking

=============================================================================
*/

static void GL2_MarkLightsNode (const dlight_t *light, unsigned int bit, mnode_t *node)
{
	mplane_t	*splitplane;
	float		dist;
	msurface_t	*surf;
	int		i;

	if (!node || node->contents < 0)
		return;

	splitplane = node->plane;
	dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

	if (dist > light->radius)
	{
		GL2_MarkLightsNode (light, bit, node->children[0]);
		return;
	}
	if (dist < -light->radius)
	{
		GL2_MarkLightsNode (light, bit, node->children[1]);
		return;
	}

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
	{
		if (surf->dlightframe != gl2_dlightframecount)
		{
			surf->dlightbits = 0;
			surf->dlightframe = gl2_dlightframecount;
		}
		surf->dlightbits |= bit;
	}

	GL2_MarkLightsNode (light, bit, node->children[0]);
	GL2_MarkLightsNode (light, bit, node->children[1]);
}

void GL2_PushDlights (void)
{
	int		i;
	const dlight_t	*light;

	if (!cl.worldmodel || !r_dynamic.integer)
		return;

	gl2_dlightframecount++;

	light = cl_dlights;
	for (i = 0; i < MAX_DLIGHTS; i++, light++)
	{
		if (light->die < cl.time || !light->radius)
			continue;
		GL2_MarkLightsNode (light, 1u << i, cl.worldmodel->nodes);
	}
}

void GL2_AnimateLight (void)
{
	int	i, j, k;

	i = (int)(cl.time * 10);
	for (j = 0; j < MAX_LIGHTSTYLES; j++)
	{
		if (!cl_lightstyle[j].length)
		{
			d_lightstylevalue[j] = 256;
			continue;
		}
		k = i % cl_lightstyle[j].length;
		k = cl_lightstyle[j].map[k] - 'a';
		d_lightstylevalue[j] = k * 22;
	}
}

/*
=============================================================================

	setup and teardown

=============================================================================
*/

void GL2_WorldShutdown (void)
{
	int	i;

	if (gl2_world_vbo)
	{
		glDeleteBuffers (1, &gl2_world_vbo);
		gl2_world_vbo = 0;
	}
	if (gl2_world_ibo)
	{
		glDeleteBuffers (1, &gl2_world_ibo);
		gl2_world_ibo = 0;
	}
	if (gl2_world_vao)
	{
		glDeleteVertexArrays (1, &gl2_world_vao);
		gl2_world_vao = 0;
	}
	for (i = 0; i < WEB_MAX_LIGHTMAPS; i++)
	{
		if (gl2_lightmap_textures[i])
		{
			glDeleteTextures (1, &gl2_lightmap_textures[i]);
			gl2_lightmap_textures[i] = 0;
		}
		free (gl2_lightmap_data[i]);
		gl2_lightmap_data[i] = NULL;
		gl2_lmhead[i] = -1;
	}
	gl2_numlightmaps = 0;

	free (gl2_surfaces);
	gl2_surfaces = NULL;
	gl2_numsurfaces = 0;
	free (gl2_surf_indices);
	gl2_surf_indices = NULL;
	free (gl2_frame_indices);
	gl2_frame_indices = NULL;
	gl2_total_indices = 0;
	free (gl2_texchain);
	gl2_texchain = NULL;
	free (gl2_textures_by_index);
	gl2_textures_by_index = NULL;
	gl2_numtexchains = 0;
	gl2_skychain = gl2_waterchain = -1;
	gl2_solidsky = gl2_alphasky = NULL;
	gl2_litdata = NULL;
	gl2_lit_loaded = false;
}

/*
================
GL2_WorldNewMap
================
*/
void GL2_WorldNewMap (void)
{
	qmodel_t	*world = cl.worldmodel;
	int		i;

	GL2_WorldShutdown ();

	if (!world || !world->numsurfaces)
		return;

	gl2_numsurfaces = world->numsurfaces;
	gl2_surfaces = (gl2surf_t *) calloc ((size_t)gl2_numsurfaces, sizeof(gl2surf_t));
	if (!gl2_surfaces)
		Sys_Error ("WebGlide: out of memory for the surface table");

	for (i = 0; i < gl2_numsurfaces; i++)
	{
		gl2_surfaces[i].chain = -1;
		gl2_surfaces[i].lmchain = -1;
		gl2_surfaces[i].lightmap = -1;
		gl2_surfaces[i].texindex = -1;
	}
	for (i = 0; i < WEB_MAX_LIGHTMAPS; i++)
		gl2_lmhead[i] = -1;

	memset (gl2_lightmap_allocated, 0, sizeof(gl2_lightmap_allocated));
	gl2_numlightmaps = 0;
	gl2_lightmap_overbright = gl_overbright.integer ? 1 : 0;
	gl2_lightmap_shift = 7 + gl2_lightmap_overbright;

	GL2_LoadLitFile (world);
	GL2_LoadWorldTextures (world);

	for (i = 0; i < gl2_numsurfaces; i++)
	{
		const texture_t	*texture = world->surfaces[i].texinfo->texture;
		int		j;

		for (j = 0; j < gl2_numtexchains; j++)
		{
			if (world->textures[j] == texture)
			{
				gl2_surfaces[i].texindex = j;
				break;
			}
		}
	}

	GL2_BuildWorldBuffers (world);
	GL2_BuildLightmaps ();

	Con_DPrintf ("WebGlide: %d surfaces, %d lightmap atlases, %s lighting\n",
			gl2_numsurfaces, gl2_numlightmaps,
			gl2_lit_loaded ? "coloured" : "greyscale");
}

qboolean GL2_WorldHasColoredLight (void)
{
	return gl2_lit_loaded;
}
