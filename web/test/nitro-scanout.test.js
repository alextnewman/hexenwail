import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const root = process.cwd();
const read = (path) => readFileSync(join(root, path), 'utf8');
const header = read('engine/h2shared/wgpu_nitro.h');
const renderer = read('engine/hexen2/r_webgpu.c');
const backend = read('engine/web/webgpu_nitro.js');

test('Nitro scan-out effects are archived and independently optional', () => {
  for (const name of [
    'r_nitro_dither',
    'r_nitro_resolve',
    'r_nitro_persistence',
  ]) {
    assert.match(header, new RegExp(`extern cvar_t\\s+${name}`));
    assert.match(renderer, new RegExp(`${name} = \\{"${name}".*CVAR_ARCHIVE`));
    assert.match(renderer, new RegExp(`Cvar_RegisterVariable\\(&${name}\\)`));
  }
  assert.match(renderer, /r_nitro_persistence = \{"r_nitro_persistence", "0\.06"/);
  assert.match(renderer, /CLAMP \(0\.0f, r_nitro_persistence\.value, 0\.25f\)/);
});

test('scan-out protects edges while resolving dark low-contrast pixels', () => {
  assert.match(backend, /let localRange =/);
  assert.match(backend, /let edgeGuard = 1\.0 - smoothstep\(0\.04, 0\.16, localRange\)/);
  assert.match(backend, /scan\.resolve \* 0\.22 \* edgeGuard/);
  assert.match(backend, /let nearBlack = 1\.0 - smoothstep\(0\.08, 0\.35, finalLuma\)/);
  assert.match(backend, /let fogGradient =/);
  assert.match(backend, /let levels = vec3f\(31\.0, 63\.0, 31\.0\)/);
});

test('history retains coloured light only and resets with scene identity', () => {
  assert.match(backend, /let lightTrail =[\s\S]*historyChroma/);
  assert.match(backend, /scan\.persistence \* lightTrail/);
  assert.match(backend, /state\.historyValid = false;\s+Nitro\.rebuildFrameBindGroup/);
  assert.match(backend, /copyTextureToTexture\(/);
  assert.match(backend, /state\.historyValid \? floats\[49\] : 0\.0/);
});

test('scan-out uniform remains a 48-byte WGSL and JavaScript contract', () => {
  const params = backend.match(/struct ScanParams \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(params, 'ScanParams is defined');
  assert.doesNotMatch(params, /vec3f/,
    'a vec3 after persistence would align to byte 48 and grow the struct to 64 bytes');
  assert.match(params, /persistence : f32,\s+pad0 : f32,\s+pad1 : f32,\s+pad2 : f32,/);
  assert.match(backend, /label: 'WebGlideNitro scan-out uniform',\s+size: 48,/);
  assert.match(backend, /const scan = new Float32Array\(12\)/);
});

test('the processed scene is drawn before the crisp UI pass', () => {
  const sceneDraw = backend.indexOf('pass.setPipeline(state.scanoutPipeline)');
  const uiDraw = backend.indexOf('pass.setPipeline(state.uiPipeline)');
  assert.ok(sceneDraw >= 0 && uiDraw > sceneDraw);
});
