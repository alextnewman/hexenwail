import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const read = (path) => readFileSync(path, 'utf8');
const header = read('engine/h2shared/wgpu_nitro.h');
const renderer = read('engine/hexen2/r_webgpu.c');
const backend = read('engine/web/webgpu_nitro.js');

test('Nitro spell treatments and luminous haze are reversible', () => {
  for (const name of ['r_nitro_spelleffects', 'r_nitro_glowhaze']) {
    assert.match(header, new RegExp(`extern cvar_t\\s+${name}`));
    assert.match(renderer, new RegExp(`${name} = \\{"${name}".*CVAR_ARCHIVE`));
    assert.match(renderer, new RegExp(`Cvar_RegisterVariable\\(&${name}\\)`));
  }
  assert.match(renderer, /r_nitro_spelleffects = \{"r_nitro_spelleffects", "1"/);
  assert.match(renderer, /r_nitro_glowhaze = \{"r_nitro_glowhaze", "0\.35"/);
});

test('particle instances carry authored spell families to one GPU-pulled batch', () => {
  for (const family of ['FIRE', 'ICE', 'POISON', 'NECRO']) {
    assert.match(header, new RegExp(`NITROPARTICLE_${family}`));
    assert.match(renderer, new RegExp(`return NITROPARTICLE_${family}`));
  }
  assert.match(header, /_Static_assert \(sizeof\(wgpuparticle_t\) == 24/);
  assert.match(backend, /PARTICLE_STRIDE: 24/);
  assert.match(backend, /shaderLocation: 3, offset: 20, format: 'uint32'/);
  assert.match(backend, /pass\.draw\(6, particleCount\)/);
});

test('spell families have distinct silhouettes and temporal signatures', () => {
  const effectShader = backend.match(/effectShader: `([\s\S]*?)`,\s+particleShader:/)?.[1];
  const particleShader = backend.match(/particleShader: `([\s\S]*?)`,\s+scanoutShader:/)?.[1];
  assert.ok(effectShader, 'effect shader is defined');
  assert.ok(particleShader, 'particle shader is defined');
  assert.doesNotMatch(effectShader, /output\.(local|style)/);
  assert.match(particleShader, /output\.local = corner;\s+output\.style = style;/);
  assert.match(backend, /frame\.time \* select\(4\.0, 11\.0, style == 1u\)/);
  assert.match(backend, /style == 2u[\s\S]*corner\.x \* 0\.48/);
  assert.match(backend, /input\.style == 3u[\s\S]*stipple/);
  assert.match(backend, /abs\(radius - 0\.58\)/);
  assert.match(backend, /mask = mix\(1\.0, mask, particle\.up\.w\)/);
});

test('bright palette colours diffuse restrained haze before scan-out', () => {
  assert.match(backend, /fn radianceWeight/);
  assert.match(backend, /scan\.glowHaze > 0\.0/);
  assert.match(backend, /let offsets = array<vec2i, 4>/);
  assert.match(backend, /rgb = paletteQuantize\(mix\(rgb, max\(rgb, hazeColor/);
  assert.match(backend, /scan\[10\] = floats\[55\]/);
});
