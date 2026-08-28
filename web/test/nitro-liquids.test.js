import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = (path) => readFileSync(path, 'utf8');
const header = read('engine/h2shared/wgpu_nitro.h');
const renderer = read('engine/hexen2/r_webgpu.c');
const world = read('engine/h2shared/wgpu_world.c');
const backend = read('engine/web/webgpu_nitro.js');

test('Nitro liquid effects are archived and independently reversible', () => {
  for (const name of [
    'r_nitro_liquidmotion',
    'r_nitro_liquidstipple',
    'r_nitro_liquidrefract',
    'r_nitro_liquidglow',
  ]) {
    assert.match(header, new RegExp(`extern cvar_t\\s+${name}`));
    assert.match(renderer, new RegExp(`${name} = \\{"${name}".*CVAR_ARCHIVE`));
    assert.match(renderer, new RegExp(`Cvar_RegisterVariable\\(&${name}\\)`));
  }
  assert.match(renderer, /r_nitro_liquidrefract = \{"r_nitro_liquidrefract", "0\.12"/);
});

test('world textures retain a compact material identity', () => {
  assert.match(header, /#define NITROTEX_LIQUID_MASK\s+\(3u <</);
  assert.match(world, /NITROTEX_LIQUID_SLIME/);
  assert.match(world, /NITROTEX_LIQUID_LAVA/);
  assert.match(world, /NITROTEX_LIQUID_PORTAL/);
  assert.match(world, /WGPUWorld_LiquidClass \(world, texture\)/);
  assert.match(world, /case CONTENTS_SLIME: material = NITROLIQUID_SLIME/);
  assert.match(world, /case CONTENTS_LAVA: material = NITROLIQUID_LAVA/);
  assert.match(world, /"portal", 6\)[\s\S]*return NITROLIQUID_PORTAL[\s\S]*for \(li = 0;/,
    'named portals override the generic water contents class');
});

test('liquid motion is material-specific with the authored warp as its off state', () => {
  assert.match(backend, /let legacy = vec2f\(sin\(texel\.y \* 0\.125/);
  assert.match(backend, /material == 1u[\s\S]*frame\.time \* 0\.55/);
  assert.match(backend, /material == 2u[\s\S]*frame\.time \* 0\.70/);
  assert.match(backend, /material == 3u[\s\S]*frame\.time \* 2\.20/);
  assert.match(backend, /mix\(legacy, identity, frame\.liquidMotion\)/);
});

test('liquid translucency uses stable ordered coverage', () => {
  assert.match(backend, /let coverage = mix\(1\.0, alpha, frame\.liquidStipple\)/);
  assert.match(backend, /if \(threshold > coverage\)[\s\S]*discard/);
  assert.match(backend, /alpha \/= coverage/);
  assert.match(world, /alpha \+= \(0\.82f - alpha\) \* style/);
  assert.match(world, /1\.0f - 0\.12f \* style/);
  assert.match(world, /1\.0f - 0\.06f \* style/);
  assert.match(world, /r_wateralpha\.value >= 1\.0f/,
    'authored alpha values just below one must not be replaced by Nitro defaults');
  assert.doesNotMatch(world, /!translucent && r_wateralpha\.value >= 1\.0f/,
    'authored translucent water must receive Nitro coverage at the default alpha');
  assert.match(world, /!translucent && !strstr \(name, "water"\)[\s\S]*return 1\.0f/,
    'opaque water-content rtex surfaces retain authored opacity');
});

test('liquid refraction samples only retained scene colour and returns to the palette', () => {
  assert.match(backend, /@group\(0\) @binding\(7\) var historyTexture/);
  assert.match(backend, /frame\.historyValid != 0\.0/);
  assert.match(backend, /textureLoad\(historyTexture, historyTexel, 0\)/);
  assert.match(backend,
    /paletteQuantize\(mix\(rgb, history, frame\.liquidRefract \* finalScale\)\)/);
  assert.match(backend,
    /fogDensity : f32,\s+liquidMotion : f32,\s+liquidStipple : f32,\s+liquidRefract : f32,\s+historyValid : f32,/);
  assert.match(backend, /const frame = new Float32Array\(36\)/);
  assert.match(backend, /frame\[31\] = state\.historyValid \? 1\.0 : 0\.0/);
});

test('lava and portals receive reversible palette-domain luminosity', () => {
  assert.match(backend, /frame\.liquidGlow > 0\.0/);
  assert.match(backend, /material == 2u \|\| material == 3u/);
  assert.match(backend, /let breath = 0\.82 \+ 0\.18 \* sin\(frame\.time/);
  assert.match(backend, /row = min\(row,[\s\S]*frame\.liquidGlow \* breath/);
  assert.match(backend, /label: 'WebGlideNitro frame uniform',\s+size: 144/);
  assert.doesNotMatch(backend, /liquidGlow[\s\S]{0,200}(bloom|hdr)/i);
});
