import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import { nonBlackPixelRatio } from '../lib/webgl-diagnostics.js';
import { extractEngineWebGLPrograms } from '../../scripts/webgl-engine-shader-smoke.mjs';

test('black-frame detector distinguishes visible and dark frames', () => {
  // Guard cases (null, undefined, zero length, and length < 4)
  assert.equal(nonBlackPixelRatio(null), 0);
  assert.equal(nonBlackPixelRatio(undefined), 0);
  assert.equal(nonBlackPixelRatio(new Uint8Array(0)), 0);
  assert.equal(nonBlackPixelRatio(new Uint8Array(3)), 0);

  // All-black pixels (16-byte array with all zeros)
  assert.equal(nonBlackPixelRatio(new Uint8Array(16)), 0);

  // Normal frames
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
  assert.match(renderer, /if \(gl_renderer_caps\.profile == GL_RENDERER_WEBGL2\)\s*\n\s*return false;/);
});

test('headless smoke gate compiles the actual engine WebGL shader sources', async () => {
  const programs = await extractEngineWebGLPrograms();
  const byName = Object.fromEntries(programs.map((program) => [program.name, program]));

  assert.deepEqual(programs.map(({ name }) => name), [
    '2d', 'flat', 'world', 'world_opaque', 'alias', 'particle', 'sky',
    'postprocess', 'bloom_bright', 'bloom_down', 'bloom_up',
  ]);
  assert.match(byName.world.fragment, /vec4 BicubicLightmap/);
  assert.match(byName.world.fragment, /float Caustics/);
  assert.match(byName.postprocess.fragment, /vec4 fxaa/);
  assert.match(byName.postprocess.fragment, /HDR tonemapping/);
  for (const program of programs) {
    assert.match(program.vertex, /^#version 300 es\n#define BINDLESS 0\n/);
    assert.match(program.fragment, /^#version 300 es\n#define BINDLESS 0\n/);
    assert.doesNotMatch(program.vertex, /#version 430/);
    assert.doesNotMatch(program.fragment, /#version 430/);
  }
});
