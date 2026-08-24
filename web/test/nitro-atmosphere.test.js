import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const root = process.cwd();
const read = (path) => readFileSync(join(root, path), 'utf8');
const header = read('engine/h2shared/wgpu_nitro.h');
const renderer = read('engine/hexen2/r_webgpu.c');
const backend = read('engine/web/webgpu_nitro.js');
const world = read('engine/h2shared/wgpu_world.c');
const entities = read('engine/h2shared/wgpu_entity.c');

test('Nitro atmosphere controls are archived and independently optional', () => {
  for (const name of [
    'r_nitro_fogbands',
    'r_nitro_haze',
    'r_nitro_paletteshifts',
  ]) {
    assert.match(header, new RegExp(`extern cvar_t\\s+${name}`));
    assert.match(renderer, new RegExp(`${name} = \\{"${name}".*CVAR_ARCHIVE`));
    assert.match(renderer, new RegExp(`Cvar_RegisterVariable\\(&${name}\\)`));
  }
  assert.match(renderer, /r_nitro_haze = \{"r_nitro_haze", "0"/);
});

test('fog is depth-banded and optional haze is world-space localized', () => {
  assert.match(backend, /if \(frame\.fogBands >= 2\.0\)/);
  assert.match(backend, /fog = floor\(fog \* steps \+ 0\.5\) \/ steps/);
  assert.match(backend, /if \(frame\.haze > 0\.0\)/);
  assert.match(backend, /sin\(position\.x \* 0\.021 \+ frame\.time \* 0\.11\)/);
  assert.match(backend, /fog = max\(fog, pocket \* frame\.haze \* 0\.45\)/);
});

test('atmosphere and full-screen shifts return to the authored palette', () => {
  assert.match(backend,
    /paletteQuantize\(mix\(rgb, frame\.fogColor, fog\)\)/);
  assert.match(backend,
    /scan\.paletteShifts != 0\.0 && scan\.tint\.a > 0\.0[\s\S]*paletteQuantize\(rgb\)/);
  assert.match(backend, /binding: 4, resource: state\.paletteTexture\.createView\(\)/);
  assert.match(backend, /binding: 5, resource: state\.paletteLutTexture\.createView\(\)/);
});

test('contents shifts are current before scene parameters are captured', () => {
  const contents = renderer.indexOf('V_SetContentsColor(wgpu_viewleaf->contents)');
  const setup = renderer.indexOf('WGPU_SetupScene (&scene)');
  assert.ok(contents >= 0 && contents < setup);
});

test('scene parameter offsets include atmosphere without changing scan uniform size', () => {
  assert.match(backend, /frame\[18\] = floats\[45\]/);
  assert.match(backend, /frame\[19\] = floats\[46\]/);
  assert.match(backend, /scan\[9\] = floats\[50\]/);
  assert.match(backend, /const scan = new Float32Array\(12\)/);
});

test('basic transparency is active before the later supernatural-liquid phase', () => {
  assert.match(world, /WGPUWorld_LiquidAlpha/);
  assert.match(world, /source->alpha \* alpha/);
  assert.match(entities, /flags \|= NITROMODEL_BLEND_ALPHA/);
  assert.match(backend, /worldBlendPipeline/);
  assert.match(backend, /modelAlphaPipeline/);
  assert.match(backend, /depthWriteEnabled: false/);
});
