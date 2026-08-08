import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import { nonBlackPixelRatio } from '../lib/webgl-diagnostics.js';

test('black-frame detector distinguishes visible and dark frames', () => {
  assert.equal(nonBlackPixelRatio(new Uint8Array(16)), 0);
  assert.equal(nonBlackPixelRatio(new Uint8Array([
    64, 128, 192, 255,
    64, 128, 192, 255,
  ])), 1);
  assert.equal(nonBlackPixelRatio(new Uint8Array([
    0, 0, 0, 255,
    64, 128, 192, 255,
  ])), 0.5);
});

test('launcher blocks startup on a failed renderer self-test', async () => {
  const app = await readFile(new URL('../app.js', import.meta.url), 'utf8');
  assert.match(app, /runWebGLDiagnostics\(\)/);
  assert.match(app, /!state\.rendererReady/);
  assert.match(app, /\[renderer:error\]/);
});

test('framebuffer self-test validates a generated world draw', async () => {
  const diagnostics = await readFile(new URL('../lib/webgl-diagnostics.js', import.meta.url), 'utf8');
  assert.match(diagnostics, /gl\.clearColor\(0, 0, 0, 1\)/);
  assert.match(diagnostics, /gl\.drawArrays\(gl\.TRIANGLES, 0, 6\)/);
  assert.match(diagnostics, /generated world draw returned/);
  assert.match(diagnostics, /gamma post-process returned/);
  assert.match(diagnostics, /palette post-process returned/);
});

test('engine WebGL profile uses shared high-precision shaders and CPU brush fallback', async () => {
  const [header, shader, postprocess, renderer] = await Promise.all([
    readFile(new URL('../../engine/h2shared/gl_shader.h', import.meta.url), 'utf8'),
    readFile(new URL('../../engine/h2shared/gl_shader.c', import.meta.url), 'utf8'),
    readFile(new URL('../../engine/h2shared/gl_postprocess.c', import.meta.url), 'utf8'),
    readFile(new URL('../../engine/hexen2/gl_rmain.c', import.meta.url), 'utf8'),
  ]);

  assert.match(header, /#define GLSL_PROFILE_VERSION\s+"#version 300 es\\n"/);
  assert.match(header, /GLSL_FRAGMENT_PRECISION[\s\S]*"precision highp float;\\n"/);
  assert.match(shader, /GLSL_FRAG_HEADER/);
  assert.match(postprocess, /GLSL_FRAG_HEADER/);
  assert.match(renderer, /gl_renderer_caps\.profile == GL_RENDERER_WEBGL2[\s\S]*return false;/);
});
