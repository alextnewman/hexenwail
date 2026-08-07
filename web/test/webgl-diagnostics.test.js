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
});
