/*
 * gl2_texture.c -- WebGlide texture manager.
 *
 * Everything expensive happens here, once, at load time: palette
 * expansion, alpha fringe repair, fullbright masks and mip generation.  The
 * frame loop only ever binds.
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

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT		0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT	0x84FF
#endif

#define GL2_TEXTURE_HASH_SIZE	256

typedef struct
{
	const char	*name;
	GLint		minfilter;
	GLint		magfilter;
	qboolean	mipmapped;
} gl2filtermode_t;

static const gl2filtermode_t gl2_filter_modes[] = {
	{ "GL_NEAREST",                GL_NEAREST,                GL_NEAREST, false },
	{ "GL_NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST, true  },
	{ "GL_NEAREST_MIPMAP_LINEAR",  GL_NEAREST_MIPMAP_LINEAR,  GL_NEAREST, true  },
	{ "GL_LINEAR",                 GL_LINEAR,                 GL_LINEAR,  false },
	{ "GL_LINEAR_MIPMAP_NEAREST",  GL_LINEAR_MIPMAP_NEAREST,  GL_LINEAR,  true  },
	{ "GL_LINEAR_MIPMAP_LINEAR",   GL_LINEAR_MIPMAP_LINEAR,   GL_LINEAR,  true  },
};

#define NUM_GL2_FILTER_MODES	((int)(sizeof(gl2_filter_modes) / sizeof(gl2_filter_modes[0])))

static gl2texture_t	gl2_textures[WEB_MAX_TEXTURES];
static int		gl2_numtextures;
static gl2texture_t	*gl2_texture_hash[GL2_TEXTURE_HASH_SIZE];

static gl2texture_t	*gl2_particle_texture;
static gl2texture_t	*gl2_white_texture;
static gl2texture_t	*gl2_black_texture;

static GLuint		gl2_bound[4];

static unsigned int	*gl2_scratch;
static int		gl2_scratch_texels;

/*
================
GL2_ProbeCaps

What this context will actually do for us.  Nothing else in the web build
fills gl_renderer_caps in -- gl_vidsdl.c is a desktop file and is not
compiled -- so without this the anisotropy path below is dead and
gl_glide_anisotropy silently does nothing.
================
*/
static void GL2_ProbeCaps (void)
{
	GLint	count = 0, i;
	GLfloat	max_aniso = 0.0f;

	memset (&gl_renderer_caps, 0, sizeof(gl_renderer_caps));
	gl_max_anisotropy = 1;

	glGetIntegerv (GL_NUM_EXTENSIONS, &count);
	for (i = 0; i < count; i++)
	{
		const char	*name = (const char *) glGetStringi (GL_EXTENSIONS, (GLuint)i);

		/* Both spellings define GL_TEXTURE_MAX_ANISOTROPY at the same
		 * value; anything else that merely reads alike does not. */
		if (!name || (!strstr (name, "EXT_texture_filter_anisotropic") &&
			      !strstr (name, "ARB_texture_filter_anisotropic")))
			continue;
		glGetFloatv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso);
		if (max_aniso >= 2.0f)
		{
			gl_renderer_caps.anisotropy = true;
			gl_max_anisotropy = (int) max_aniso;
		}
		break;
	}

	Con_DPrintf ("WebGlide: anisotropic filtering %s (max %d)\n",
		gl_renderer_caps.anisotropy ? "available" : "unavailable",
		gl_max_anisotropy);
}

/*
================
GL2_TextureHash
================
*/
static unsigned int GL2_TextureHash (const char *name)
{
	unsigned int	hash = 5381;

	while (*name)
		hash = ((hash << 5) + hash) ^ (unsigned char)(*name++);
	return hash & (GL2_TEXTURE_HASH_SIZE - 1);
}

/*
================
GL2_Scratch

One growable staging buffer for the RGBA expansion.  Textures are loaded
one at a time, so one buffer is enough.
================
*/
static unsigned int *GL2_Scratch (int texels)
{
	if (texels > gl2_scratch_texels)
	{
		free (gl2_scratch);
		gl2_scratch = (unsigned int *) malloc ((size_t)texels * sizeof(unsigned int));
		if (!gl2_scratch)
		{
			gl2_scratch_texels = 0;
			Sys_Error ("WebGlide: out of memory for a %d texel texture", texels);
		}
		gl2_scratch_texels = texels;
	}
	return gl2_scratch;
}

/*
================
GL2_RepairAlphaFringe

Transparent texels carry the palette's colour for index 0 or 255, which is
usually black.  Bilinear filtering and mip generation both average that
into the visible edge and leave a dark halo around every grate, torch and
tree.  Give the invisible texels the average colour of the visible ones
instead; the alpha channel still cuts them out.
================
*/
static void GL2_RepairAlphaFringe (unsigned int *rgba, int texels)
{
	int	i, opaque = 0;
	unsigned int	fill;
	unsigned int	r = 0, g = 0, b = 0;
	byte	*pixel;

	for (i = 0; i < texels; i++)
	{
		pixel = (byte *)&rgba[i];
		if (pixel[3] == 0)
			continue;
		r += pixel[0];
		g += pixel[1];
		b += pixel[2];
		opaque++;
	}

	if (!opaque || opaque == texels)
		return;

	fill = (r / opaque) | ((g / opaque) << 8) | ((b / opaque) << 16);
	for (i = 0; i < texels; i++)
	{
		pixel = (byte *)&rgba[i];
		if (pixel[3] == 0)
			rgba[i] = fill;
	}
}

/*
================
GL2_ExpandPalette
================
*/
static unsigned int *GL2_ExpandPalette (const byte *data, int texels, unsigned int flags)
{
	unsigned int	*rgba = GL2_Scratch (texels);
	qboolean	has_alpha = false;
	int		i;

	for (i = 0; i < texels; i++)
	{
		byte	index = data[i];

		if (((flags & GL2TEX_ALPHA) && index == 255) ||
		    ((flags & GL2TEX_HOLEY) && index == 0))
		{
			rgba[i] = 0;
			has_alpha = true;
		}
		else
		{
			rgba[i] = d_8to24table[index] | 0xff000000u;
		}
	}

	if (has_alpha)
		GL2_RepairAlphaFringe (rgba, texels);
	return rgba;
}

/*
================
GL2_SetFilters

Mip filters only make sense on a texture that has mips; sky layers and the
particle dot are deliberately unfiltered or clamped.
================
*/
static void GL2_SetFilters (const gl2texture_t *texture)
{
	const gl2filtermode_t	*mode;
	GLint			wrap;
	float			aniso;

	if (gl_filter_idx < 0 || gl_filter_idx >= NUM_GL2_FILTER_MODES)
		gl_filter_idx = NUM_GL2_FILTER_MODES - 1;
	mode = &gl2_filter_modes[gl_filter_idx];

	if (texture->flags & GL2TEX_NEAREST)
	{
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			(texture->flags & GL2TEX_MIPMAP) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else if (texture->flags & GL2TEX_MIPMAP)
	{
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode->minfilter);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode->magfilter);
	}
	else
	{
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode->magfilter);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode->magfilter);
	}

	wrap = (texture->flags & GL2TEX_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);

	if (gl_renderer_caps.anisotropy && (texture->flags & GL2TEX_MIPMAP))
	{
		aniso = gl_glide_anisotropy.value;
		if (aniso < 1.0f)
			aniso = 1.0f;
		if (aniso > (float)gl_max_anisotropy)
			aniso = (float)gl_max_anisotropy;
		glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
	}
}

/*
================
GL2_BuildFullbrightMask

The self-lit companion of a palette texture: the texels whose palette index
is at or above vid.fullbright (224 in the stock colormap, and the reason a
torch stays lit in a pitch-black room) kept at full colour, everything else
black.  The world and model shaders add it on top of the lit surface, so
black costs nothing and the mip chain fades the glow out with distance.

Nothing is allocated for a texture that has no such texels, which is most
of them.
================
*/
static void GL2_BuildFullbrightMask (gl2texture_t *texture, const byte *data)
{
	int		texels = texture->width * texture->height;
	int		threshold = vid.fullbright;
	unsigned int	*rgba;
	qboolean	any = false;
	int		i;

	/* The same guard as the desktop's Mod_LoadFullbrightTexture: a
	 * colormap that does not describe a fullbright range (256 means
	 * "none") gets no mask rather than an invented threshold. */
	if (threshold < 1 || threshold > 255)
	{
		if (texture->fullbright)
		{
			glDeleteTextures (1, &texture->fullbright);
			texture->fullbright = 0;
		}
		return;
	}

	for (i = 0; i < texels; i++)
	{
		/* 255 is the transparency index, never a light source. */
		if (data[i] >= threshold && data[i] != 255)
		{
			any = true;
			break;
		}
	}

	if (!any)
	{
		if (texture->fullbright)
		{
			glDeleteTextures (1, &texture->fullbright);
			texture->fullbright = 0;
		}
		return;
	}

	rgba = GL2_Scratch (texels);
	for (i = 0; i < texels; i++)
	{
		if (data[i] >= threshold && data[i] != 255)
			rgba[i] = d_8to24table[data[i]] | 0xff000000u;
		else
			rgba[i] = 0xff000000u;
	}

	if (!texture->fullbright)
		glGenTextures (1, &texture->fullbright);
	glActiveTexture (GL_TEXTURE0);
	gl2_bound[0] = texture->fullbright;
	glBindTexture (GL_TEXTURE_2D, texture->fullbright);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, texture->width, texture->height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	if (texture->flags & GL2TEX_MIPMAP)
		glGenerateMipmap (GL_TEXTURE_2D);
	GL2_SetFilters (texture);
}

/*
================
GL2_FindTexture
================
*/
gl2texture_t *GL2_FindTexture (const char *name)
{
	gl2texture_t	*texture;

	if (!name || !name[0])
		return NULL;
	for (texture = gl2_texture_hash[GL2_TextureHash (name)]; texture; texture = texture->next)
	{
		if (!strcmp (texture->name, name))
			return texture;
	}
	return NULL;
}

/*
================
GL2_LoadTexture

Uploads 8bpp palette data (or RGBA with GL2TEX_RGBA) and returns a handle.
A second request for the same name and shape reuses the existing texture,
which is what makes brush models and animated textures cheap.

The cache key is the name, so a caller whose pixels change under a fixed
name -- a translated player skin -- must pass GL2TEX_DYNAMIC to get the
new content uploaded over the existing GL object.
================
*/
gl2texture_t *GL2_LoadTexture (const char *name, int width, int height,
				const byte *data, unsigned int flags)
{
	gl2texture_t	*texture;
	unsigned int	*rgba;
	unsigned int	bucket;

	if (width <= 0 || height <= 0 || !data)
		return NULL;

	texture = GL2_FindTexture (name);
	if (texture)
	{
		if (texture->width != width || texture->height != height ||
		    texture->flags != flags)
		{
			/* Same name, different shape: reload in place. */
			glDeleteTextures (1, &texture->id);
			texture->id = 0;
		}
		else if (!(flags & GL2TEX_DYNAMIC))
			return texture;
		/* else: volatile content, re-specified over the same GL name. */
	}
	else
	{
		if (gl2_numtextures >= WEB_MAX_TEXTURES)
		{
			Con_Printf ("WebGlide: out of texture slots (%d)\n", WEB_MAX_TEXTURES);
			return NULL;
		}
		texture = &gl2_textures[gl2_numtextures++];
		memset (texture, 0, sizeof(*texture));
		if (name)
			q_strlcpy (texture->name, name, sizeof(texture->name));
		bucket = GL2_TextureHash (texture->name);
		texture->next = gl2_texture_hash[bucket];
		gl2_texture_hash[bucket] = texture;
	}

	texture->width = width;
	texture->height = height;
	texture->flags = flags;

	if (flags & GL2TEX_RGBA)
	{
		rgba = GL2_Scratch (width * height);
		memcpy (rgba, data, (size_t)width * height * 4);
		GL2_RepairAlphaFringe (rgba, width * height);
	}
	else
	{
		rgba = GL2_ExpandPalette (data, width * height, flags);
	}

	if (!texture->id)
		glGenTextures (1, &texture->id);
	glActiveTexture (GL_TEXTURE0);
	gl2_bound[0] = texture->id;
	glBindTexture (GL_TEXTURE_2D, texture->id);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	if (flags & GL2TEX_MIPMAP)
		glGenerateMipmap (GL_TEXTURE_2D);
	GL2_SetFilters (texture);

	if ((flags & GL2TEX_FULLBRIGHT) && !(flags & GL2TEX_RGBA))
	{
		GL2_BuildFullbrightMask (texture, data);
	}
	else if (texture->fullbright)
	{
		glDeleteTextures (1, &texture->fullbright);
		texture->fullbright = 0;
	}

	return texture;
}

/*
================
GL2_Bind
================
*/
void GL2_Bind (int unit, gl2texture_t *texture)
{
	GL2_BindName (unit, texture ? texture->id : 0);
}

/*
================
GL2_BindFullbright

Binds the texture's self-lit mask and says whether there is one.  There is
always something bound -- a 1x1 black texture when there is no mask -- so
the shader's fullbright fetch is well defined however the driver decides to
schedule the branch.
================
*/
qboolean GL2_BindFullbright (int unit, gl2texture_t *texture)
{
	GLuint	mask = 0;

	if (texture && gl_fullbrights.integer)
		mask = texture->fullbright;

	GL2_BindName (unit, mask ? mask :
			(gl2_black_texture ? gl2_black_texture->id : 0));
	return mask != 0;
}

void GL2_BindName (int unit, GLuint texture)
{
	if (unit < 0 || unit >= (int)(sizeof(gl2_bound) / sizeof(gl2_bound[0])))
		return;
	if (gl2_bound[unit] == texture)
		return;
	gl2_bound[unit] = texture;
	glActiveTexture (GL_TEXTURE0 + unit);
	glBindTexture (GL_TEXTURE_2D, texture);
}

/*
================
GL2_InvalidateBindings
================
*/
void GL2_InvalidateBindings (void)
{
	memset (gl2_bound, 0, sizeof(gl2_bound));
}

/*
================
GL2_ApplyTextureMode

Resolves gl_texturemode into gl_filter_idx and re-applies it, plus
gl_glide_anisotropy, to every live texture.  Called once before the first
upload and again whenever either cvar changes -- rare, and cheap enough to
do the obvious way.
================
*/
void GL2_ApplyTextureMode (void)
{
	int	i;

	for (i = 0; i < NUM_GL2_FILTER_MODES; i++)
	{
		if (!q_strcasecmp (gl_texturemode.string, gl2_filter_modes[i].name))
		{
			gl_filter_idx = i;
			break;
		}
	}
	if (i == NUM_GL2_FILTER_MODES)
	{
		Con_Printf ("bad filter name: %s\n", gl_texturemode.string);
		Cvar_Set ("gl_texturemode", gl2_filter_modes[gl_filter_idx].name);
		return;
	}

	for (i = 0; i < gl2_numtextures; i++)
	{
		if (!gl2_textures[i].id)
			continue;
		glActiveTexture (GL_TEXTURE0);
		gl2_bound[0] = gl2_textures[i].id;
		glBindTexture (GL_TEXTURE_2D, gl2_textures[i].id);
		GL2_SetFilters (&gl2_textures[i]);
		if (gl2_textures[i].fullbright)
		{
			gl2_bound[0] = gl2_textures[i].fullbright;
			glBindTexture (GL_TEXTURE_2D, gl2_textures[i].fullbright);
			GL2_SetFilters (&gl2_textures[i]);
		}
	}
}

/*
================
GL2_FreeMapTextures

Everything the map brought with it goes away between levels; the renderer's
own textures are flagged GL2TEX_PERSIST and stay.
================
*/
void GL2_FreeMapTextures (void)
{
	int	i, kept = 0;

	memset (gl2_texture_hash, 0, sizeof(gl2_texture_hash));
	GL2_InvalidateBindings ();

	for (i = 0; i < gl2_numtextures; i++)
	{
		if (gl2_textures[i].flags & GL2TEX_PERSIST)
		{
			if (kept != i)
				gl2_textures[kept] = gl2_textures[i];
			kept++;
		}
		else
		{
			if (gl2_textures[i].id)
				glDeleteTextures (1, &gl2_textures[i].id);
			if (gl2_textures[i].fullbright)
				glDeleteTextures (1, &gl2_textures[i].fullbright);
		}
	}
	gl2_numtextures = kept;

	for (i = 0; i < gl2_numtextures; i++)
	{
		unsigned int	bucket = GL2_TextureHash (gl2_textures[i].name);

		gl2_textures[i].next = gl2_texture_hash[bucket];
		gl2_texture_hash[bucket] = &gl2_textures[i];
	}

	/* The kept entries moved, so re-resolve the renderer's own handles. */
	gl2_particle_texture = GL2_FindTexture ("*particle");
	gl2_white_texture = GL2_FindTexture ("*white");
	gl2_black_texture = GL2_FindTexture ("*black");
}

/*
================
GL2_BuildParticleTexture

A soft round dot.  Glide drew particles as filtered, blended quads and the
result was the single most obvious "you have a 3Dfx" tell in 1997.
================
*/
static void GL2_BuildParticleTexture (void)
{
#define GL2_PARTICLE_SIZE	32
	static byte	dot[GL2_PARTICLE_SIZE * GL2_PARTICLE_SIZE * 4];
	int		x, y;

	for (y = 0; y < GL2_PARTICLE_SIZE; y++)
	{
		for (x = 0; x < GL2_PARTICLE_SIZE; x++)
		{
			float	dx = (x + 0.5f) / GL2_PARTICLE_SIZE * 2.0f - 1.0f;
			float	dy = (y + 0.5f) / GL2_PARTICLE_SIZE * 2.0f - 1.0f;
			float	d = 1.0f - sqrt (dx * dx + dy * dy);
			byte	*pixel = &dot[(y * GL2_PARTICLE_SIZE + x) * 4];

			if (d < 0.0f)
				d = 0.0f;
			d = d * d;
			pixel[0] = pixel[1] = pixel[2] = 255;
			pixel[3] = (byte)(d * 255.0f);
		}
	}

	gl2_particle_texture = GL2_LoadTexture ("*particle", GL2_PARTICLE_SIZE,
			GL2_PARTICLE_SIZE, dot,
			GL2TEX_RGBA | GL2TEX_MIPMAP | GL2TEX_CLAMP | GL2TEX_PERSIST);
}

/*
================
GL2_TextureInit
================
*/
void GL2_TextureInit (void)
{
	static const byte	white[4] = { 255, 255, 255, 255 };
	static const byte	black[4] = { 0, 0, 0, 255 };

	gl2_numtextures = 0;
	memset (gl2_texture_hash, 0, sizeof(gl2_texture_hash));
	GL2_InvalidateBindings ();

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);

	GL2_ProbeCaps ();
	/* Resolve gl_texturemode before the first texture is uploaded: every
	 * texture picks its filters up at load time, and nothing re-applies
	 * them unless the cvar changes. */
	GL2_ApplyTextureMode ();

	gl2_white_texture = GL2_LoadTexture ("*white", 1, 1, white,
			GL2TEX_RGBA | GL2TEX_CLAMP | GL2TEX_PERSIST);
	gl2_black_texture = GL2_LoadTexture ("*black", 1, 1, black,
			GL2TEX_RGBA | GL2TEX_CLAMP | GL2TEX_PERSIST);
	GL2_BuildParticleTexture ();
}

gl2texture_t *GL2_ParticleTexture (void)
{
	return gl2_particle_texture;
}

gl2texture_t *GL2_WhiteTexture (void)
{
	return gl2_white_texture;
}
