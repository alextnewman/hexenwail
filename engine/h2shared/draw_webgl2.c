#include "quakedef.h"
#include "web_perf.h"
#include "gl2_glide.h"

#define WEB_MAX_CACHED_PICS	256
#define WEB_PLAYER_WIDTH	68
#define WEB_PLAYER_HEIGHT	114

typedef struct
{
	GLuint texture;
} webpic_t;

typedef struct
{
	char name[MAX_QPATH];
	qpic_t pic;
	byte padding[sizeof(webpic_t) + 8];
} webcachepic_t;

typedef struct
{
	float x, y, s, t;
} webui_vertex_t;

qboolean draw_reinit;
cvar_t scr_sbarscale = {"scr_sbarscale", "1", CVAR_ARCHIVE};
cvar_t scr_menuscale = {"scr_menuscale", "1", CVAR_ARCHIVE};
cvar_t scr_crosshairscale = {"scr_crosshairscale", "1", CVAR_ARCHIVE};
cvar_t scr_conalpha = {"scr_conalpha", "0.8", CVAR_ARCHIVE};
cvar_t scr_conbrightness = {"scr_conbrightness", "1", CVAR_ARCHIVE};

static webcachepic_t pic_cache[WEB_MAX_CACHED_PICS];
static int pic_cache_count;
static GLuint ui_program, ui_vao, ui_vbo;
static GLint ui_size_uniform, ui_color_uniform, ui_texture_uniform;
static GLint ui_gamma_uniform, ui_contrast_uniform;
static GLuint charset_texture, smallfont_texture, bigfont_texture;
static GLuint console_texture, backtile_texture;
static byte player_pixels[MAX_PLAYER_CLASS][WEB_PLAYER_WIDTH * WEB_PLAYER_HEIGHT];
static float character_alpha = 1.0f;
static int canvas_width, canvas_height;
/* Origin of the current 2D canvas, in screen pixels (see GL_SetCanvas). */
static int canvas_x, canvas_y;

static const char ui_vertex_source[] =
	"#version 300 es\n"
	"layout(location=0) in vec2 a_position;\n"
	"layout(location=1) in vec2 a_texcoord;\n"
	"uniform vec2 u_size;\n"
	"out vec2 v_texcoord;\n"
	"void main(){\n"
	" vec2 p=a_position/u_size*2.0-1.0;\n"
	" gl_Position=vec4(p.x,-p.y,0.0,1.0);\n"
	" v_texcoord=a_texcoord;\n"
	"}\n";

/* The 2D layer is drawn straight to the canvas, after the scene has been
 * scanned out, so it has to apply the gamma ramp itself: the software
 * renderer gets it for free by baking v_gamma into the palette, and
 * without this the brightness slider would move the world and leave the
 * menu, HUD and console behind. */
static const char ui_fragment_source[] =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 v_texcoord;\n"
	"uniform sampler2D u_texture;\n"
	"uniform vec4 u_color;\n"
	"uniform float u_gamma;\n"
	"uniform float u_contrast;\n"
	"out vec4 frag_color;\n"
	"void main(){\n"
	" vec4 c=texture(u_texture,v_texcoord)*u_color;\n"
	" c.rgb=(c.rgb-0.5)*u_contrast+0.5;\n"
	" c.rgb=pow(max(c.rgb,vec3(0.0)),vec3(u_gamma));\n"
	" frag_color=vec4(clamp(c.rgb,0.0,1.0),c.a);\n"
	"}\n";

static GLuint Draw_CompileShader (GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok;
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		Sys_Error("WebGL2 UI shader: %s", log);
	}
	return shader;
}

static void Draw_InitProgram (void)
{
	GLuint vertex = Draw_CompileShader(GL_VERTEX_SHADER, ui_vertex_source);
	GLuint fragment = Draw_CompileShader(GL_FRAGMENT_SHADER, ui_fragment_source);
	GLint ok;

	ui_program = glCreateProgram();
	glAttachShader(ui_program, vertex);
	glAttachShader(ui_program, fragment);
	glLinkProgram(ui_program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	glGetProgramiv(ui_program, GL_LINK_STATUS, &ok);
	if (!ok)
		Sys_Error("Unable to link the WebGL2 UI shader");
	ui_size_uniform = glGetUniformLocation(ui_program, "u_size");
	ui_color_uniform = glGetUniformLocation(ui_program, "u_color");
	ui_texture_uniform = glGetUniformLocation(ui_program, "u_texture");
	ui_gamma_uniform = glGetUniformLocation(ui_program, "u_gamma");
	ui_contrast_uniform = glGetUniformLocation(ui_program, "u_contrast");

	glGenVertexArrays(1, &ui_vao);
	glGenBuffers(1, &ui_vbo);
	glBindVertexArray(ui_vao);
	glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
	glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(webui_vertex_t), NULL, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(webui_vertex_t), (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(webui_vertex_t),
		(void *)(2 * sizeof(float)));
	glBindVertexArray(0);
}

static GLuint Draw_LoadTexture (const byte *pixels, int width, int height,
	qboolean transparent, qboolean zero_transparent)
{
	unsigned int *rgba;
	GLuint texture;
	int i, count = width * height;

	rgba = malloc(count * sizeof(*rgba));
	if (!rgba)
		Sys_Error("Web UI texture allocation failed");
	for (i = 0; i < count; ++i)
	{
		int index = pixels[i];
		rgba[i] = d_8to24table[index];
		if ((transparent && index == 255) || (zero_transparent && index == 0))
			rgba[i] &= 0x00ffffffu;
	}
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	free(rgba);
	return texture;
}

static void Draw_Quad (GLuint texture, float x, float y, float width, float height,
	float sl, float tl, float sh, float th, float r, float g, float b, float a)
{
	/* canvas coordinates -> screen coordinates */
	const float x0 = x + canvas_x, y0 = y + canvas_y;
	webui_vertex_t vertices[6] = {
		{x0, y0, sl, tl}, {x0 + width, y0, sh, tl}, {x0 + width, y0 + height, sh, th},
		{x0, y0, sl, tl}, {x0 + width, y0 + height, sh, th}, {x0, y0 + height, sl, th}
	};
	float gamma, contrast;
	if (!texture)
		return;
	GL2_GammaContrast(&gamma, &contrast);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(ui_program);
	glUniform2f(ui_size_uniform, canvas_width, canvas_height);
	glUniform4f(ui_color_uniform, r, g, b, a);
	glUniform1i(ui_texture_uniform, 0);
	glUniform1f(ui_gamma_uniform, gamma);
	glUniform1f(ui_contrast_uniform, contrast);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBindVertexArray(ui_vao);
	glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	WebPerf_CountDraw (2);
	glBindVertexArray(0);
	glEnable(GL_DEPTH_TEST);
}

static void Draw_SetPic (qpic_t *pic, GLuint texture)
{
	webpic_t value;
	value.texture = texture;
	memcpy(pic->data, &value, sizeof(value));
}

static GLuint Draw_GetPic (const qpic_t *pic)
{
	webpic_t value;
	memcpy(&value, pic->data, sizeof(value));
	return value.texture;
}

static qpic_t *Draw_LoadPicFile (const char *path, qboolean no_transparency)
{
	qpic_t *pic = (qpic_t *)FS_LoadHunkFile(path, NULL);
	if (!pic)
		return NULL;
	SwapPic(pic);
	Draw_SetPic(pic, Draw_LoadTexture(pic->data, pic->width, pic->height,
		!no_transparency, false));
	return pic;
}

qpic_t *Draw_PicFromFile (const char *name)
{
	return Draw_LoadPicFile(name, false);
}

qpic_t *Draw_PicFromWad (const char *name)
{
	qpic_t *pic = W_GetLumpName(name);
	Draw_SetPic(pic, Draw_LoadTexture(pic->data, pic->width, pic->height, true, false));
	return pic;
}

static qpic_t *Draw_CachePicInternal (const char *path, qboolean no_transparency)
{
	qpic_t *source;
	webcachepic_t *entry;
	GLuint texture;
	int i;

	for (i = 0; i < pic_cache_count; ++i)
		if (!strcmp(pic_cache[i].name, path))
			return &pic_cache[i].pic;
	if (pic_cache_count == WEB_MAX_CACHED_PICS)
		Sys_Error("Web UI picture cache exhausted");
	source = (qpic_t *)FS_LoadTempFile(path, NULL);
	if (!source)
		Sys_Error("Failed to load %s", path);
	SwapPic(source);
	texture = Draw_LoadTexture(source->data, source->width, source->height,
		!no_transparency, false);
	entry = &pic_cache[pic_cache_count++];
	q_strlcpy(entry->name, path, sizeof(entry->name));
	entry->pic.width = source->width;
	entry->pic.height = source->height;
	Draw_SetPic(&entry->pic, texture);
	if (!strncmp(path, "gfx/menu/netp", 13) &&
		path[13] >= '1' && path[13] <= '0' + MAX_PLAYER_CLASS)
		memcpy(player_pixels[path[13] - '1'], source->data,
			WEB_PLAYER_WIDTH * WEB_PLAYER_HEIGHT);
	return &entry->pic;
}

qpic_t *Draw_CachePic (const char *path) { return Draw_CachePicInternal(path, false); }
qpic_t *Draw_CachePicNoTrans (const char *path) { return Draw_CachePicInternal(path, true); }
qpic_t *Draw_CacheLoadingPic (void) { return Draw_CachePic("gfx/menu/loading.lmp"); }

void Draw_Init (void)
{
	qpic_t *pic;
	byte *pixels;
	if (!ui_program)
		Draw_InitProgram();
	if (!draw_reinit)
	{
		Cvar_RegisterVariable(&scr_sbarscale);
		Cvar_RegisterVariable(&scr_menuscale);
		Cvar_RegisterVariable(&scr_crosshairscale);
		Cvar_RegisterVariable(&scr_conalpha);
		Cvar_RegisterVariable(&scr_conbrightness);
	}
	pixels = FS_LoadTempFile("gfx/menu/conchars.lmp", NULL);
	if (!pixels || fs_filesize < 256 * 128)
		Sys_Error("Invalid gfx/menu/conchars.lmp");
	charset_texture = Draw_LoadTexture(pixels, 256, 128, false, true);
	pixels = W_GetLumpName("tinyfont");
	smallfont_texture = Draw_LoadTexture(pixels, 128, 32, false, true);
	pic = (qpic_t *)FS_LoadTempFile("gfx/menu/bigfont2.lmp", NULL);
	if (!pic)
		pic = (qpic_t *)FS_LoadTempFile("gfx/menu/bigfont.lmp", NULL);
	if (!pic)
		Sys_Error("Missing big menu font");
	SwapPic(pic);
	bigfont_texture = Draw_LoadTexture(pic->data, pic->width, pic->height, false, true);
	pic = (qpic_t *)FS_LoadTempFile("gfx/menu/conback.lmp", NULL);
	if (!pic)
		Sys_Error("Missing console background");
	SwapPic(pic);
	console_texture = Draw_LoadTexture(pic->data, pic->width, pic->height, false, false);
	pic = (qpic_t *)FS_LoadTempFile("gfx/menu/backtile.lmp", NULL);
	if (!pic)
		Sys_Error("Missing background tile");
	SwapPic(pic);
	backtile_texture = Draw_LoadTexture(pic->data, pic->width, pic->height, false, false);
	GL_SetCanvas(CANVAS_DEFAULT);
}

void Draw_ReInit (void) { draw_reinit = true; Draw_Init(); draw_reinit = false; }
/*
================
GL_SetCanvas

Places one of the fixed-size logical UI canvases on the screen. Draw_Quad
translates every 2D vertex by the canvas origin, which is what anchors the
status bar to the bottom of the screen and centres the menus; sbar.c and
menu.c draw in canvas coordinates. The UI scale is 1 here -- the software
renderer's canvases must land in the same place, and it cannot scale 2D.
================
*/
void GL_SetCanvas (canvastype canvas)
{
	canvas_width = vid.width;
	canvas_height = vid.height;

	switch (canvas)
	{
	case CANVAS_SBAR:
		canvas_x = (vid.width - UI_CANVAS_WIDTH) / 2;
		canvas_y = vid.height - UI_SBAR_CANVAS_HEIGHT;
		break;
	case CANVAS_MENU:
		canvas_x = (vid.width - UI_CANVAS_WIDTH) / 2;
		canvas_y = 0;
		break;
	default:
		canvas_x = 0;
		canvas_y = 0;
		break;
	}

	if (canvas_x < 0)
		canvas_x = 0;
	if (canvas_y < 0)
		canvas_y = 0;
}
void Draw_FlushCharBatch (void) {}
/* The web 2D layers do not scale the UI canvases -- both renderers place
 * them at scale 1 -- so report that honestly. menu.c derives its mouse
 * hit-testing from this value, and a scale the canvas does not actually
 * apply would move the hit boxes away from the glyphs. */
float SCR_CalcUIScale (cvar_t *user) { (void)user; return 1.0f; }

void Draw_Pic (int x, int y, qpic_t *pic)
{
	Draw_Quad(Draw_GetPic(pic), x, y, pic->width, pic->height, 0, 0, 1, 1, 1, 1, 1, 1);
}
void Draw_TransPic (int x, int y, qpic_t *pic) { Draw_Pic(x, y, pic); }
void Draw_AlphaPic (int x, int y, qpic_t *pic, float alpha)
{
	Draw_Quad(Draw_GetPic(pic), x, y, pic->width, pic->height, 0, 0, 1, 1,
		1, 1, 1, alpha);
}
void Draw_PicCropped (int x, int y, qpic_t *pic) { Draw_Pic(x, y, pic); }
void Draw_TransPicCropped (int x, int y, qpic_t *pic) { Draw_Pic(x, y, pic); }
void Draw_SubPicCropped (int x, int y, int height, qpic_t *pic)
{
	float ratio = q_min(height, pic->height) / (float)pic->height;
	Draw_Quad(Draw_GetPic(pic), x, y, pic->width, pic->height * ratio,
		0, 0, 1, ratio, 1, 1, 1, 1);
}
void Draw_SubPic (int x, int y, qpic_t *pic, int sx, int sy, int width, int height)
{
	Draw_Quad(Draw_GetPic(pic), x, y, width, height,
		sx / (float)pic->width, sy / (float)pic->height,
		(sx + width) / (float)pic->width, (sy + height) / (float)pic->height,
		1, 1, 1, 1);
}

void Draw_Character (int x, int y, unsigned int num)
{
	float sl, tl;
	if (num == 32)
		return;
	num &= 511;
	sl = (num & 31) / 32.0f;
	tl = (num >> 5) / 16.0f;
	Draw_Quad(charset_texture, x, y, 8, 8, sl, tl, sl + 1.0f / 32.0f,
		tl + 1.0f / 16.0f, 1, 1, 1, character_alpha);
}

void Draw_SetCharacterAlpha (float alpha) { character_alpha = alpha; }
void Draw_String (int x, int y, const char *text)
{
	while (*text) { Draw_Character(x, y, (byte)*text++); x += 8; }
}
void Draw_RedString (int x, int y, const char *text)
{
	while (*text) { Draw_Character(x, y, ((byte)*text++) + 256); x += 8; }
}
void Draw_SmallCharacter (int x, int y, int num)
{
	int row, col;
	float sl, tl;

	/* The tinyfont atlas is 128x32: 16 columns of 8x8 over 4 rows, and it
	 * only covers ' '..'_'.  Lower case folds onto upper case.  Same
	 * folding as gl_draw.c:968 and draw.c. */
	if (num < 32)
		num = 0;
	else if (num >= 'a' && num <= 'z')
		num -= 64;
	else if (num > '_')
		num = 0;
	else
		num -= 32;

	if (num == 0)
		return;

	row = num >> 4;
	col = num & 15;
	sl = col / 16.0f;
	tl = row / 4.0f;
	Draw_Quad(smallfont_texture, x, y, 8, 8, sl, tl, sl + 1.0f / 16.0f,
		tl + 1.0f / 4.0f, 1, 1, 1, character_alpha);
}
void Draw_SmallString (int x, int y, const char *text)
{
	while (*text) { Draw_SmallCharacter(x, y, (byte)*text++); x += 6; }
}
void Draw_BigCharacter (int x, int y, int num)
{
	float sl, tl;
	/* num is a glyph index, not an ASCII code: M_DrawBigCharacter has
	 * already mapped '/' to 26 and 'A'..'Z' to 0..25, and rejects
	 * anything else.  The bigfont atlas is 160x80, 8 columns of 20x20
	 * over 4 rows.  Same contract as gl_draw.c:1029 and draw.c. */
	sl = (num % 8) / 8.0f;
	tl = (num / 8) / 4.0f;
	Draw_Quad(bigfont_texture, x, y, 20, 20, sl, tl, sl + 0.125f,
		tl + 0.25f, 1, 1, 1, 1);
}

static GLuint Draw_WhiteTexture (void)
{
	static GLuint texture;
	if (!texture)
	{
		byte white = 254;
		texture = Draw_LoadTexture(&white, 1, 1, false, false);
	}
	return texture;
}

void Draw_FillAlpha (int x, int y, int w, int h, float r, float g, float b, float a)
{
	Draw_Quad(Draw_WhiteTexture(), x, y, w, h, 0, 0, 1, 1, r, g, b, a);
}
void Draw_Fill (int x, int y, int w, int h, int color)
{
	byte *rgba = (byte *)&d_8to24table[color & 255];
	Draw_FillAlpha(x, y, w, h, rgba[0] / 255.0f, rgba[1] / 255.0f,
		rgba[2] / 255.0f, 1);
}
void Draw_FadeScreen (void) { Draw_FillAlpha(0, 0, vid.width, vid.height, 0, 0, 0, 0.65f); }
void Draw_MenuBackdrop (void) { Draw_ConsoleBackground(vid.height); }
void Draw_ConsoleBackground (int lines)
{
	Draw_Quad(console_texture, 0, 0, vid.width, lines, 0, 0, 1, 1,
		scr_conbrightness.value, scr_conbrightness.value, scr_conbrightness.value,
		scr_conalpha.value);
}
void Draw_TileClear (int x, int y, int w, int h)
{
	Draw_Quad(backtile_texture, x, y, w, h, 0, 0, w / 64.0f, h / 64.0f, 1, 1, 1, 1);
}
void Draw_Crosshair (void)
{
	Draw_Character(vid.width / 2 - 4, vid.height / 2 - 4, '+');
}
void Draw_IntermissionPic (qpic_t *pic)
{
	Draw_Quad(Draw_GetPic(pic), 0, 0, vid.width, vid.height, 0, 0, 1, 1, 1, 1, 1, 1);
}
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation, int p_class)
{
	(void)translation;
	(void)p_class;
	Draw_TransPic(x, y, pic);
}
