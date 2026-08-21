#!/usr/bin/env node

import { readFile, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const SHADER_HEADER = resolve(ROOT, 'engine/h2shared/gl_shader.h');
const SHADER_SOURCE = resolve(ROOT, 'engine/h2shared/gl_shader.c');
const POSTPROCESS_SOURCE = resolve(ROOT, 'engine/h2shared/gl_postprocess.c');
const WEBGLIDE_SOURCE = resolve(ROOT, 'engine/h2shared/gl2_shader.c');
const WEBGLIDE_UI_SOURCE = resolve(ROOT, 'engine/h2shared/draw_webgl2.c');

const PROGRAM_SPECS = [
  ['2d', 'shader', 's2d_vert', 's2d_frag'],
  ['flat', 'shader', 'sflat_vert', 'sflat_frag'],
  ['world', 'shader', 'sworld_vert', 'sworld_frag'],
  ['world_opaque', 'shader', 'sworld_vert', 'sworld_frag_opaque'],
  ['alias', 'shader', 'salias_vert', 'salias_frag'],
  ['particle', 'shader', 'spart_vert', 'spart_frag'],
  ['sky', 'shader', 'ssky_vert', 'ssky_frag'],
  ['postprocess', 'postprocess', 'pp_vert_src', 'pp_frag_src'],
  ['bloom_bright', 'postprocess', 'bloom_vert_src', 'bloom_bright_frag_src'],
  ['bloom_down', 'postprocess', 'bloom_vert_src', 'bloom_down_frag_src'],
  ['bloom_up', 'postprocess', 'bloom_vert_src', 'bloom_up_frag_src'],
  // WebGlide: the experimental web GPU renderer (docs/web/WEBGLIDE.md).
  ['webglide_world', 'webglide', 'glide_world_vert', 'glide_world_frag'],
  ['webglide_sky', 'webglide', 'glide_sky_vert', 'glide_sky_frag'],
  ['webglide_model', 'webglide', 'glide_model_vert', 'glide_model_frag'],
  ['webglide_post', 'webglide', 'glide_post_vert', 'glide_post_frag'],
  ['webglide_ui', 'webglide_ui', 'ui_vertex_source', 'ui_fragment_source'],
];

const WEBGLIDE_MACROS = [
  'GLIDE_VERSION',
  'GLIDE_VERT_PRECISION',
  'GLIDE_FRAG_PRECISION',
  'GLIDE_BAYER_FN',
  'GLIDE_FOG_FN',
  'GLIDE_DITHER_FN',
  'GLIDE_LOD_FN',
  'GLIDE_PALETTE_FN',
];

function tokenize(expression) {
  return expression.match(/"(?:\\.|[^"\\])*"|\/\*[\s\S]*?\*\/|\/\/[^\n]*|[A-Za-z_]\w*/g) || [];
}

function evaluateCStringExpression(expression, macros) {
  let value = '';
  for (const token of tokenize(expression)) {
    if (token.startsWith('"')) {
      value += JSON.parse(token);
    } else if (!token.startsWith('/*') && !token.startsWith('//')) {
      if (!macros.has(token)) throw new Error(`unknown C string macro ${token}`);
      value += macros.get(token);
    }
  }
  return value;
}

function readDefine(source, name, macros) {
  const lines = source.split('\n');
  const start = lines.findIndex((line) => new RegExp(`^\\s*#define\\s+${name}\\b`).test(line));
  if (start < 0) throw new Error(`missing shader macro ${name}`);

  const parts = [];
  for (let index = start; index < lines.length; index += 1) {
    let line = lines[index];
    if (index === start) line = line.replace(new RegExp(`^\\s*#define\\s+${name}\\b`), '');
    const continued = /\\\s*$/.test(line);
    parts.push(line.replace(/\\\s*$/, ''));
    if (!continued) break;
  }
  return evaluateCStringExpression(parts.join('\n'), macros);
}

function readArrayExpression(source, name) {
  const marker = new RegExp(`static\\s+const\\s+char\\s+${name}\\s*\\[\\s*\\]\\s*=`, 'm');
  const match = marker.exec(source);
  if (!match) throw new Error(`missing shader source ${name}`);

  let inString = false;
  let inLineComment = false;
  let inBlockComment = false;
  let escaped = false;
  const start = match.index + match[0].length;

  for (let index = start; index < source.length; index += 1) {
    const char = source[index];
    const next = source[index + 1];

    if (inString) {
      if (escaped) escaped = false;
      else if (char === '\\') escaped = true;
      else if (char === '"') inString = false;
      continue;
    }
    if (inLineComment) {
      if (char === '\n') inLineComment = false;
      continue;
    }
    if (inBlockComment) {
      if (char === '*' && next === '/') {
        inBlockComment = false;
        index += 1;
      }
      continue;
    }
    if (char === '"') inString = true;
    else if (char === '/' && next === '/') {
      inLineComment = true;
      index += 1;
    } else if (char === '/' && next === '*') {
      inBlockComment = true;
      index += 1;
    } else if (char === ';') {
      return source.slice(start, index);
    }
  }
  throw new Error(`unterminated shader source ${name}`);
}

function injectEngineDefines(source) {
  const versionEnd = source.indexOf('\n');
  if (versionEnd < 0) throw new Error('shader source has no #version line');
  return `${source.slice(0, versionEnd + 1)}#define BINDLESS 0\n${source.slice(versionEnd + 1)}`;
}

export async function extractEngineWebGLPrograms() {
  const [header, shaderSource, postprocessSource, webglideSource, webglideUiSource] = await Promise.all([
    readFile(SHADER_HEADER, 'utf8'),
    readFile(SHADER_SOURCE, 'utf8'),
    readFile(POSTPROCESS_SOURCE, 'utf8'),
    readFile(WEBGLIDE_SOURCE, 'utf8'),
    readFile(WEBGLIDE_UI_SOURCE, 'utf8'),
  ]);
  const webProfileStart = header.indexOf('#ifdef __EMSCRIPTEN__');
  const webProfileEnd = header.indexOf('#else', webProfileStart);
  if (webProfileStart < 0 || webProfileEnd < 0) throw new Error('missing WebGL shader profile');
  const webProfile = header.slice(webProfileStart, webProfileEnd);

  const macros = new Map();
  macros.set('GLSL_VERTEX_PRECISION', readDefine(header, 'GLSL_VERTEX_PRECISION', macros));
  macros.set('GLSL_FRAGMENT_PRECISION', readDefine(header, 'GLSL_FRAGMENT_PRECISION', macros));
  macros.set('GLSL_PROFILE_VERSION', readDefine(webProfile, 'GLSL_PROFILE_VERSION', macros));
  macros.set('GLSL_EARLY_Z', readDefine(webProfile, 'GLSL_EARLY_Z', macros));
  macros.set('GLSL_EARLY_Z_OPAQUE', readDefine(webProfile, 'GLSL_EARLY_Z_OPAQUE', macros));
  macros.set('GLSL_VERT_HEADER', readDefine(header, 'GLSL_VERT_HEADER', macros));
  macros.set('GLSL_FRAG_HEADER', readDefine(header, 'GLSL_FRAG_HEADER', macros));
  macros.set('GLSL_BICUBIC_LM_FN', readDefine(shaderSource, 'GLSL_BICUBIC_LM_FN', macros));
  macros.set('GLSL_CAUSTICS_FN', readDefine(shaderSource, 'GLSL_CAUSTICS_FN', macros));

  for (const name of WEBGLIDE_MACROS) {
    macros.set(name, readDefine(webglideSource, name, macros));
  }

  const sources = {
    shader: shaderSource,
    postprocess: postprocessSource,
    webglide: webglideSource,
    webglide_ui: webglideUiSource,
  };
  return PROGRAM_SPECS.map(([name, family, vertexName, fragmentName]) => ({
    name,
    vertex: injectEngineDefines(evaluateCStringExpression(
      readArrayExpression(sources[family], vertexName), macros,
    )),
    fragment: injectEngineDefines(evaluateCStringExpression(
      readArrayExpression(sources[family], fragmentName), macros,
    )),
  }));
}

export function renderEngineShaderSmokePage(programs) {
  const serialized = JSON.stringify(programs).replaceAll('<', '\\u003c');
  return `<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><title>Engine WebGL2 shader smoke test</title></head>
<body data-result="pending"><pre id="result">pending</pre>
<script>
const sources = ${serialized};
const output = document.getElementById('result');
function compile(gl, type, source, family, stage) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) || 'unknown compile error';
    gl.deleteShader(shader);
    throw new Error(family + ' ' + stage + ' shader compilation failed: ' + message);
  }
  return shader;
}
function texture(gl, unit, internalFormat, width, height, format, data) {
  const value = gl.createTexture();
  gl.activeTexture(gl.TEXTURE0 + unit);
  gl.bindTexture(gl.TEXTURE_2D, value);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
  gl.texImage2D(gl.TEXTURE_2D, 0, internalFormat, width, height, 0, format, gl.UNSIGNED_BYTE, data);
  return value;
}
function uniform(gl, program, name, setter, ...values) {
  const location = gl.getUniformLocation(program, name);
  if (location !== null) gl[setter](location, ...values);
}
function testWebGlidePaletteShading(gl, program) {
  const palette = new Uint8Array(256 * 4);
  for (let index = 0; index < 256; index += 1) palette[index * 4 + 3] = 255;
  palette.set([200, 40, 20, 255], 10 * 4);
  palette.set([32, 8, 4, 255], 11 * 4);
  palette.set([12, 240, 24, 255], 224 * 4);

  const colormap = new Uint8Array(256 * 64);
  for (let row = 0; row < 64; row += 1) {
    for (let index = 0; index < 256; index += 1) colormap[row * 256 + index] = index;
  }
  colormap[63 * 256 + 10] = 11;

  texture(gl, 0, gl.R8UI, 2, 1, gl.RED_INTEGER, new Uint8Array([10, 224]));
  texture(gl, 1, gl.RGBA8, 1, 1, gl.RGBA, new Uint8Array([0, 0, 0, 0]));
  texture(gl, 3, gl.RGBA8, 256, 1, gl.RGBA, palette);
  texture(gl, 4, gl.R8UI, 256, 64, gl.RED_INTEGER, colormap);

  const target = texture(gl, 5, gl.RGBA8, 2, 1, gl.RGBA, null);
  const framebuffer = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, target, 0);
  if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
    throw new Error('WebGlide palette test framebuffer is incomplete');
  }

  const vertices = new Float32Array([
    -1, -1, 0, 0, 0, 0.5, 0.5,
     3, -1, 0, 2, 0, 0.5, 0.5,
    -1,  3, 0, 0, 2, 0.5, 0.5,
  ]);
  const vao = gl.createVertexArray();
  const buffer = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
  for (const [location, size, offset] of [[0, 3, 0], [1, 2, 12], [2, 2, 20]]) {
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, size, gl.FLOAT, false, 28, offset);
  }

  gl.useProgram(program);
  uniform(gl, program, 'u_mvp', 'uniformMatrix4fv', false, new Float32Array([
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
  ]));
  uniform(gl, program, 'u_indices', 'uniform1i', 0);
  uniform(gl, program, 'u_lightmap', 'uniform1i', 1);
  uniform(gl, program, 'u_palette', 'uniform1i', 3);
  uniform(gl, program, 'u_colormap', 'uniform1i', 4);
  uniform(gl, program, 'u_scroll', 'uniform2f', 0, 0);
  uniform(gl, program, 'u_turbscale', 'uniform2f', 1, 1);
  uniform(gl, program, 'u_turbtime', 'uniform1f', 0);
  uniform(gl, program, 'u_alpha', 'uniform1f', 1);
  uniform(gl, program, 'u_light', 'uniform1f', 1);
  uniform(gl, program, 'u_flags', 'uniform1i', 10);
  uniform(gl, program, 'u_fullbright', 'uniform1i', 224);
  uniform(gl, program, 'u_filter', 'uniform1i', 0);
  uniform(gl, program, 'u_debug', 'uniform1i', 0);
  uniform(gl, program, 'u_fogdensity', 'uniform1f', 0);
  uniform(gl, program, 'u_fogcolor', 'uniform3f', 0, 0, 0);
  uniform(gl, program, 'u_dither', 'uniform3f', 0, 0, 0);
  uniform(gl, program, 'u_lodbias', 'uniform1f', 0);
  uniform(gl, program, 'u_mipdither', 'uniform1f', 0);
  gl.viewport(0, 0, 2, 1);
  gl.disable(gl.BLEND);
  gl.disable(gl.DEPTH_TEST);
  gl.drawArrays(gl.TRIANGLES, 0, 3);

  const pixels = new Uint8Array(8);
  gl.readPixels(0, 0, 2, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
  const expected = [32, 8, 4, 255, 12, 240, 24, 255];
  if (!expected.every((value, index) => Math.abs(value - pixels[index]) <= 1)) {
    throw new Error('WebGlide palette shading mismatch: expected ' + expected + ', got ' + [...pixels]);
  }
}
try {
  const canvas = document.createElement('canvas');
  const gl = canvas.getContext('webgl2', { antialias: false });
  if (!gl) throw new Error('WebGL2 context creation failed');
  let webglideWorld = null;
  for (const source of sources) {
    const vertex = compile(gl, gl.VERTEX_SHADER, source.vertex, source.name, 'vertex');
    const fragment = compile(gl, gl.FRAGMENT_SHADER, source.fragment, source.name, 'fragment');
    const program = gl.createProgram();
    gl.attachShader(program, vertex);
    gl.attachShader(program, fragment);
    gl.bindAttribLocation(program, 0, 'a_position');
    gl.bindAttribLocation(program, 1, 'a_texcoord');
    gl.bindAttribLocation(program, 2, 'a_lmcoord');
    gl.bindAttribLocation(program, 3, 'a_color');
    gl.linkProgram(program);
    gl.deleteShader(vertex);
    gl.deleteShader(fragment);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      const message = gl.getProgramInfoLog(program) || 'unknown link error';
      gl.deleteProgram(program);
      throw new Error(source.name + ' shader link failed: ' + message);
    }
    if (source.name === 'webglide_world') webglideWorld = program;
    else gl.deleteProgram(program);
  }
  if (!webglideWorld) throw new Error('missing WebGlide world shader');
  testWebGlidePaletteShading(gl, webglideWorld);
  gl.deleteProgram(webglideWorld);
  const error = gl.getError();
  if (error !== gl.NO_ERROR) throw new Error('WebGL2 error after engine shader test: 0x' + error.toString(16));
  document.body.dataset.result = 'pass';
  output.textContent = JSON.stringify({ profile: gl.getParameter(gl.VERSION), shaders: sources.map(({ name }) => name) });
} catch (error) {
  document.body.dataset.result = 'fail';
  output.textContent = error.stack || error.message;
}
</script></body></html>
`;
}

async function main() {
  const output = process.argv[2];
  if (!output) throw new Error('usage: webgl-engine-shader-smoke.mjs <output.html>');
  const programs = await extractEngineWebGLPrograms();
  await writeFile(output, renderEngineShaderSmokePage(programs));
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
  });
}
