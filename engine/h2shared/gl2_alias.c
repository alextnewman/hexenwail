/*
 * gl2_alias.c -- WebGlide models, sprites and particles.
 *
 * Everything here is transformed and lit on the CPU and handed to the GPU
 * as pre-lit textured triangles.  That is what Glide wanted -- the card
 * was a rasteriser, the CPU was the geometry engine -- and on one WASM
 * thread it is also simply the cheapest thing that works: no per-object
 * uniform churn, no shader permutations, one streamed vertex buffer and as
 * few draw calls as the batcher can get away with.
 *
 * The lighting is where the 90s show up.  Hexen II already ships coloured
 * dynamic lights (cl_dlights[].color) and per-entity colour shades
 * (entity_t::colorshade, the tinted spell and artifact effects), and both
 * feed a per-vertex Gouraud term through the quantised normal-dot tables.
 * The 8bpp software renderer collapses all of that to a grey ramp.
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
#include "web_perf.h"
#include "gl2_glide.h"

#define GL2_NUMVERTEXNORMALS	162

/* r_alias.c:26.  The software renderer floors a model's ambient light at
 * this before inverting it into a colormap row, so a software model's
 * darkness index caps at (255 - LIGHT_MIN) << VID_CBITS and can never reach
 * the darkest row.
 *
 * Do not mistake it for the floor that keeps software models readable.  It
 * is a floor in *colormap row* space -- row 62 of 64 -- and the rows below
 * it are hue, not brightness.  Carried across to WebGlide's linear multiply
 * it is 5/200 of the albedo, about 2.5%, five times darker than the world's
 * r_ambient floor and indistinguishable from black.  The real model floor
 * is r_ambient, applied in GL2_LightPoint where r_light.c:307 applies it;
 * this remains as the backstop for the light-style paths above, which never
 * consult the world at all. */
#define LIGHT_MIN		5.0f

/* r_main.c:806 and gl_rmain.c:3837, both of which say "always give some
 * light on gun".  The view model is the one thing on screen the player
 * cannot walk away from, so both renderers sample it at the entity origin
 * rather than the model's mid-point and floor it far above the world's
 * floor.  Neither number is negotiable art direction; they are what the
 * shipped game does. */
#define VIEWMODEL_LIGHT_MIN	24.0f

static const float	gl2_avertexnormals[GL2_NUMVERTEXNORMALS][3] =
{
#include "anorms.h"
};

/* Batch capacity: a comfortable multiple of the biggest model in the game
 * (MAXALIASVERTS 2000, three vertices per triangle after the CPU fan). */
#define GL2_MAX_BATCH_VERTS	(64 * 1024)

static gl2vertex_t	*gl2_batch_verts;
static int		gl2_batch_count;
static GLuint		gl2_model_vao;
static GLuint		gl2_model_vbo;
static int		gl2_model_vbo_offset;
static qboolean		gl2_model_vbo_orphaned;

static gl2texture_t	*gl2_batch_texture;
static unsigned int	gl2_batch_flags;
static float		gl2_batch_alpha;
static int		gl2_batch_blend;
static qboolean		gl2_batch_open;

/* Per-entity lighting, in 0..255 per channel plus the classic grey pair. */
static float		gl2_lightcolor[3];
static float		gl2_ambientlight;
static float		gl2_shadelight;

/*
=============================================================================

	batching

=============================================================================
*/

void GL2_ModelInit (void)
{
	if (!gl2_batch_verts)
	{
		gl2_batch_verts = (gl2vertex_t *)
			malloc ((size_t)GL2_MAX_BATCH_VERTS * sizeof(gl2vertex_t));
		if (!gl2_batch_verts)
			Sys_Error ("WebGlide: out of memory for the model batch");
	}
	gl2_batch_count = 0;
	gl2_batch_open = false;

	if (!gl2_model_vao)
		glGenVertexArrays (1, &gl2_model_vao);
	if (!gl2_model_vbo)
		glGenBuffers (1, &gl2_model_vbo);

	glBindVertexArray (gl2_model_vao);
	glBindBuffer (GL_ARRAY_BUFFER, gl2_model_vbo);
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE, sizeof(gl2vertex_t),
			(const void *)(uintptr_t)offsetof(gl2vertex_t, x));
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, sizeof(gl2vertex_t),
			(const void *)(uintptr_t)offsetof(gl2vertex_t, s));
	glEnableVertexAttribArray (3);
	glVertexAttribPointer (3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(gl2vertex_t),
			(const void *)(uintptr_t)offsetof(gl2vertex_t, color));
	glBindVertexArray (0);
	gl2_model_vbo_offset = 0;
	gl2_model_vbo_orphaned = false;
}

void GL2_ModelShutdown (void)
{
	if (gl2_model_vbo)
	{
		glDeleteBuffers (1, &gl2_model_vbo);
		gl2_model_vbo = 0;
	}
	if (gl2_model_vao)
	{
		glDeleteVertexArrays (1, &gl2_model_vao);
		gl2_model_vao = 0;
	}
	free (gl2_batch_verts);
	gl2_batch_verts = NULL;
	gl2_batch_count = 0;
	gl2_batch_open = false;
	gl2_model_vbo_offset = 0;
	gl2_model_vbo_orphaned = false;
}

static void GL2_ApplyBlendMode (int blend)
{
	switch (blend)
	{
	case GL2_BLEND_ADD:
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE);
		glDepthMask (GL_FALSE);
		break;
	case GL2_BLEND_ALPHA:
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask (GL_FALSE);
		break;
	default:
		glDisable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask (GL_TRUE);
		break;
	}
}

/*
================
GL2_FlushModelBatch

One upload into the frame's streaming buffer and one draw call per state
change.  Each batch gets a distinct range so the driver never has to wait
for a previous draw before accepting the next upload.
================
*/
void GL2_FlushModelBatch (void)
{
	const gl2program_t	*program = &gl2_model_program;

	if (!gl2_batch_count || !GL2_ShadersReady ())
	{
		gl2_batch_count = 0;
		gl2_batch_open = false;
		return;
	}

	glBindVertexArray (gl2_model_vao);
	glBindBuffer (GL_ARRAY_BUFFER, gl2_model_vbo);

	if (!gl2_model_vbo_orphaned ||
	    gl2_model_vbo_offset + gl2_batch_count > GL2_MAX_BATCH_VERTS)
	{
		glBufferData (GL_ARRAY_BUFFER,
			(GLsizeiptr)GL2_MAX_BATCH_VERTS * sizeof(gl2vertex_t),
			NULL, GL_STREAM_DRAW);
		gl2_model_vbo_offset = 0;
		gl2_model_vbo_orphaned = true;
	}
	glBufferSubData (GL_ARRAY_BUFFER,
		(GLintptr)gl2_model_vbo_offset * sizeof(gl2vertex_t),
		(GLsizeiptr)gl2_batch_count * sizeof(gl2vertex_t),
		gl2_batch_verts);

	GL2_UseProgram (program);
	glUniformMatrix4fv (program->u_mvp, 1, GL_FALSE, gl2_view_projection.m);
	glUniform1f (program->u_alpha, gl2_batch_alpha);
	glUniform1i (program->u_flags, (GLint)gl2_batch_flags);
	GL2_SetupSceneUniforms (program);

	if (gl2_batch_texture && (gl2_batch_texture->flags & GL2TEX_INDEXED))
	{
		GL2_Bind (0, GL2_WhiteTexture ());
		GL2_BindIndexed (2, gl2_batch_texture);
		glUniform1i (program->u_flags,
			(GLint)(gl2_batch_flags | GL2_MODELFLAG_INDEXED |
				((gl2_batch_texture->flags & GL2TEX_HOLEY) ?
				 GL2_MODELFLAG_HOLEY : 0) |
				((gl2_batch_texture->flags & GL2TEX_ALPHA) ?
				 GL2_MODELFLAG_ALPHA255 : 0)));
	}
	else
	{
		GL2_Bind (0, gl2_batch_texture);
		GL2_BindIndexed (2, NULL);
	}
	GL2_ApplyBlendMode (gl2_batch_blend);

	glDrawArrays (GL_TRIANGLES, gl2_model_vbo_offset, gl2_batch_count);
	WebPerf_CountDraw (gl2_batch_count / 3);
	gl2_frame_polys += gl2_batch_count / 3;
	gl2_frame_batches++;
	gl2_model_vbo_offset += gl2_batch_count;

	glBindVertexArray (0);
	gl2_batch_count = 0;
	gl2_batch_open = false;
}

void GL2_BeginModelBatch (gl2texture_t *texture, unsigned int shaderflags,
				float alpha, int blend)
{
	if (gl2_batch_open &&
	    (texture != gl2_batch_texture || shaderflags != gl2_batch_flags ||
	     alpha != gl2_batch_alpha || blend != gl2_batch_blend))
		GL2_FlushModelBatch ();

	gl2_batch_texture = texture;
	gl2_batch_flags = shaderflags;
	gl2_batch_alpha = alpha;
	gl2_batch_blend = blend;
	gl2_batch_open = true;
}

gl2vertex_t *GL2_ModelVertices (int count)
{
	gl2vertex_t	*out;

	if (count <= 0 || count > GL2_MAX_BATCH_VERTS || !gl2_batch_verts)
		return NULL;

	if (gl2_batch_count + count > GL2_MAX_BATCH_VERTS)
	{
		gl2texture_t	*texture = gl2_batch_texture;
		unsigned int	flags = gl2_batch_flags;
		float		alpha = gl2_batch_alpha;
		int		blend = gl2_batch_blend;

		GL2_FlushModelBatch ();
		GL2_BeginModelBatch (texture, flags, alpha, blend);
	}

	out = &gl2_batch_verts[gl2_batch_count];
	gl2_batch_count += count;
	return out;
}

void GL2_BeginModelFrame (void)
{
	gl2_batch_count = 0;
	gl2_batch_open = false;
	gl2_batch_texture = NULL;
	gl2_model_vbo_offset = 0;
	/* Defer the orphan until the first upload; empty frames need no VBO work. */
	gl2_model_vbo_orphaned = false;
}

void GL2_EndModelFrame (void)
{
	GL2_FlushModelBatch ();
	GL2_ApplyBlendMode (GL2_BLEND_OPAQUE);
}

/*
=============================================================================

	skins

=============================================================================
*/

/*
================
GL2_AliasSkinIndex

Skin groups animate on their own clock; single skins do not.  Mirrors
R_AliasSetupSkin without the software renderer's mip-map-far selection,
which the GPU's own mip chain replaces.
================
*/
static const maliasskindesc_t *GL2_AliasSkinDesc (const aliashdr_t *paliashdr,
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

/*
================
GL2_AliasSkin

Uploads a model's skin once and caches it under "<model>:<skin>".  A
player entity with its own colormap gets a translated copy of its own,
because a GL texture cannot be recoloured at draw time.
================
*/
/*
================
GL2_ColormapTag

A cheap FNV-1a over the 256-entry translation table, used to tell whether a
cached player skin is still the right colours.  Never returns 0, so it can
never match the zero-initialised tag of a texture nobody has tagged.
================
*/
static unsigned int GL2_ColormapTag (const byte *colormap)
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

static gl2texture_t *GL2_AliasSkin (entity_t *entity, const aliashdr_t *paliashdr,
					const newmdl_t *pmdl)
{
	char			name[MAX_QPATH];
	const maliasskindesc_t	*pskindesc;
	const byte		*pixels;
	gl2texture_t		*texture;
	unsigned int		flags = GL2TEX_MIPMAP | GL2TEX_INDEXED;
	int			skinnum, size, entnum;

	pskindesc = GL2_AliasSkinDesc (paliashdr, pmdl, entity, &skinnum);
	pixels = (const byte *)paliashdr + pskindesc->skin;
	size = pmdl->skinwidth * pmdl->skinheight;
	if (size <= 0)
		return NULL;

	if (entity->model->flags & EF_HOLEY)
		flags |= GL2TEX_HOLEY;
	if (entity->model->flags & (EF_TRANSPARENT | EF_SPECIAL_TRANS))
		flags |= GL2TEX_HOLEY;

	entnum = (int)(entity - cl_entities) - 1;
	if (entity->colormap && entity->colormap != vid.colormap &&
	    entnum >= 0 && entnum < cl.maxclients)
	{
		/* Player colours are a 256-entry translation of the skin. */
		unsigned int	tag = GL2_ColormapTag (entity->colormap);
		byte		*translated;
		int		i;

		q_snprintf (name, sizeof(name), "%s:player%d:%d",
				entity->model->name, entnum, skinnum);

		/* The name is stable across colour changes, so the cached
		 * copy is only good while the translation table behind it is
		 * unchanged.  Checking the 256-byte table first keeps the
		 * common case free of both the translate and the upload. */
		texture = GL2_FindTexture (name);
		if (texture && texture->content_tag == tag &&
		    texture->width == pmdl->skinwidth &&
		    texture->height == pmdl->skinheight)
			return texture;

		translated = (byte *) malloc ((size_t)size);
		if (translated)
		{
			for (i = 0; i < size; i++)
				translated[i] = entity->colormap[pixels[i]];
			pixels = translated;
			/* Tell the cache to re-specify over the existing GL
			 * object instead of handing back the old pixels. */
			flags |= GL2TEX_DYNAMIC;

			texture = GL2_LoadTexture (name, pmdl->skinwidth,
						pmdl->skinheight, pixels, flags);
			free (translated);
			if (texture)
				texture->content_tag = tag;
			return texture;
		}
	}

	q_snprintf (name, sizeof(name), "%s:%d", entity->model->name, skinnum);
	return GL2_LoadTexture (name, pmdl->skinwidth, pmdl->skinheight, pixels, flags);
}

/*
=============================================================================

	lighting

=============================================================================
*/

/*
================
GL2_SetupEntityLighting

The Hexen II model light styles come first -- they override the world
outright -- then the sampled lightmap colour, then every dynamic light in
range.  This is the GL renderer's rule set, kept because it is the one
Raven's own GL build shipped with.

The view model is the one exception both renderers make: it samples the
world at its own origin, not the model's mid-point, and floors the result
well above the world's floor.
================
*/
static void GL2_SetupEntityLighting (entity_t *entity)
{
	vec3_t	adjust, dist;
	int	lnum, mls, i;
	float	add, intensity;

	mls = entity->drawflags & MLS_MASKIN;

	if (entity->model && (entity->model->flags & EF_ROTATE))
	{
		/* Bonus items pulse rather than sample the world. */
		gl2_ambientlight = gl2_shadelight =
			60.0f + 34.0f + (float)sin (entity->origin[0] + entity->origin[1] +
						(cl.time * 3.8)) * 34.0f;
		gl2_lightcolor[0] = gl2_lightcolor[1] = gl2_lightcolor[2] = gl2_ambientlight;
		return;
	}
	if (mls == MLS_ABSLIGHT)
	{
		gl2_ambientlight = gl2_shadelight = (float)entity->abslight;
		gl2_lightcolor[0] = gl2_lightcolor[1] = gl2_lightcolor[2] = gl2_ambientlight;
		return;
	}
	if (mls != MLS_NONE)
	{
		gl2_ambientlight = gl2_shadelight = d_lightstylevalue[24 + mls] / 2.0f;
		gl2_lightcolor[0] = gl2_lightcolor[1] = gl2_lightcolor[2] = gl2_ambientlight;
		return;
	}

	VectorCopy (entity->origin, adjust);
	if (entity != &cl.viewent && entity->model)
		adjust[2] += (entity->model->mins[2] + entity->model->maxs[2]) * 0.5f;

	intensity = (float) GL2_LightPoint (adjust, gl2_lightcolor);
	gl2_ambientlight = gl2_shadelight = intensity;

	if (entity == &cl.viewent)
	{
		if (gl2_ambientlight < VIEWMODEL_LIGHT_MIN)
			gl2_ambientlight = gl2_shadelight = VIEWMODEL_LIGHT_MIN;
		for (i = 0; i < 3; i++)
		{
			if (gl2_lightcolor[i] < VIEWMODEL_LIGHT_MIN)
				gl2_lightcolor[i] = VIEWMODEL_LIGHT_MIN;
		}
	}

	if (r_dynamic.integer)
	{
		for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
		{
			const dlight_t	*light = &cl_dlights[lnum];
			float		color[3];

			if (light->die < cl.time || !light->radius)
				continue;
			VectorSubtract (entity->origin, light->origin, dist);
			add = light->radius - VectorLengthFast (dist);
			if (add <= 0)
				continue;

			if (light->color[0] <= 0.0f && light->color[1] <= 0.0f &&
			    light->color[2] <= 0.0f)
			{
				color[0] = color[1] = color[2] = 1.0f;
			}
			else
			{
				for (i = 0; i < 3; i++)
					color[i] = light->color[i];
			}

			if (light->dark)
			{
				gl2_ambientlight -= add;
				for (i = 0; i < 3; i++)
					gl2_lightcolor[i] -= color[i] * add;
			}
			else
			{
				gl2_ambientlight += add;
				for (i = 0; i < 3; i++)
					gl2_lightcolor[i] += color[i] * add;
			}
		}
	}

	if (gl2_ambientlight < 0.0f)
		gl2_ambientlight = 0.0f;
	for (i = 0; i < 3; i++)
	{
		if (gl2_lightcolor[i] < 0.0f)
			gl2_lightcolor[i] = 0.0f;
	}

	/* The darkness row always follows the software renderer's clamp. The
	 * overbright cvar may retain extra coloured-light chroma, but must not
	 * bypass the authored colormap transfer function. */
	if (gl2_ambientlight > 128.0f)
		gl2_ambientlight = 128.0f;
	if (gl2_ambientlight + gl2_shadelight > 192.0f)
		gl2_shadelight = 192.0f - gl2_ambientlight;
	if (!gl_overbright_models.integer)
	{
		for (i = 0; i < 3; i++)
		{
			if (gl2_lightcolor[i] > 192.0f)
				gl2_lightcolor[i] = 192.0f;
		}
	}
}

/*
================
GL2_ApplyAliasLightFloor

R_AliasSetupLighting applies LIGHT_MIN on the single path every software
model goes through, after the light styles have been resolved.  The GL
rule set returns early for EF_ROTATE and the MLS styles, so the floor has
to be applied here, at the one call site, to cover them all.

gl2_lightcolor is what actually reaches the shader, so flooring only
gl2_ambientlight would be inert.
================
*/
static void GL2_ApplyAliasLightFloor (void)
{
	int	i;

	if (gl2_ambientlight < LIGHT_MIN)
		gl2_ambientlight = LIGHT_MIN;
	for (i = 0; i < 3; i++)
	{
		if (gl2_lightcolor[i] < LIGHT_MIN)
			gl2_lightcolor[i] = LIGHT_MIN;
	}
}

/*
================
GL2_ApplyColorShade

entity_t::colorshade indexes Hexen II's 16x16 tint table: the coloured
glows the QC puts on spells, artifacts and powered-up weapons.  This is
period colour that the 8bpp path can only approximate through the
translucency tables.
================
*/
static void GL2_ApplyColorShade (const entity_t *entity, float *rgb)
{
	int	cs = entity->colorshade;

	if (!cs)
		return;
	rgb[0] *= RTint[cs];
	rgb[1] *= GTint[cs];
	rgb[2] *= BTint[cs];
}

/*
=============================================================================

	alias models

=============================================================================
*/

/*
================
GL2_AliasPose

The pose a frame resolves to right now.  Frame groups run on their own
interval table, exactly as the software renderer reads them.
================
*/
static int GL2_AliasPose (const aliashdr_t *paliashdr, const newmdl_t *pmdl,
				entity_t *entity)
{
	const maliasframedesc_t	*pframedesc;
	int			frame = entity->frame;

	if (frame >= pmdl->numframes || frame < 0)
		frame = 0;
	pframedesc = &paliashdr->frames[frame];

	if (pframedesc->type == ALIAS_SINGLE)
		return pframedesc->frame;

	{
		const maliasgroup_t	*group = (const maliasgroup_t *)
				((const byte *)paliashdr + pframedesc->frame);
		const float	*intervals = (const float *)
				((const byte *)paliashdr + group->intervals);
		int		i, numframes = group->numframes;
		float		fullinterval, targettime, time;

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
}

/*
================
GL2_AliasScaleMatrix

Hexen II scales models per entity, with three scale types and three
scaling origins.  Reproduced from the GL renderer so scaled monsters,
shrunken players and growing effects land in the same place.
================
*/
static void GL2_AliasScaleMatrix (entity_t *entity, const newmdl_t *pmdl,
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
GL2_AliasAngles

Mirrors R_AliasSetUpTransform: EF_FACE_VIEW turns the model bodily
towards the eye, EF_ROTATE spins it on a world-position clock, and
everything else just negates pitch.
================
*/
static void GL2_AliasAngles (entity_t *entity, vec3_t angles)
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
GL2_DrawAliasModel
================
*/
void GL2_DrawAliasModel (entity_t *entity)
{
	qmodel_t		*model = entity->model;
	aliashdr_t		*paliashdr;
	const newmdl_t		*pmdl;
	const trivertx_t	*poseverts;
	const stvert_t		*pstverts;
	const mtriangle_t	*ptri;
	gl2texture_t		*skin;
	gl2vertex_t		*out;
	vec3_t			mins, maxs, scale, offset, angles;
	vec3_t			forward, right, up, plightvec;
	float			alpha = 1.0f;
	float			iw, ih;
	unsigned int		shaderflags = 0;
	int			blend = GL2_BLEND_OPAQUE;
	int			i, pose, numtris;
	float			tint[3], tintmax;

	if (!model || !GL2_ShadersReady ())
		return;

	VectorAdd (entity->origin, model->mins, mins);
	VectorAdd (entity->origin, model->maxs, maxs);
	if (entity != &cl.viewent && GL2_CullBox (mins, maxs))
		return;

	paliashdr = (aliashdr_t *) Mod_Extradata (model);
	if (!paliashdr)
		return;
	pmdl = (const newmdl_t *)((const byte *)paliashdr + paliashdr->model);
	if (pmdl->skinwidth <= 0 || pmdl->skinheight <= 0 || pmdl->numtris <= 0)
		return;

	skin = GL2_AliasSkin (entity, paliashdr, pmdl);
	if (!skin)
		return;

	GL2_SetupEntityLighting (entity);
	GL2_ApplyAliasLightFloor ();
	VectorCopy (gl2_lightcolor, tint);
	GL2_ApplyColorShade (entity, tint);

	/* Colour is layered around the colormap result in the shader. Keep only
	 * chroma here so a neutral light leaves the authored ramp untouched. */
	tintmax = q_max(tint[0], q_max(tint[1], tint[2]));
	if (tintmax > 0.0f)
		VectorScale (tint, 1.0f / tintmax, tint);
	else
		VectorSet (tint, 1.0f, 1.0f, 1.0f);

	if (entity->drawflags & DRF_TRANSLUCENT)
	{
		alpha = r_wateralpha.value;
		blend = GL2_BLEND_ALPHA;
	}
	else if (model->flags & (EF_TRANSPARENT | EF_SPECIAL_TRANS))
	{
		alpha = 0.5f;
		blend = GL2_BLEND_ALPHA;
	}
	else if (entity->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(entity->alpha))
	{
		alpha = ENTALPHA_DECODE(entity->alpha);
		blend = GL2_BLEND_ALPHA;
	}
	if (model->flags & EF_HOLEY)
		shaderflags |= GL2_MODELFLAG_ALPHATEST;
	if (alpha < 0.0f)
		alpha = 0.0f;
	else if (alpha > 1.0f)
		alpha = 1.0f;

	pose = GL2_AliasPose (paliashdr, pmdl, entity);
	poseverts = (const trivertx_t *)((const byte *)paliashdr + pose);
	pstverts = (const stvert_t *)((const byte *)paliashdr + paliashdr->stverts);
	ptri = (const mtriangle_t *)((const byte *)paliashdr + paliashdr->triangles);
	numtris = pmdl->numtris;

	GL2_AliasScaleMatrix (entity, pmdl, scale, offset);

	GL2_AliasAngles (entity, angles);
	AngleVectors (angles, forward, right, up);
	VectorInverse (right);	/* local +Y is left, as in R_AliasSetUpTransform */
	/* R_AliasSetupLighting uses the fixed world light vector {-1,0,0}
	 * transformed into model space. right is already inverted above. */
	plightvec[0] = -forward[0];
	plightvec[1] = -right[0];
	plightvec[2] = -up[0];
	if ((model->flags & EF_ROTATE) ||
	    ((entity->drawflags & MLS_MASKIN) == MLS_ABSLIGHT))
		VectorClear (plightvec);

	iw = 1.0f / (float)pmdl->skinwidth;
	ih = 1.0f / (float)pmdl->skinheight;

	GL2_BeginModelBatch (skin, shaderflags, alpha, blend);
	out = GL2_ModelVertices (numtris * 3);
	if (!out)
		return;

	for (i = 0; i < numtris; i++, ptri++)
	{
		int	j;

		for (j = 0; j < 3; j++)
		{
			const trivertx_t	*vert = &poseverts[ptri->vertindex[j]];
			const stvert_t		*st = &pstverts[ptri->stindex[j]];
			vec3_t			local, world;
			float			lightcos, darkness;
			int			c;

			local[0] = vert->v[0] * scale[0] + offset[0];
			local[1] = vert->v[1] * scale[1] + offset[1];
			local[2] = vert->v[2] * scale[2] + offset[2];

			world[0] = entity->origin[0] + local[0] * forward[0] +
					local[1] * right[0] + local[2] * up[0];
			world[1] = entity->origin[1] + local[0] * forward[1] +
					local[1] * right[1] + local[2] * up[1];
			world[2] = entity->origin[2] + local[0] * forward[2] +
					local[1] * right[2] + local[2] * up[2];

			out->x = world[0];
			out->y = world[1];
			out->z = world[2];
			/* The model loader stores alias UVs as 16.16 fixed point.
			 * Normalise both the fixed-point scale and the skin size;
			 * omitting the former leaves integer coordinates whose
			 * fractional part is always zero in the indexed sampler. */
			out->s = st->s * (1.0f / 65536.0f) * iw;
			out->t = st->t * (1.0f / 65536.0f) * ih;

			lightcos = DotProduct (gl2_avertexnormals[vert->lightnormalindex],
						plightvec);
			darkness = (255.0f - gl2_ambientlight) * VID_GRADES;
			if (lightcos < 0.0f)
				darkness += gl2_shadelight * VID_GRADES * lightcos;
			if (darkness < 0.0f)
				darkness = 0.0f;
			else if (darkness > 255.0f * VID_GRADES)
				darkness = 255.0f * VID_GRADES;
			for (c = 0; c < 3; c++)
			{
				int	value = (int)(tint[c] * 255.0f);

				if (value < 0)
					value = 0;
				else if (value > 255)
					value = 255;
				out->color[c] = (byte)value;
			}
			out->color[3] = (byte)((int)darkness >> VID_CBITS);
			out++;
		}
	}
}

/*
=============================================================================

	sprites

=============================================================================
*/

static const mspriteframe_t *GL2_SpriteFrame (entity_t *entity, const msprite_t *psprite,
						int *frame_out, int *subframe_out)
{
	const mspriteframedesc_t	*pframedesc;
	const mspritegroup_t		*pspritegroup;
	int				frame = entity->frame;
	int				i, numframes;
	const float			*intervals;
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
GL2_SpriteAxes

The five Quake sprite orientations, straight from R_DrawSpriteModel.
Hexen II leans on all of them: flames are upright-facing, blood splats
are view-parallel, and beam segments are world-oriented.
================
*/
static qboolean GL2_SpriteAxes (entity_t *entity, const msprite_t *psprite,
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
GL2_DrawSpriteModel

Two triangles built from the frame's own up/down/left/right offsets, so
the sprite's authored origin is preserved.  Wound clockwise to match the
GLQuake convention this renderer inherits (glCullFace GL_FRONT).
================
*/
void GL2_DrawSpriteModel (entity_t *entity)
{
	qmodel_t		*model = entity->model;
	const msprite_t		*psprite;
	const mspriteframe_t	*frame;
	gl2texture_t		*texture;
	gl2vertex_t		*out;
	char			name[MAX_QPATH];
	vec3_t			up, right, point;
	float			alpha = 1.0f;
	unsigned int		flags = GL2TEX_ALPHA;
	int			blend = GL2_BLEND_ALPHA;
	int			i, framenum, subframe;
	static const float	corner_s[6] = { 0, 0, 1, 0, 1, 1 };
	static const float	corner_t[6] = { 1, 0, 0, 1, 0, 1 };

	if (!model || model->type != mod_sprite || !GL2_ShadersReady ())
		return;

	/* Sprite payloads are hunk-owned, despite occupying cache.data.  Sending
	 * one through Mod_Extradata would interpret the preceding hunk bytes as a
	 * cache header and corrupt the cache LRU. */
	psprite = (const msprite_t *) model->cache.data;
	if (!psprite)
		return;
	frame = GL2_SpriteFrame (entity, psprite, &framenum, &subframe);
	if (!frame || frame->width <= 0 || frame->height <= 0)
		return;

	if (model->flags & EF_HOLEY)
		flags = GL2TEX_HOLEY;
	if (model->flags & EF_TRANSPARENT)
		alpha = 0.5f;
	if (entity->drawflags & DRF_TRANSLUCENT)
		alpha = r_wateralpha.value;
	if (entity->alpha != ENTALPHA_DEFAULT && !ENTALPHA_OPAQUE(entity->alpha))
		alpha = ENTALPHA_DECODE(entity->alpha);
	if (model->flags & EF_SPECIAL_TRANS)
		blend = GL2_BLEND_ADD;

	/* Grouped sprites animate within one entity frame, so the subframe has
	 * to be part of the cache key or the first one uploaded sticks. */
	q_snprintf (name, sizeof(name), "%s:%d:%d", model->name, framenum, subframe);
	texture = GL2_LoadTexture (name, frame->width, frame->height,
				frame->pixels, flags | GL2TEX_CLAMP);
	if (!texture)
		return;

	if (!GL2_SpriteAxes (entity, psprite, up, right))
		return;

	GL2_BeginModelBatch (texture, GL2_MODELFLAG_ALPHATEST, alpha, blend);
	out = GL2_ModelVertices (6);
	if (!out)
		return;

	for (i = 0; i < 6; i++)
	{
		float	s = corner_s[i];
		float	t = corner_t[i];
		float	xofs = (s > 0.5f) ? frame->right : frame->left;
		float	yofs = (t > 0.5f) ? frame->down : frame->up;

		VectorMA (entity->origin, yofs, up, point);
		VectorMA (point, xofs, right, point);

		out->x = point[0];
		out->y = point[1];
		out->z = point[2];
		out->s = s;
		out->t = t;
		out->color[0] = out->color[1] = out->color[2] = out->color[3] = 255;
		out++;
	}
}

/*
=============================================================================

	particles

	One camera-facing quad each, drawn with the soft round dot the
	texture manager generates.  Hardware points would be cheaper and
	uglier; every accelerator of the period drew particles as textured
	quads for exactly this reason.

=============================================================================
*/

void GL2_DrawParticleList (particle_t *first)
{
	const particle_t	*particle;
	gl2texture_t		*texture;
	vec3_t			up, right;
	float			scale_base;

	if (!first || !gl_particles.integer || !GL2_ShadersReady ())
		return;

	texture = GL2_ParticleTexture ();
	if (!texture)
		return;

	VectorScale (vup, 1.5f, up);
	VectorScale (vright, 1.5f, right);
	scale_base = 1.0f;

	GL2_BeginModelBatch (texture, GL2_MODELFLAG_NOFOG, 1.0f, GL2_BLEND_ADD);

	for (particle = first; particle; particle = particle->next)
	{
		gl2vertex_t	*out;
		unsigned int	rgba;
		vec3_t			delta;
		float		scale;
		byte		color[4];
		int		i;
		static const float	corner_x[6] = { 0, 0, 1, 0, 1, 1 };
		static const float	corner_y[6] = { 0, 1, 1, 0, 1, 0 };

		rgba = d_8to24table[(int)particle->color & 255];
		color[0] = (byte)(rgba & 0xff);
		color[1] = (byte)((rgba >> 8) & 0xff);
		color[2] = (byte)((rgba >> 16) & 0xff);
		color[3] = 255;

		/* Hold a roughly constant screen size, the way the software
		 * renderer's distance-scaled dot does. */
		VectorSubtract (particle->org, r_origin, delta);
		scale = scale_base + DotProduct (delta, vpn) * 0.004f;
		if (scale < 1.0f)
			scale = 1.0f;
		else if (scale > 12.0f)
			scale = 12.0f;

		out = GL2_ModelVertices (6);
		if (!out)
			return;

		for (i = 0; i < 6; i++)
		{
			vec3_t	point;

			VectorCopy (particle->org, point);
			VectorMA (point, (corner_y[i] - 0.5f) * scale, up, point);
			VectorMA (point, (corner_x[i] - 0.5f) * scale, right, point);

			out->x = point[0];
			out->y = point[1];
			out->z = point[2];
			out->s = corner_x[i];
			out->t = corner_y[i];
			out->color[0] = color[0];
			out->color[1] = color[1];
			out->color[2] = color[2];
			out->color[3] = color[3];
			out++;
		}
	}

	GL2_FlushModelBatch ();
}
