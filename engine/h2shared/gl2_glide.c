/*
 * gl2_glide.c -- WebGlide matrices, scene buffer and scan-out.
 *
 * The scene is not drawn to the canvas.  It is drawn into an offscreen
 * buffer -- a fraction of the view by default, see gl_glide_scenescale --
 * and then "scanned out" through a pass that does what the Voodoo's RAMDAC
 * did on the way to the monitor: the 2x2 postfilter, the gamma ramp, the
 * palette blend, and (if you want it) a CRT.
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

/* An M-series GPU will happily rasterise more than this, but the frame is
 * also carrying a single-threaded WASM engine; past here the win is not
 * worth the bandwidth. */
#define GL2_MAX_SCENE_PIXELS	(2560 * 1440)

static GLuint	gl2_scene_fbo;
static GLuint	gl2_scene_color;
static GLuint	gl2_scene_depth;
static GLuint	gl2_history_fbo[2];
static GLuint	gl2_history_color[2];
static GLuint	gl2_empty_vao;

static int	gl2_history_index;
static qboolean	gl2_history_valid;
static int	gl2_history_width, gl2_history_height;

static int	gl2_dest_x, gl2_dest_y, gl2_dest_width, gl2_dest_height;
static int	gl2_max_texture_size = 2048;

int		gl2_scene_width, gl2_scene_height;

/*
=============================================================================

	matrices

	Column major, in the order glUniformMatrix4fv wants without a
	transpose (which GLES3 does not allow anyway).

=============================================================================
*/

void GL2_MatrixIdentity (gl2matrix_t *out)
{
	memset (out, 0, sizeof(*out));
	out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
}

void GL2_MatrixMultiply (gl2matrix_t *out, const gl2matrix_t *a, const gl2matrix_t *b)
{
	gl2matrix_t	result;
	int		row, col;

	for (col = 0; col < 4; col++)
	{
		for (row = 0; row < 4; row++)
		{
			result.m[col * 4 + row] =
				a->m[0 * 4 + row] * b->m[col * 4 + 0] +
				a->m[1 * 4 + row] * b->m[col * 4 + 1] +
				a->m[2 * 4 + row] * b->m[col * 4 + 2] +
				a->m[3 * 4 + row] * b->m[col * 4 + 3];
		}
	}
	*out = result;
}

/*
================
GL2_MatrixFrustum

An infinite far plane: Hexen II's maps are large and its skies are drawn
as ordinary geometry, so there is nothing to gain from a finite one and a
whole class of far-clipping bugs to lose.
================
*/
void GL2_MatrixFrustum (gl2matrix_t *out, float fovx, float fovy, float zNear)
{
	float	w, h;

	if (fovx < 1.0f) fovx = 1.0f;
	if (fovx > 179.0f) fovx = 179.0f;
	if (fovy < 1.0f) fovy = 1.0f;
	if (fovy > 179.0f) fovy = 179.0f;

	w = 1.0f / tanf (fovx * (float)M_PI / 360.0f);
	h = 1.0f / tanf (fovy * (float)M_PI / 360.0f);

	memset (out, 0, sizeof(*out));
	out->m[0] = w;
	out->m[5] = h;
	out->m[10] = -1.0f;
	out->m[11] = -1.0f;
	out->m[14] = -2.0f * zNear;
}

void GL2_MatrixOrtho (gl2matrix_t *out, float left, float right, float bottom,
			float top, float zNear, float zFar)
{
	memset (out, 0, sizeof(*out));
	out->m[0] = 2.0f / (right - left);
	out->m[5] = 2.0f / (top - bottom);
	out->m[10] = -2.0f / (zFar - zNear);
	out->m[12] = -(right + left) / (right - left);
	out->m[13] = -(top + bottom) / (top - bottom);
	out->m[14] = -(zFar + zNear) / (zFar - zNear);
	out->m[15] = 1.0f;
}

void GL2_MatrixRotate (gl2matrix_t *out, float degrees, float x, float y, float z)
{
	float	radians = degrees * (float)M_PI / 180.0f;
	float	c = cosf (radians);
	float	s = sinf (radians);
	float	length = sqrtf (x * x + y * y + z * z);

	GL2_MatrixIdentity (out);
	if (length < 0.000001f)
		return;
	x /= length;
	y /= length;
	z /= length;

	out->m[0] = x * x * (1.0f - c) + c;
	out->m[1] = y * x * (1.0f - c) + z * s;
	out->m[2] = x * z * (1.0f - c) - y * s;
	out->m[4] = x * y * (1.0f - c) - z * s;
	out->m[5] = y * y * (1.0f - c) + c;
	out->m[6] = y * z * (1.0f - c) + x * s;
	out->m[8] = x * z * (1.0f - c) + y * s;
	out->m[9] = y * z * (1.0f - c) - x * s;
	out->m[10] = z * z * (1.0f - c) + c;
}

void GL2_MatrixTranslate (gl2matrix_t *out, float x, float y, float z)
{
	GL2_MatrixIdentity (out);
	out->m[12] = x;
	out->m[13] = y;
	out->m[14] = z;
}

void GL2_MatrixScale (gl2matrix_t *out, float x, float y, float z)
{
	GL2_MatrixIdentity (out);
	out->m[0] = x;
	out->m[5] = y;
	out->m[10] = z;
}

/*
=============================================================================

	fog

=============================================================================
*/

/*
================
GL2_SetupProgramFog

gl_glide_fogtable 0 turns fog off outright, which is the honest way to
express "this card has no fog unit".
================
*/
void GL2_SetupProgramFog (const gl2program_t *program)
{
	float	density = gl_glide_fogtable.integer ? r_fog_density : 0.0f;

	if (program->u_fogcolor >= 0)
		glUniform3f (program->u_fogcolor, r_fog_color[0], r_fog_color[1], r_fog_color[2]);
	if (program->u_fogdensity >= 0)
		glUniform1f (program->u_fogdensity, density);
}

/*
================
GL2_SetupSceneUniforms

Everything every scene program shares: the fog table, the 16bpp dither
steps and the texture LOD controls.  Called once per program per frame.
================
*/
void GL2_SetupSceneUniforms (const gl2program_t *program)
{
	GL2_SetupProgramFog (program);

	if (program->u_dither >= 0)
	{
		/* RGB 5:6:5 is what the Voodoo wrote, so those are the
		 * quantisation steps; 0 means leave the frame at RGBA8. */
		if (gl_glide_dither.integer && gl_glide_colordepth.integer <= 16)
			glUniform3f (program->u_dither, 31.0f, 63.0f, 31.0f);
		else
			glUniform3f (program->u_dither, 0.0f, 0.0f, 0.0f);
	}
	if (program->u_lodbias >= 0)
		glUniform1f (program->u_lodbias, gl_glide_lodbias.value);
	if (program->u_mipdither >= 0)
		glUniform1f (program->u_mipdither, gl_glide_mipmapdither.value);
}

/*
=============================================================================

	scene buffer

=============================================================================
*/

static void GL2_DeleteSceneTargets (void)
{
	int	i;

	if (gl2_scene_fbo)
	{
		glDeleteFramebuffers (1, &gl2_scene_fbo);
		gl2_scene_fbo = 0;
	}
	if (gl2_scene_color)
	{
		glDeleteTextures (1, &gl2_scene_color);
		gl2_scene_color = 0;
	}
	if (gl2_scene_depth)
	{
		glDeleteRenderbuffers (1, &gl2_scene_depth);
		gl2_scene_depth = 0;
	}
	for (i = 0; i < 2; i++)
	{
		if (gl2_history_fbo[i])
		{
			glDeleteFramebuffers (1, &gl2_history_fbo[i]);
			gl2_history_fbo[i] = 0;
		}
		if (gl2_history_color[i])
		{
			glDeleteTextures (1, &gl2_history_color[i]);
			gl2_history_color[i] = 0;
		}
	}
	gl2_history_valid = false;
	gl2_history_width = gl2_history_height = 0;
	GL2_InvalidateBindings ();
}

static GLuint GL2_CreateSceneTexture (int width, int height)
{
	GLuint	texture;

	glGenTextures (1, &texture);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, texture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	/* Linear both ways: the sampler is what resolves a supersampled
	 * buffer and what stretches a quarter-resolution one. */
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	GL2_InvalidateBindings ();
	return texture;
}

/*
================
GL2_ResizeScene

Rebuilds the scene target.  Returns false if the driver will not give us a
complete framebuffer, in which case the caller draws to the canvas directly
and the player loses the scan-out effects but keeps a picture.
================
*/
static qboolean GL2_ResizeScene (int width, int height)
{
	if (width == gl2_scene_width && height == gl2_scene_height && gl2_scene_fbo)
		return true;

	GL2_DeleteSceneTargets ();
	gl2_scene_width = width;
	gl2_scene_height = height;

	gl2_scene_color = GL2_CreateSceneTexture (width, height);

	glGenRenderbuffers (1, &gl2_scene_depth);
	glBindRenderbuffer (GL_RENDERBUFFER, gl2_scene_depth);
	glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glBindRenderbuffer (GL_RENDERBUFFER, 0);

	glGenFramebuffers (1, &gl2_scene_fbo);
	glBindFramebuffer (GL_FRAMEBUFFER, gl2_scene_fbo);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				gl2_scene_color, 0);
	glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
				gl2_scene_depth);

	if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_Printf ("WebGlide: %dx%d scene buffer is incomplete\n", width, height);
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		GL2_DeleteSceneTargets ();
		gl2_scene_width = gl2_scene_height = 0;
		return false;
	}
	return true;
}

/*
================
GL2_EnsureHistory

The T-buffer's retained sub-buffer.  Two of them, because a pass cannot
read and write the same texture.
================
*/
static qboolean GL2_EnsureHistory (void)
{
	int	i;

	if (gl2_history_fbo[0] && gl2_history_width == gl2_scene_width &&
	    gl2_history_height == gl2_scene_height)
		return true;

	for (i = 0; i < 2; i++)
	{
		if (gl2_history_fbo[i])
			glDeleteFramebuffers (1, &gl2_history_fbo[i]);
		if (gl2_history_color[i])
			glDeleteTextures (1, &gl2_history_color[i]);
		gl2_history_fbo[i] = 0;
		gl2_history_color[i] = 0;
	}
	gl2_history_valid = false;
	gl2_history_index = 0;
	gl2_history_width = gl2_scene_width;
	gl2_history_height = gl2_scene_height;

	for (i = 0; i < 2; i++)
	{
		gl2_history_color[i] = GL2_CreateSceneTexture (gl2_scene_width, gl2_scene_height);
		glGenFramebuffers (1, &gl2_history_fbo[i]);
		glBindFramebuffer (GL_FRAMEBUFFER, gl2_history_fbo[i]);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					gl2_history_color[i], 0);
		if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			glBindFramebuffer (GL_FRAMEBUFFER, 0);
			Con_Printf ("WebGlide: T-buffer unavailable, disabling\n");
			Cvar_Set ("gl_glide_tbuffer", "0");
			return false;
		}
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
	}
	return true;
}

/*
================
GL2_MotionBlurAmount

0 when the T-buffer is off or the value is not worth a whole extra pass.
================
*/
static float GL2_MotionBlurAmount (void)
{
	float	amount;

	if (!gl_glide_tbuffer.integer)
		return 0.0f;
	amount = gl_glide_motionblur.value;
	if (amount < 0.02f)
		return 0.0f;
	if (amount > 0.9f)
		amount = 0.9f;
	return amount;
}

/*
================
GL2_GlideBeginScene

x/y/width/height are the destination rectangle on the canvas, in GL
(bottom-left origin) pixels.

The scene itself is drawn at gl_glide_scenescale of that -- a quarter of
the pixels by default -- and the scan-out pass resolves it onto the
destination, which is also where the sampler turns a supersampled buffer
back into a picture.
================
*/
void GL2_GlideBeginScene (int x, int y, int width, int height)
{
	float	scale;
	int	scene_w, scene_h;
	double	pixels;

	gl2_dest_x = x;
	gl2_dest_y = y;
	gl2_dest_width = q_max(width, 1);
	gl2_dest_height = q_max(height, 1);

	scale = gl_glide_scenescale.value;
	if (scale < 0.25f)
		scale = 0.25f;
	if (scale > 2.0f)
		scale = 2.0f;

	scene_w = (int)(gl2_dest_width * scale + 0.5f);
	scene_h = (int)(gl2_dest_height * scale + 0.5f);
	scene_w = q_max(scene_w, 1);
	scene_h = q_max(scene_h, 1);

	/* Two ceilings: what the driver will allocate, and what we are
	 * willing to spend. Both scale the request rather than crop it. */
	pixels = (double)scene_w * (double)scene_h;
	if (pixels > GL2_MAX_SCENE_PIXELS)
	{
		float	shrink = (float)sqrt (GL2_MAX_SCENE_PIXELS / pixels);

		scene_w = q_max((int)(scene_w * shrink), 1);
		scene_h = q_max((int)(scene_h * shrink), 1);
	}
	if (scene_w > gl2_max_texture_size)
		scene_w = gl2_max_texture_size;
	if (scene_h > gl2_max_texture_size)
		scene_h = gl2_max_texture_size;

	if (!GL2_ResizeScene (scene_w, scene_h))
	{
		/* Fall back to drawing straight into the canvas. */
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		glViewport (gl2_dest_x, gl2_dest_y, gl2_dest_width, gl2_dest_height);
	}
	else
	{
		glBindFramebuffer (GL_FRAMEBUFFER, gl2_scene_fbo);
		glViewport (0, 0, gl2_scene_width, gl2_scene_height);
	}

	GL2_InvalidateProgram ();
	glEnable (GL_DEPTH_TEST);
	glDepthMask (GL_TRUE);
	glDepthFunc (GL_LEQUAL);
	glDisable (GL_BLEND);
	glEnable (GL_CULL_FACE);
	glCullFace (GL_FRONT);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/*
================
GL2_GammaContrast

The engine's brightness controls, resolved once for both layers of the
frame.  v_gamma is an exponent, not a multiplier: the software renderer
bakes pow(c, v_gamma) into the palette, so values below 1 brighten.
gl_glide_gamma rides on top of it as the Voodoo's own RAMDAC ramp did,
and applies to the 2D layer as well because a RAMDAC could not tell the
menu from the world.
================
*/
void GL2_GammaContrast (float *gamma, float *contrast)
{
	float	g = gl_glide_gamma.value * v_gamma.value;

	if (g < 0.3f)
		g = 0.3f;
	if (g > 3.0f)
		g = 3.0f;
	*gamma = g;
	*contrast = q_max(v_contrast.value, 0.1f);
}

/*
================
GL2_ScanOutPass

One fullscreen triangle through the post program.
================
*/
static void GL2_ScanOutPass (GLuint source, GLuint history, int flags, float blend)
{
	float	dither = 0.0f;
	float	tint[4];
	float	gamma, contrast;

	GL2_UseProgram (&gl2_post_program);
	GL2_BindName (0, source);
	GL2_BindName (1, history);

	glUniform1i (gl2_post_program.u_flags, flags);
	glUniform1f (gl2_post_program.u_blend, blend);
	glUniform2f (gl2_post_program.u_screensize, (float)vid.width, (float)vid.height);
	glUniform2f (gl2_post_program.u_sourcesize, (float)gl2_scene_width, (float)gl2_scene_height);

	if (!(flags & 1))
	{
		dither = gl_glide_postfilter.value;
		if (dither < 0.0f) dither = 0.0f;
		if (dither > 1.0f) dither = 1.0f;
		glUniform1f (gl2_post_program.u_postfilter, dither);

		GL2_GammaContrast (&gamma, &contrast);
		glUniform1f (gl2_post_program.u_gamma, gamma);
		glUniform1f (gl2_post_program.u_contrast, contrast);

		GL2_PolyBlendColor (tint);
		glUniform4f (gl2_post_program.u_tint, tint[0], tint[1], tint[2], tint[3]);

		glUniform4f (gl2_post_program.u_crt,
				q_max(gl_glide_crt.value, 0.0f),
				q_max(gl_glide_crt_mask.value, 0.0f),
				q_max(gl_glide_crt_curve.value, 0.0f),
				q_max(gl_glide_crt_vignette.value, 0.0f));
	}

	glBindVertexArray (gl2_empty_vao);
	glDrawArrays (GL_TRIANGLES, 0, 3);
	glBindVertexArray (0);
}

/*
================
GL2_GlideEndScene
================
*/
void GL2_GlideEndScene (void)
{
	GLuint	source;
	float	blur;

	if (!gl2_scene_fbo)
	{
		/* Direct-to-canvas fallback: nothing to scan out. */
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		return;
	}

	glDisable (GL_DEPTH_TEST);
	glDepthMask (GL_FALSE);
	glDisable (GL_BLEND);
	glDisable (GL_CULL_FACE);

	source = gl2_scene_color;
	blur = GL2_MotionBlurAmount ();
	if (blur > 0.0f && GL2_EnsureHistory ())
	{
		int	next = gl2_history_index ^ 1;

		glBindFramebuffer (GL_FRAMEBUFFER, gl2_history_fbo[next]);
		glViewport (0, 0, gl2_scene_width, gl2_scene_height);
		GL2_ScanOutPass (gl2_scene_color,
				gl2_history_valid ? gl2_history_color[gl2_history_index] : gl2_scene_color,
				1, gl2_history_valid ? blur : 0.0f);
		gl2_history_index = next;
		gl2_history_valid = true;
		source = gl2_history_color[next];
	}
	else
	{
		gl2_history_valid = false;
	}

	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (gl2_dest_x, gl2_dest_y, gl2_dest_width, gl2_dest_height);
	GL2_ScanOutPass (source, source, 0, 0.0f);

	/* Hand the canvas back the way the 2D layer expects to find it. */
	glViewport (0, 0, glwidth, glheight);
	GL2_InvalidateProgram ();
	GL2_InvalidateBindings ();
	glActiveTexture (GL_TEXTURE0);
}

/*
================
GL2_GlideInit
================
*/
void GL2_GlideInit (void)
{
	GLint	max_size = 0;

	glGetIntegerv (GL_MAX_TEXTURE_SIZE, &max_size);
	if (max_size >= 1024)
		gl2_max_texture_size = (int)max_size;

	if (!gl2_empty_vao)
		glGenVertexArrays (1, &gl2_empty_vao);

	gl2_scene_width = gl2_scene_height = 0;
	gl2_history_valid = false;
}

/*
================
GL2_GlideShutdown
================
*/
void GL2_GlideShutdown (void)
{
	GL2_DeleteSceneTargets ();
	gl2_scene_width = gl2_scene_height = 0;
	if (gl2_empty_vao)
	{
		glDeleteVertexArrays (1, &gl2_empty_vao);
		gl2_empty_vao = 0;
	}
}
