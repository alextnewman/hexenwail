/*
 * wgpu_entity.c -- alias models, sprites and the view weapon for
 * WebGlideNitro.
 *
 * The world is immutable geometry the GPU keeps; entities are the opposite,
 * so they are built the way the software renderer builds them.  Every alias
 * model is posed, scaled, rotated and lit on the CPU into one streamed
 * vertex arena, and the arena is uploaded once per frame.  Nothing here is
 * a per-model buffer, a per-model bind group, or a per-model uniform: the
 * only thing that changes between two draws is the skin's bind group.
 *
 * What stays indexed is the shading.  A vertex does not carry a lit RGB
 * colour; it carries the colormap row d_polyse.c would have interpolated
 * across the triangle, so the fragment shader can do exactly what the
 * rasteriser's inner loop does -- colormap[row][skin index] -- and the
 * palette turns that into colour last.  Hexen II's per-entity colorshade
 * stays indexed too: it is a row of gfx/tinttab.lmp, applied as an
 * index-to-index remap, which is the same table R_AliasDrawModel builds
 * globalcolormap from.
 *
 * The maths -- pose selection, skin groups, the scale types and origins,
 * EF_ROTATE's bob and spin, EF_FACE_VIEW, the sprite orientations, the
 * lighting clamps -- are facts about Hexen II, and are reproduced from the
 * software renderer (r_alias.c, r_main.c, d_sprite.c) with WebGlide's
 * gl2_alias.c as a cross-check.  No GL call is translated here.
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

#define NITRO_NUMVERTEXNORMALS	162

/* r_alias.c's floor, applied on the single path every software model goes
 * through.  The light-style shortcuts below return early, so it is applied
 * at the one call site instead. */
#define LIGHT_MIN		5.0f

/* r_main.c:806, "always give some light on gun".  The view model is the one
 * thing on screen the player cannot walk away from, so it samples the world
 * at its own origin rather than the model's mid-point and floors the result
 * far above the world's floor. */
#define VIEWMODEL_LIGHT_MIN	24.0f

static const float	nitro_avertexnormals[NITRO_NUMVERTEXNORMALS][3] =
{
#include "anorms.h"
};

/*
=============================================================================

	the frame's model arena

	One vertex array and one batch list, both grown on demand and reset
	every frame.  A batch is a texture plus a blend mode; consecutive
	models sharing both are merged, so a room full of one monster costs
	one draw call.

=============================================================================
*/

static wgpumodel_vertex_t	*nitro_model_vertices;
static int			nitro_model_capacity;
static int			nitro_model_count;

static wgpumodelbatch_t		*nitro_model_batches;
static int			nitro_model_batch_capacity;
static int			nitro_model_batch_count;
static int			nitro_model_opaque_batches;

static int		nitro_batch_texture;
static unsigned int	nitro_batch_flags;
static int		nitro_batch_open;

extern cvar_t	r_shadows;

static void WGPUEntity_FlushBatch (void)
{
	wgpumodelbatch_t	*batch;

	if (!nitro_batch_open)
		return;
	nitro_batch_open = 0;

	batch = &nitro_model_batches[nitro_model_batch_count - 1];
	batch->count = nitro_model_count - batch->first;
	if (batch->count <= 0)
	{
		nitro_model_batch_count--;
		return;
	}
	WebPerf_CountDraw (batch->count / 3);
}

static void WGPUEntity_BeginBatch (int texture, unsigned int flags)
{
	wgpumodelbatch_t	*batch;

	if (nitro_batch_open && nitro_batch_texture == texture &&
	    nitro_batch_flags == flags)
		return;

	WGPUEntity_FlushBatch ();

	if (nitro_model_batch_count == nitro_model_batch_capacity)
	{
		int			capacity = nitro_model_batch_capacity ?
						nitro_model_batch_capacity * 2 : 128;
		wgpumodelbatch_t	*grown = (wgpumodelbatch_t *)
					realloc (nitro_model_batches,
						(size_t)capacity * sizeof(wgpumodelbatch_t));

		if (!grown)
			Sys_Error ("WebGlideNitro: out of memory for %d model batches", capacity);
		nitro_model_batches = grown;
		nitro_model_batch_capacity = capacity;
	}

	batch = &nitro_model_batches[nitro_model_batch_count++];
	batch->texture = texture;
	batch->first = nitro_model_count;
	batch->count = 0;
	batch->flags = flags;

	nitro_batch_texture = texture;
	nitro_batch_flags = flags;
	nitro_batch_open = 1;
}

static wgpumodel_vertex_t *WGPUEntity_Vertices (int count)
{
	wgpumodel_vertex_t	*out;

	if (nitro_model_count + count > nitro_model_capacity)
	{
		int			capacity = nitro_model_capacity;
		wgpumodel_vertex_t	*grown;

		while (capacity < nitro_model_count + count)
			capacity = capacity ? capacity * 2 : 4096;
		grown = (wgpumodel_vertex_t *) realloc (nitro_model_vertices,
				(size_t)capacity * sizeof(wgpumodel_vertex_t));
		if (!grown)
			Sys_Error ("WebGlideNitro: out of memory for %d model vertices", capacity);
		nitro_model_vertices = grown;
		nitro_model_capacity = capacity;
	}

	out = nitro_model_vertices + nitro_model_count;
	nitro_model_count += count;
	return out;
}

/*
=============================================================================

	the skin cache

	A Nitro texture is created once per (model, skin) pair and kept until
	the map changes.  Player skins are the one moving part: their pixels
	are a translation of the base skin through the entity's 256-entry
	colormap, so the cache entry carries a hash of that table and is
	re-specified in place when it changes.

=============================================================================
*/

typedef struct nitroskin_s
{
	struct nitroskin_s	*next;
	char			name[MAX_QPATH];
	int			id;
	int			width;
	int			height;
	unsigned int		tag;
} nitroskin_t;

#define NITRO_SKIN_HASH	256

static nitroskin_t	*nitro_skin_hash[NITRO_SKIN_HASH];

static unsigned int WGPUEntity_HashName (const char *name)
{
	unsigned int	hash = 2166136261u;

	while (*name)
	{
		hash ^= (unsigned char)*name++;
		hash *= 16777619u;
	}
	return hash;
}

/*
================
WGPUEntity_ColormapTag

A cheap FNV-1a over the 256-entry translation table, used to tell whether a
cached player skin is still the right colours.  Never returns 0, so it can
never match an entry nobody has tagged.
================
*/
static unsigned int WGPUEntity_ColormapTag (const byte *colormap)
{
	unsigned int	hash = 2166136261u;
	int		i;

	for (i = 0; i < 256; i++)
	{
		hash ^= colormap[i];
		hash *= 16777619u;
	}
	return hash | 1u;
}

static nitroskin_t *WGPUEntity_FindSkin (const char *name)
{
	nitroskin_t	*skin = nitro_skin_hash[WGPUEntity_HashName (name) % NITRO_SKIN_HASH];

	while (skin)
	{
		if (!strcmp (skin->name, name))
			return skin;
		skin = skin->next;
	}
	return NULL;
}

/*
================
WGPUEntity_LoadSkin

Returns the Nitro texture id for a named 8-bit image, uploading it the
first time it is asked for.  When the content tag of an existing entry
changes -- a player picking a new colour -- the texels are re-specified over
the same GPU texture rather than leaking a new one every frame.
================
*/
static int WGPUEntity_LoadSkin (const char *name, int width, int height,
				const byte *pixels, unsigned int flags, unsigned int tag)
{
	unsigned int	bucket = WGPUEntity_HashName (name) % NITRO_SKIN_HASH;
	nitroskin_t	*skin = WGPUEntity_FindSkin (name);

	if (skin)
	{
		if (skin->width == width && skin->height == height)
		{
			if (skin->tag != tag)
			{
				Nitro_UpdateTexture (skin->id, pixels);
				skin->tag = tag;
				WebPerf_CountUpload ((size_t)width * height);
			}
			return skin->id;
		}
		/* Same name, different size: the model was reloaded under it. */
		Nitro_DestroyTexture (skin->id);
		skin->id = Nitro_CreateTexture (name, width, height, pixels, flags);
		skin->width = width;
		skin->height = height;
		skin->tag = tag;
		return skin->id;
	}

	skin = (nitroskin_t *) calloc (1, sizeof(*skin));
	if (!skin)
		return 0;

	q_strlcpy (skin->name, name, sizeof(skin->name));
	skin->id = Nitro_CreateTexture (name, width, height, pixels, flags);
	skin->width = width;
	skin->height = height;
	skin->tag = tag;
	skin->next = nitro_skin_hash[bucket];
	nitro_skin_hash[bucket] = skin;
	WebPerf_CountUpload ((size_t)width * height);
	return skin->id;
}

static void WGPUEntity_FlushSkins (void)
{
	int	i;

	for (i = 0; i < NITRO_SKIN_HASH; i++)
	{
		nitroskin_t	*skin = nitro_skin_hash[i];

		while (skin)
		{
			nitroskin_t	*next = skin->next;

			Nitro_DestroyTexture (skin->id);
			free (skin);
			skin = next;
		}
		nitro_skin_hash[i] = NULL;
	}
}

/*
=============================================================================

	skins

=============================================================================
*/

/*
================
WGPUEntity_AliasSkinDesc

Skin groups animate on their own clock; single skins do not.  Mirrors
R_AliasSetupSkin without the software renderer's mip-map-far selection.
================
*/
static const maliasskindesc_t *WGPUEntity_AliasSkinDesc (const aliashdr_t *paliashdr,
						const newmdl_t *pmdl, entity_t *entity,
						int *skinnum_out)
{
	const maliasskindesc_t	*pskindesc;
	int			skinnum = entity->skinnum;

	if (skinnum >= 100)
		skinnum = 0;
	if (skinnum >= pmdl->numskins || skinnum < 0)
		skinnum = 0;
	*skinnum_out = skinnum;

	pskindesc = ((const maliasskindesc_t *)
			((const byte *)paliashdr + paliashdr->skindesc)) + skinnum;

	if (pskindesc->type == ALIAS_SKIN_GROUP)
	{
		const maliasskingroup_t	*group = (const maliasskingroup_t *)
				((const byte *)paliashdr + pskindesc->skin);
		const float	*intervals = (const float *)
				((const byte *)paliashdr + group->intervals);
		int		i, numskins = group->numskins;
		float		fullinterval, targettime, time;

		if (numskins < 1)
			return pskindesc;
		fullinterval = intervals[numskins - 1];
		time = cl.time + entity->syncbase;
		targettime = time - ((int)(time / fullinterval)) * fullinterval;
		for (i = 0; i < numskins - 1; i++)
		{
			if (intervals[i] > targettime)
				break;
		}
		pskindesc = &group->skindescs[i];
		*skinnum_out = skinnum * 100 + i;
	}

	return pskindesc;
}

static int WGPUEntity_AliasSkin (entity_t *entity, const aliashdr_t *paliashdr,
					const newmdl_t *pmdl)
{
	char			name[MAX_QPATH];
	const maliasskindesc_t	*pskindesc;
	const byte		*pixels;
	unsigned int		flags = 0;
	int			skinnum, size, entnum, id;

	pskindesc = WGPUEntity_AliasSkinDesc (paliashdr, pmdl, entity, &skinnum);
	pixels = (const byte *)paliashdr + pskindesc->skin;
	size = pmdl->skinwidth * pmdl->skinheight;
	if (size <= 0)
		return 0;

	if (entity->model->flags & (EF_HOLEY | EF_TRANSPARENT | EF_SPECIAL_TRANS))
		flags |= NITROTEX_HOLEY;

	entnum = (int)(entity - cl_entities) - 1;
	if (entity->colormap && entity->colormap != vid.colormap &&
	    entnum >= 0 && entnum < cl.maxclients)
	{
		/* Player colours are a 256-entry translation of the skin, so
		 * they stay a paletted image: index in, index out. */
		unsigned int	tag = WGPUEntity_ColormapTag (entity->colormap);
		nitroskin_t	*cached;
		byte		*translated;
		int		i;

		q_snprintf (name, sizeof(name), "%s:player%d:%d",
				entity->model->name, entnum, skinnum);

		/* Checking the 256-byte table first keeps the common case free
		 * of both the translate and the upload. */
		cached = WGPUEntity_FindSkin (name);
		if (cached && cached->tag == tag &&
		    cached->width == pmdl->skinwidth && cached->height == pmdl->skinheight)
			return cached->id;

		translated = (byte *) malloc ((size_t)size);
		if (translated)
		{
			for (i = 0; i < size; i++)
				translated[i] = entity->colormap[pixels[i]];
			id = WGPUEntity_LoadSkin (name, pmdl->skinwidth, pmdl->skinheight,
						translated, flags, tag);
			free (translated);
			return id;
		}
	}

	q_snprintf (name, sizeof(name), "%s:%d", entity->model->name, skinnum);
	return WGPUEntity_LoadSkin (name, pmdl->skinwidth, pmdl->skinheight,
				pixels, flags, 1u);
}

/*
=============================================================================

	lighting

=============================================================================
*/

static float	nitro_ambientlight;
static float	nitro_shadelight;
static vec3_t	nitro_lightdir;
static vec3_t	nitro_lightcolor;

static unsigned int WGPUEntity_PackLightColor (void)
{
	unsigned int	r, g, b;

	r = (unsigned int)CLAMP(0, (int)(nitro_lightcolor[0] * 64.0f + 0.5f), 255);
	g = (unsigned int)CLAMP(0, (int)(nitro_lightcolor[1] * 64.0f + 0.5f), 255);
	b = (unsigned int)CLAMP(0, (int)(nitro_lightcolor[2] * 64.0f + 0.5f), 255);
	return r | (g << 8) | (b << 16) | (255u << 24);
}

/*
================
WGPUEntity_SetupLighting

The Hexen II model light styles come first -- they override the world
outright -- then the sampled world light, then every dynamic light in range.
The result is a scalar intensity, not a colour, because the colormap row it
selects is what the software renderer would have selected.
================
*/
static void WGPUEntity_SetupLighting (entity_t *entity)
{
	vec3_t	adjust;
	wgpulightsample_t sample;
	int	mls;

	nitro_lightdir[0] = -1.0f;
	nitro_lightdir[1] = nitro_lightdir[2] = 0.0f;
	nitro_lightcolor[0] = nitro_lightcolor[1] = nitro_lightcolor[2] = 1.0f;
	mls = entity->drawflags & MLS_MASKIN;

	if (entity->model && (entity->model->flags & EF_ROTATE))
	{
		/* Bonus items pulse rather than sample the world. */
		nitro_ambientlight = nitro_shadelight =
			60.0f + 34.0f + (float)sin (entity->origin[0] + entity->origin[1] +
						(cl.time * 3.8)) * 34.0f;
		return;
	}
	if (mls == MLS_ABSLIGHT)
	{
		nitro_ambientlight = nitro_shadelight = (float)entity->abslight;
		return;
	}
	if (mls != MLS_NONE)
	{
		nitro_ambientlight = nitro_shadelight = d_lightstylevalue[24 + mls] / 2.0f;
		return;
	}

	VectorCopy (entity->origin, adjust);
	if (entity != &cl.viewent && entity->model)
		adjust[2] += (entity->model->mins[2] + entity->model->maxs[2]) * 0.5f;

	if (WGPULightVol_Sample (adjust, &sample))
	{
		nitro_ambientlight = sample.ambient;
		nitro_shadelight = sample.shade;
		VectorCopy (sample.direction, nitro_lightdir);
		if (sample.ambient > 0.001f)
			VectorScale (sample.color, 1.0f / sample.ambient, nitro_lightcolor);
	}
	else
	{
		sample.ambient = (float) WGPUWorld_LightPoint (adjust, NULL);
		sample.shade = sample.ambient;
		sample.color[0] = sample.ambient;
		sample.color[1] = sample.ambient;
		sample.color[2] = sample.ambient;
		sample.direction[0] = -1.0f;
		sample.direction[1] = sample.direction[2] = 0.0f;
		WGPULightVol_ApplyDynamic (adjust, &sample);
		nitro_ambientlight = sample.ambient;
		nitro_shadelight = sample.shade;
		VectorCopy (sample.direction, nitro_lightdir);
		if (sample.ambient > 0.001f)
			VectorScale (sample.color, 1.0f / sample.ambient, nitro_lightcolor);
	}

	if (entity == &cl.viewent && nitro_ambientlight < VIEWMODEL_LIGHT_MIN)
	{
		nitro_ambientlight = VIEWMODEL_LIGHT_MIN;
		if (nitro_shadelight < VIEWMODEL_LIGHT_MIN)
			nitro_shadelight = VIEWMODEL_LIGHT_MIN;
	}

	if (nitro_ambientlight < 0.0f)
		nitro_ambientlight = 0.0f;

	/* r_alias.c's clamps: the colormap has 64 rows and the top of the
	 * ramp is reached well before full intensity. */
	if (nitro_ambientlight > 128.0f)
		nitro_ambientlight = 128.0f;
	if (nitro_ambientlight + nitro_shadelight > 192.0f)
		nitro_shadelight = 192.0f - nitro_ambientlight;
}

static void WGPUEntity_ApplyLightFloor (void)
{
	if (nitro_ambientlight < LIGHT_MIN)
		nitro_ambientlight = LIGHT_MIN;
	if (nitro_shadelight < LIGHT_MIN)
		nitro_shadelight = LIGHT_MIN;
}

static int WGPUEntity_ViewModelLightLevel (entity_t *entity)
{
	WGPUEntity_SetupLighting (entity);
	if (nitro_ambientlight < 0.0f)
		return 0;
	if (nitro_ambientlight > 255.0f)
		return 255;
	return (int)nitro_ambientlight;
}

/*
=============================================================================

	alias models

=============================================================================
*/

/*
================
WGPUEntity_AliasPose

The pose a frame resolves to right now.  Frame groups run on their own
interval table, exactly as the software renderer reads them.
================
*/
static int WGPUEntity_AliasPose (const aliashdr_t *paliashdr, const newmdl_t *pmdl,
				entity_t *entity)
{
	const maliasframedesc_t	*pframedesc;
	const maliasgroup_t	*group;
	const float		*intervals;
	int			frame = entity->frame;
	int			i, numframes;
	float			fullinterval, targettime, time;

	if (frame >= pmdl->numframes || frame < 0)
		frame = 0;
	pframedesc = &paliashdr->frames[frame];

	if (pframedesc->type == ALIAS_SINGLE)
		return pframedesc->frame;

	group = (const maliasgroup_t *)((const byte *)paliashdr + pframedesc->frame);
	intervals = (const float *)((const byte *)paliashdr + group->intervals);
	numframes = group->numframes;
	if (numframes < 1)
		return pframedesc->frame;
	fullinterval = intervals[numframes - 1];
	time = cl.time + entity->syncbase;
	targettime = time - ((int)(time / fullinterval)) * fullinterval;
	for (i = 0; i < numframes - 1; i++)
	{
		if (intervals[i] > targettime)
			break;
	}
	return group->frames[i].frame;
}

/*
================
WGPUEntity_AliasScale

Hexen II scales models per entity, with three scale types and three scaling
origins.  Reproduced from R_AliasSetUpTransform so scaled monsters, shrunken
players and growing effects land in the same place.
================
*/
static void WGPUEntity_AliasScale (entity_t *entity, const newmdl_t *pmdl,
					vec3_t scale, vec3_t offset)
{
	float	entScale, xyfact = 1.0f, zfact = 1.0f;

	VectorCopy (pmdl->scale, scale);
	VectorCopy (pmdl->scale_origin, offset);

	if (entity->scale && entity->scale != 100)
	{
		entScale = (float)entity->scale / 100.0f;

		switch (entity->drawflags & SCALE_TYPE_MASKIN)
		{
		case SCALE_TYPE_UNIFORM:
			scale[0] = pmdl->scale[0] * entScale;
			scale[1] = pmdl->scale[1] * entScale;
			scale[2] = pmdl->scale[2] * entScale;
			xyfact = zfact = (entScale - 1.0f) * 127.95f;
			break;
		case SCALE_TYPE_XYONLY:
			scale[0] = pmdl->scale[0] * entScale;
			scale[1] = pmdl->scale[1] * entScale;
			xyfact = (entScale - 1.0f) * 127.95f;
			break;
		case SCALE_TYPE_ZONLY:
			scale[2] = pmdl->scale[2] * entScale;
			zfact = (entScale - 1.0f) * 127.95f;
			break;
		default:
			break;
		}

		switch (entity->drawflags & SCALE_ORIGIN_MASKIN)
		{
		case SCALE_ORIGIN_CENTER:
			offset[0] = pmdl->scale_origin[0] - pmdl->scale[0] * xyfact;
			offset[1] = pmdl->scale_origin[1] - pmdl->scale[1] * xyfact;
			offset[2] = pmdl->scale_origin[2] - pmdl->scale[2] * zfact;
			break;
		case SCALE_ORIGIN_BOTTOM:
			offset[0] = pmdl->scale_origin[0] - pmdl->scale[0] * xyfact;
			offset[1] = pmdl->scale_origin[1] - pmdl->scale[1] * xyfact;
			offset[2] = pmdl->scale_origin[2];
			break;
		case SCALE_ORIGIN_TOP:
			offset[0] = pmdl->scale_origin[0] - pmdl->scale[0] * xyfact;
			offset[1] = pmdl->scale_origin[1] - pmdl->scale[1] * xyfact;
			offset[2] = pmdl->scale_origin[2] - pmdl->scale[2] * zfact * 2.0f;
			break;
		default:
			break;
		}
	}

	if (entity->model->flags & EF_ROTATE)
	{
		/* The bob every pickup in the game does. */
		offset[2] += (float) sin (entity->origin[0] + entity->origin[1] +
					(cl.time * 3.0)) * 5.5f;
	}
}

/*
================
WGPUEntity_AliasAngles

EF_FACE_VIEW turns the model bodily towards the eye, EF_ROTATE spins it on a
world-position clock, and everything else just negates pitch.
================
*/
static void WGPUEntity_AliasAngles (entity_t *entity, vec3_t angles)
{
	if (entity->model->flags & EF_FACE_VIEW)
	{
		vec3_t	dir;
		float	forward, yaw, pitch;

		VectorSubtract (r_origin, entity->origin, dir);
		VectorNormalizeFast (dir);

		if (dir[1] == 0 && dir[0] == 0)
		{
			yaw = 0;
			pitch = (dir[2] > 0) ? 90.0f : 270.0f;
		}
		else
		{
			yaw = (float)(int)(atan2 (dir[1], dir[0]) * 180 / M_PI);
			if (yaw < 0)
				yaw += 360;
			forward = (float) sqrt (dir[0] * dir[0] + dir[1] * dir[1]);
			pitch = (float)(int)(atan2 (dir[2], forward) * 180 / M_PI);
			if (pitch < 0)
				pitch += 360;
		}

		angles[PITCH] = -pitch;
		angles[YAW] = yaw;
		angles[ROLL] = entity->angles[ROLL];
		return;
	}

	angles[ROLL] = entity->angles[ROLL];
	angles[PITCH] = -entity->angles[PITCH];
	if (entity->model->flags & EF_ROTATE)
		angles[YAW] = anglemod ((entity->origin[0] + entity->origin[1]) * 0.8f +
					(108 * cl.time));
	else
		angles[YAW] = entity->angles[YAW];
}

/*
================
WGPUEntity_DrawAliasModel
================
*/
static void WGPUEntity_DrawAliasModel (entity_t *entity, unsigned int extraflags)
{
	qmodel_t		*model = entity->model;
	aliashdr_t		*paliashdr;
	const newmdl_t		*pmdl;
	const trivertx_t	*poseverts;
	const stvert_t		*pstverts;
	const mtriangle_t	*ptri;
	wgpumodel_vertex_t	*out;
	vec3_t			mins, maxs, scale, offset, angles;
	vec3_t			forward, right, up, plightvec;
	float			alpha = 1.0f, radius;
	float			iw, ih;
	unsigned int		flags = extraflags;
	unsigned int		shade;
	unsigned int		lightcolor;
	int			i, pose, numtris, skin, alphabyte, pimp_flags;

	if (!model)
		return;

	radius = 0.0f;
	for (i = 0; i < 3; i++)
	{
		float	extent = q_max(fabsf(model->mins[i]), fabsf(model->maxs[i]));

		radius += extent * extent;
	}
	radius = sqrtf(radius);
	if (entity->scale && entity->scale > 100)
		radius *= (float)entity->scale / 100.0f;
	if (model->flags & EF_ROTATE)
		radius += 5.5f;
	for (i = 0; i < 3; i++)
	{
		mins[i] = entity->origin[i] - radius;
		maxs[i] = entity->origin[i] + radius;
	}
	if (entity != &cl.viewent && WGPU_CullBox (mins, maxs))
		return;

	paliashdr = (aliashdr_t *) Mod_Extradata (model);
	if (!paliashdr)
		return;
	pmdl = (const newmdl_t *)((const byte *)paliashdr + paliashdr->model);
	if (pmdl->skinwidth <= 0 || pmdl->skinheight <= 0 || pmdl->numtris <= 0)
		return;

	skin = WGPUEntity_AliasSkin (entity, paliashdr, pmdl);
	if (skin <= 0)
		return;

	if (entity->drawflags & DRF_TRANSLUCENT)
	{
		alpha = r_wateralpha.value;
		flags |= NITROMODEL_BLEND_ALPHA;
	}
	else if (model->flags & (EF_TRANSPARENT | EF_SPECIAL_TRANS))
	{
		alpha = 0.5f;
		flags |= NITROMODEL_BLEND_ALPHA;
	}
	else if (entity->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(entity->alpha))
	{
		alpha = ENTALPHA_DECODE(entity->alpha);
		flags |= NITROMODEL_BLEND_ALPHA;
	}
	if (model->flags & EF_SPECIAL_TRANS)
		flags = (flags & ~NITROMODEL_BLEND_ALPHA) | NITROMODEL_BLEND_ADD;
	alpha = CLAMP(0.0f, alpha, 1.0f);
	alphabyte = (int)(alpha * 255.0f + 0.5f);

	WGPUEntity_SetupLighting (entity);
	WGPUEntity_ApplyLightFloor ();
	lightcolor = WGPUEntity_PackLightColor ();
	pimp_flags = R_GetPimpFlags (entity, NULL);

	/* colorshade is a row of gfx/tinttab.lmp.  R_AliasDrawModel remaps the
	 * whole colormap through it; here the row travels with the vertex and
	 * the shader does the same lookup, so the tint stays an index remap
	 * instead of becoming an RGB multiply. */
	shade = ((unsigned int)(alphabyte & 255)) |
		(((unsigned int)entity->colorshade & 255u) << 8);
	if (pimp_flags & (XF_TORCH_GLOW | XF_GLOW | XF_MISSILE_GLOW | EF_GLOW))
		shade |= 1u << 16;

	pose = WGPUEntity_AliasPose (paliashdr, pmdl, entity);
	poseverts = (const trivertx_t *)((const byte *)paliashdr + pose);
	pstverts = (const stvert_t *)((const byte *)paliashdr + paliashdr->stverts);
	ptri = (const mtriangle_t *)((const byte *)paliashdr + paliashdr->triangles);
	numtris = pmdl->numtris;

	WGPUEntity_AliasScale (entity, pmdl, scale, offset);

	WGPUEntity_AliasAngles (entity, angles);
	AngleVectors (angles, forward, right, up);
	VectorInverse (right);	/* local +Y is left, as in R_AliasSetUpTransform */
	/* The shared volume supplies light travel in world space. Transform it
	 * into model space; right is already inverted above. */
	plightvec[0] = DotProduct (nitro_lightdir, forward);
	plightvec[1] = DotProduct (nitro_lightdir, right);
	plightvec[2] = DotProduct (nitro_lightdir, up);
	if ((model->flags & EF_ROTATE) ||
	    ((entity->drawflags & MLS_MASKIN) == MLS_ABSLIGHT))
		VectorClear (plightvec);

	iw = 1.0f / (float)pmdl->skinwidth;
	ih = 1.0f / (float)pmdl->skinheight;

	WGPUEntity_BeginBatch (skin, flags);
	out = WGPUEntity_Vertices (numtris * 3);

	for (i = 0; i < numtris; i++, ptri++)
	{
		int	j;

		for (j = 0; j < 3; j++)
		{
			const trivertx_t	*vert = &poseverts[ptri->vertindex[j]];
			const stvert_t		*st = &pstverts[ptri->stindex[j]];
			vec3_t			local;
			float			lightcos, darkness;

			local[0] = vert->v[0] * scale[0] + offset[0];
			local[1] = vert->v[1] * scale[1] + offset[1];
			local[2] = vert->v[2] * scale[2] + offset[2];

			out->position[0] = entity->origin[0] + local[0] * forward[0] +
					local[1] * right[0] + local[2] * up[0];
			out->position[1] = entity->origin[1] + local[0] * forward[1] +
					local[1] * right[1] + local[2] * up[1];
			out->position[2] = entity->origin[2] + local[0] * forward[2] +
					local[1] * right[2] + local[2] * up[2];

			/* The model loader stores alias UVs as 16.16 fixed
			 * point; normalise the fixed-point scale as well as the
			 * skin size or every coordinate lands on an integer. */
			out->texcoord[0] = st->s * (1.0f / 65536.0f) * iw;
			out->texcoord[1] = st->t * (1.0f / 65536.0f) * ih;

			/* d_polyse.c's llight, interpolated across the triangle
			 * by the rasteriser and here by the GPU: a colormap row,
			 * not a brightness. */
			lightcos = DotProduct (nitro_avertexnormals[vert->lightnormalindex],
						plightvec);
			darkness = (255.0f - nitro_ambientlight) * VID_GRADES;
			if (lightcos < 0.0f)
				darkness += nitro_shadelight * VID_GRADES * lightcos;
			darkness = CLAMP(0.0f, darkness, 255.0f * VID_GRADES);
			out->light = darkness * (1.0f / 256.0f);
			out->shade = shade;
			out->lightcolor = lightcolor;
			out++;
		}
	}

	c_alias_polys += numtris;
	wgpu_frame_polys += numtris;

	if (r_shadows.integer && !WGPULightVol_Active () && entity != &cl.viewent &&
	    !(pimp_flags & (XF_TORCH_GLOW | XF_GLOW | XF_MISSILE_GLOW | EF_GLOW)) &&
	    !(flags & (NITROMODEL_BLEND_ALPHA | NITROMODEL_BLEND_ADD)))
	{
		vec3_t	lightspot, sample, shadevector, shadowvector;
		float	an, shadowz;

		VectorCopy (entity->origin, sample);
		sample[2] += (model->mins[2] + model->maxs[2]) * 0.5f;
		/* The model foot is the fallback when no world surface lies below it. */
		VectorCopy (entity->origin, lightspot);
		lightspot[2] += model->mins[2];
		WGPUWorld_LightPoint (sample, lightspot);

		shadowz = lightspot[2];
		an = entity->angles[YAW] / 180.0f * M_PI;
		shadevector[0] = cos (-an);
		shadevector[1] = sin (-an);
		shadevector[2] = 1.0f;
		VectorNormalize (shadevector);
		shadowvector[0] = shadevector[0] * forward[0] +
			shadevector[1] * right[0] + shadevector[2] * up[0];
		shadowvector[1] = shadevector[0] * forward[1] +
			shadevector[1] * right[1] + shadevector[2] * up[1];
		shadowvector[2] = 0.0f;

		ptri = (const mtriangle_t *)((const byte *)paliashdr + paliashdr->triangles);
		WGPUEntity_BeginBatch (skin, NITROMODEL_SHADOW);
		out = WGPUEntity_Vertices (numtris * 3);
		for (i = 0; i < numtris; i++, ptri++)
		{
			int	j;

			for (j = 0; j < 3; j++)
			{
				const trivertx_t	*vert = &poseverts[ptri->vertindex[j]];
				vec3_t			local, world;
				float			height;

				local[0] = vert->v[0] * scale[0] + offset[0];
				local[1] = vert->v[1] * scale[1] + offset[1];
				local[2] = vert->v[2] * scale[2] + offset[2];
				world[0] = entity->origin[0] + local[0] * forward[0] +
					local[1] * right[0] + local[2] * up[0];
				world[1] = entity->origin[1] + local[0] * forward[1] +
					local[1] * right[1] + local[2] * up[1];
				world[2] = entity->origin[2] + local[0] * forward[2] +
					local[1] * right[2] + local[2] * up[2];
				height = world[2] - shadowz;
				out->position[0] = world[0] - shadowvector[0] * height;
				out->position[1] = world[1] - shadowvector[1] * height;
				out->position[2] = shadowz;
				out->texcoord[0] = out->texcoord[1] = 0.5f;
				out->light = -1.0f;
				out->shade = 96u << 24;
				out->lightcolor = 0xff404040u;
				out++;
			}
		}
		wgpu_frame_polys += numtris;
	}
}

static unsigned int WGPUEntity_GlowColor (const float *settings, float intensity)
{
	unsigned int	r, g, b, a;

	r = (unsigned int)(CLAMP(0.0f, settings[COLOR_R] * intensity, 1.0f) * 255.0f);
	g = (unsigned int)(CLAMP(0.0f, settings[COLOR_G] * intensity, 1.0f) * 255.0f);
	b = (unsigned int)(CLAMP(0.0f, settings[COLOR_B] * intensity, 1.0f) * 255.0f);
	a = (unsigned int)(CLAMP(0.0f, settings[COLOR_A], 1.0f) * 255.0f);
	return r | (g << 8) | (b << 16) | (a << 24);
}

static void WGPUEntity_DrawGlow (entity_t *entity)
{
	static const float	corners[6][2] = {
		{-1, -1}, {-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}
	};
	float		*settings;
	wgpumodel_vertex_t *out;
	vec3_t		origin, point, delta;
	float		radius, distance, intensity;
	unsigned int	color;
	int		flags, style, i, texture;
	byte		white = 15;

	flags = R_GetPimpFlags (entity, &settings);
	if (!((gl_glows.integer && (flags & XF_TORCH_GLOW)) ||
	      (gl_missile_glows.integer && (flags & XF_MISSILE_GLOW)) ||
	      (gl_other_glows.integer && (flags & (XF_GLOW | EF_GLOW)))))
		return;

	VectorCopy (entity->origin, origin);
	origin[0] += settings[ORB_OFFSET_X];
	origin[1] += settings[ORB_OFFSET_Y];
	origin[2] += settings[ORB_OFFSET_Z];
	if (flags & XF_TORCH_GLOW)
		origin[2] += (flags & XF_TORCH_GLOW_EGYPT) ? 16.0f : 8.0f;
	if ((entity->model->flags & EF_ROTATE) || (flags & EF_FLOAT))
		origin[2] += (float)sin (entity->origin[0] + entity->origin[1] +
					cl.time * 3.0) * 5.5f;

	radius = (settings[ORB_RADIUS] > 1.0f) ? settings[ORB_RADIUS] : 20.0f;
	VectorSubtract (origin, r_origin, delta);
	distance = VectorLengthFast (delta);
	if (distance <= radius)
		return;
	intensity = CLAMP(0.0f, distance / 1024.0f, 1.0f);
	style = (int)settings[LIGHT_STYLE];
	if (style < 0 || style >= MAX_LIGHTSTYLES)
		style = 0;
	intensity *= CLAMP(0.0f, d_lightstylevalue[style] / 255.0f, 1.0f);
	if (!(flags & XF_TORCH_GLOW))
		intensity *= CLAMP(0.0f, gl_glow_intensity.value, 1.0f);
	color = WGPUEntity_GlowColor (settings, intensity);

	texture = WGPUEntity_LoadSkin ("__nitro_glow", 1, 1, &white, 0, 1u);
	if (texture <= 0)
		return;
	WGPUEntity_BeginBatch (texture, NITROMODEL_GLOW);
	out = WGPUEntity_Vertices (6);
	for (i = 0; i < 6; i++)
	{
		VectorMA (origin, corners[i][0] * radius, vright, point);
		VectorMA (point, corners[i][1] * radius, vup, point);
		VectorCopy (point, out->position);
		out->texcoord[0] = corners[i][0] * 0.5f + 0.5f;
		out->texcoord[1] = corners[i][1] * 0.5f + 0.5f;
		out->light = -1.0f;
		out->shade = color;
		out->lightcolor = 0xff404040u;
		out++;
	}
	wgpu_frame_polys += 2;
}

/*
=============================================================================

	sprites

=============================================================================
*/

static const mspriteframe_t *WGPUEntity_SpriteFrame (entity_t *entity,
						const msprite_t *psprite,
						int *frame_out, int *subframe_out)
{
	const mspriteframedesc_t	*pframedesc;
	const mspritegroup_t		*pspritegroup;
	const float			*intervals;
	int				frame = entity->frame;
	int				i, numframes;
	float				fullinterval, targettime, time;

	if (frame >= psprite->numframes || frame < 0)
		frame = 0;
	*frame_out = frame;
	*subframe_out = 0;

	pframedesc = &psprite->frames[frame];
	if (pframedesc->type == SPR_SINGLE)
		return pframedesc->frameptr;

	pspritegroup = (const mspritegroup_t *)pframedesc->frameptr;
	intervals = pspritegroup->intervals;
	numframes = pspritegroup->numframes;
	if (numframes < 1)
		return NULL;
	fullinterval = intervals[numframes - 1];
	time = cl.time + entity->syncbase;
	targettime = time - ((int)(time / fullinterval)) * fullinterval;
	for (i = 0; i < numframes - 1; i++)
	{
		if (intervals[i] > targettime)
			break;
	}
	*subframe_out = i;
	return pspritegroup->frames[i];
}

/*
================
WGPUEntity_SpriteAxes

The five Quake sprite orientations, straight from R_DrawSpriteModel.  Hexen
II leans on all of them: flames are upright-facing, blood splats are
view-parallel, and beam segments are world-oriented.
================
*/
static qboolean WGPUEntity_SpriteAxes (entity_t *entity, const msprite_t *psprite,
					vec3_t up, vec3_t right)
{
	vec3_t	tvec;
	float	dot, angle, sr, cr;
	int	i;

	switch (psprite->type)
	{
	case SPR_FACING_UPRIGHT:
		VectorSubtract (entity->origin, r_origin, tvec);
		VectorNormalize (tvec);
		dot = tvec[2];
		if (dot > 0.999848f || dot < -0.999848f)
			return false;	/* degenerate: looking straight along it */
		up[0] = 0;
		up[1] = 0;
		up[2] = 1;
		right[0] = tvec[1];
		right[1] = -tvec[0];
		right[2] = 0;
		VectorNormalize (right);
		return true;

	case SPR_VP_PARALLEL:
		VectorCopy (vup, up);
		VectorCopy (vright, right);
		return true;

	case SPR_VP_PARALLEL_UPRIGHT:
		dot = vpn[2];
		if (dot > 0.999848f || dot < -0.999848f)
			return false;
		up[0] = 0;
		up[1] = 0;
		up[2] = 1;
		right[0] = vpn[1];
		right[1] = -vpn[0];
		right[2] = 0;
		VectorNormalize (right);
		return true;

	case SPR_ORIENTED:
	    {
		vec3_t	fwd;

		AngleVectors (entity->angles, fwd, right, up);
		return true;
	    }

	case SPR_VP_PARALLEL_ORIENTED:
		angle = entity->angles[ROLL] * (float)(M_PI * 2 / 360);
		sr = (float) sin (angle);
		cr = (float) cos (angle);
		for (i = 0; i < 3; i++)
		{
			right[i] = vright[i] * cr + vup[i] * sr;
			up[i] = vright[i] * -sr + vup[i] * cr;
		}
		return true;

	default:
		return false;
	}
}

/*
================
WGPUEntity_DrawSpriteModel

Two triangles built from the frame's own up/down/left/right offsets, so the
sprite's authored origin is preserved.  Sprites are unlit -- d_sprite.c
never touches the colormap -- so the vertex carries the unlit sentinel and
the shader reads the palette directly.
================
*/
static void WGPUEntity_DrawSpriteModel (entity_t *entity, unsigned int extraflags)
{
	qmodel_t		*model = entity->model;
	const msprite_t		*psprite;
	const mspriteframe_t	*frame;
	wgpumodel_vertex_t	*out;
	char			name[MAX_QPATH];
	vec3_t			up, right, point;
	float			alpha = 1.0f;
	unsigned int		texflags = NITROTEX_ALPHA;
	unsigned int		flags = extraflags | NITROMODEL_BLEND_ALPHA;
	unsigned int		shade;
	int			i, framenum, subframe, texture, alphabyte;
	static const float	corner_s[6] = { 0, 0, 1, 0, 1, 1 };
	static const float	corner_t[6] = { 1, 0, 0, 1, 0, 1 };

	if (!model || model->type != mod_sprite)
		return;

	/* Sprite payloads are hunk-owned despite occupying cache.data.  Sending
	 * one through Mod_Extradata would read the preceding hunk bytes as a
	 * cache header and corrupt the cache LRU. */
	psprite = (const msprite_t *) model->cache.data;
	if (!psprite)
		return;
	frame = WGPUEntity_SpriteFrame (entity, psprite, &framenum, &subframe);
	if (!frame || frame->width <= 0 || frame->height <= 0)
		return;

	if (model->flags & EF_HOLEY)
		texflags = NITROTEX_HOLEY;
	if (model->flags & EF_TRANSPARENT)
		alpha = 0.5f;
	if (entity->drawflags & DRF_TRANSLUCENT)
		alpha = r_wateralpha.value;
	if (entity->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(entity->alpha))
		alpha = ENTALPHA_DECODE(entity->alpha);
	if (model->flags & EF_SPECIAL_TRANS)
		flags = (flags & ~NITROMODEL_BLEND_ALPHA) | NITROMODEL_BLEND_ADD;
	alpha = CLAMP(0.0f, alpha, 1.0f);
	alphabyte = (int)(alpha * 255.0f + 0.5f);

	/* Grouped sprites animate within one entity frame, so the subframe has
	 * to be part of the cache key or the first one uploaded sticks. */
	q_snprintf (name, sizeof(name), "%s:%d:%d", model->name, framenum, subframe);
	texture = WGPUEntity_LoadSkin (name, frame->width, frame->height,
				frame->pixels, texflags, 1u);
	if (texture <= 0)
		return;

	if (!WGPUEntity_SpriteAxes (entity, psprite, up, right))
		return;

	shade = (unsigned int)(alphabyte & 255);

	WGPUEntity_BeginBatch (texture, flags);
	out = WGPUEntity_Vertices (6);

	for (i = 0; i < 6; i++)
	{
		float	s = corner_s[i];
		float	t = corner_t[i];
		float	xofs = (s > 0.5f) ? frame->right : frame->left;
		float	yofs = (t > 0.5f) ? frame->down : frame->up;

		VectorMA (entity->origin, yofs, up, point);
		VectorMA (point, xofs, right, point);

		out->position[0] = point[0];
		out->position[1] = point[1];
		out->position[2] = point[2];
		out->texcoord[0] = s;
		out->texcoord[1] = t;
		out->light = -1.0f;	/* unlit: no colormap, as d_sprite.c */
		out->shade = shade;
		out->lightcolor = 0xff404040u;
		out++;
	}

	wgpu_frame_polys += 2;
}

/*
=============================================================================

	entity lists

=============================================================================
*/

static qboolean WGPUEntity_IsTranslucent (const entity_t *entity)
{
	if (!entity->model)
		return false;
	if (entity->drawflags & DRF_TRANSLUCENT)
		return true;
	if (entity->model->flags & (EF_TRANSPARENT | EF_SPECIAL_TRANS))
		return true;
	if (entity->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(entity->alpha))
		return true;
	return entity->model->type == mod_sprite;
}

static void WGPUEntity_DrawEntity (entity_t *entity, unsigned int extraflags)
{
	switch (entity->model->type)
	{
	case mod_brush:
		/* Brush entities index the world's own vertex buffer, so they
		 * belong to the world pass, not the model arena. */
		WGPUWorld_DrawBrushEntity (entity);
		break;
	case mod_alias:
		WGPUEntity_DrawAliasModel (entity, extraflags);
		break;
	case mod_sprite:
		WGPUEntity_DrawSpriteModel (entity, extraflags);
		break;
	default:
		break;
	}
}

void WGPUEntity_DrawEntitiesOnList (qboolean translucent)
{
	int	i;

	if (!r_drawentities.integer)
		return;

	for (i = 0; i < cl_numvisedicts; i++)
	{
		entity_t	*entity = cl_visedicts[i];

		if (!entity || !entity->model)
			continue;
		if (entity == &cl.viewent)
			continue;
		if (WGPUEntity_IsTranslucent (entity) != translucent)
			continue;
		WGPUEntity_DrawEntity (entity, 0);
		WGPUEntity_DrawGlow (entity);
	}

	WGPUEntity_FlushBatch ();
}

/*
================
WGPUEntity_DrawViewModel

Drawn last, into the near 30% of the depth range, so the weapon cannot poke
through a wall the player is standing against.  WebGPU has no glDepthRange:
the range is a property of the viewport, so the batch carries a flag and the
backend sets minDepth/maxDepth around it.
================
*/
void WGPUEntity_DrawViewModel (void)
{
	entity_t	*entity = &cl.viewent;

	if (!entity->model)
		return;
	cl.light_level = WGPUEntity_ViewModelLightLevel (entity);
	if (cl.v.health <= 0 || chase_active.integer || !r_drawviewmodel.integer ||
	    !r_drawentities.integer || scr_viewsize.integer >= 140)
		return;

	WGPUEntity_DrawEntity (entity, NITROMODEL_VIEWMODEL);
	WGPUEntity_FlushBatch ();
}

/*
=============================================================================

	frame plumbing

=============================================================================
*/

void WGPUEntity_BeginScene (void)
{
	WGPUEntity_FlushBatch ();
	nitro_model_count = 0;
	nitro_model_batch_count = 0;
	nitro_model_opaque_batches = 0;
	nitro_batch_open = 0;
}

void WGPUEntity_EndOpaque (void)
{
	WGPUEntity_FlushBatch ();
	nitro_model_opaque_batches = nitro_model_batch_count;
}

void WGPUEntity_SceneData (wgpuscenedata_t *data)
{
	WGPUEntity_FlushBatch ();

	data->modelvertices = nitro_model_vertices;
	data->modelvertexcount = nitro_model_count;
	data->modelbatches = nitro_model_batches;
	data->modelbatchcount = nitro_model_batch_count;
	data->opaquemodelbatches = nitro_model_opaque_batches;

	if (nitro_model_count > 0)
		WebPerf_CountUpload ((size_t)nitro_model_count * sizeof(wgpumodel_vertex_t));
}

void WGPUEntity_NewMap (void)
{
	WGPUEntity_FlushSkins ();
	nitro_model_count = 0;
	nitro_model_batch_count = 0;
	nitro_model_opaque_batches = 0;
	nitro_batch_open = 0;
}

void WGPUEntity_Shutdown (void)
{
	WGPUEntity_FlushSkins ();

	free (nitro_model_vertices);
	free (nitro_model_batches);
	nitro_model_vertices = NULL;
	nitro_model_batches = NULL;
	nitro_model_capacity = 0;
	nitro_model_count = 0;
	nitro_model_batch_capacity = 0;
	nitro_model_batch_count = 0;
	nitro_model_opaque_batches = 0;
	nitro_batch_open = 0;
}
