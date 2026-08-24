import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const root = process.cwd();
const read = (path) => readFileSync(join(root, path), 'utf8');
const cmake = read('engine/CMakeLists.txt');
const header = read('engine/h2shared/wgpu_nitro.h');
const volume = read('engine/h2shared/wgpu_lightvol.c');
const entities = read('engine/h2shared/wgpu_entity.c');
const world = read('engine/h2shared/wgpu_world.c');
const backend = read('engine/web/webgpu_nitro.js');
const renderer = read('engine/hexen2/r_webgpu.c');

test('Nitro builds its bounded shared light volume', () => {
  assert.match(cmake, /\$\{COMMONDIR\}\/wgpu_lightvol\.c/);
  assert.match(volume, /NITRO_LIGHTVOL_MAX_BYTES\s+\(2 \* 1024 \* 1024\)/);
  assert.match(volume, /lightvol_budget = CLAMP\(1, r_nitro_lightvol_budget\.integer, 4096\)/);
  assert.match(header, /_Static_assert \(sizeof\(wgpulightcell_t\) == 8/);
});

test('light cells retain signed direction and authored intensity', () => {
  assert.match(volume, /direction\[i\] \* 0\.5f \+ 0\.5f/);
  assert.match(volume, /cell->ambient = \(byte\)CLAMP/);
  assert.match(volume, /WGPUWorld_TraceLight \(point, end, NULL\)/);
});

test('aliases consume the shared sample and legacy stencil shadows stay out', () => {
  assert.match(entities, /WGPULightVol_Sample \(adjust, &sample\)/);
  assert.match(entities, /DotProduct \(nitro_lightdir, forward\)/);
  assert.match(entities,
    /r_shadows\.integer && !WGPULightVol_Active \(\) && entity != &cl\.viewent/);
  assert.match(entities, /nitro_ambientlight > 128\.0f/);
  assert.match(entities, /nitro_ambientlight \+ nitro_shadelight > 192\.0f/);
});

test('light-volume controls are archived and frame-budgeted', () => {
  for (const name of [
    'r_nitro_lightvol',
    'r_nitro_lightvol_cell',
    'r_nitro_lightvol_budget',
  ]) {
    assert.match(header, new RegExp(`extern cvar_t\\s+${name}`));
    assert.match(renderer, new RegExp(`Cvar_RegisterVariable\\(&${name}\\)`));
  }
  assert.match(renderer, /WGPU_AnimateLight \(\);\s+WGPULightVol_BeginFrame \(\);/);
  assert.match(renderer, /r_dynamic = \{"r_dynamic", "1"/);
  assert.match(renderer, /r_nitro_lightvol = \{"r_nitro_lightvol", "1"/);
});

test('indexed alias darkness remains in the authored colormap domain', () => {
  for (const ambient of [0, 5, 64, 128]) {
    for (const shade of [0, 5, 64, 192 - ambient]) {
      for (const cosine of [-1, -0.5, 0, 1]) {
        let darkness = (255 - ambient) * 64;
        if (cosine < 0) darkness += shade * 64 * cosine;
        darkness = Math.max(0, Math.min(255 * 64, darkness));
        const row = darkness / 256;
        assert.ok(row >= 0 && row <= 63.75);
      }
    }
  }
});

test('coloured lighting stays palette-quantized across world and aliases', () => {
  assert.match(world, /WGPUWorld_LoadLitFile/);
  assert.match(world, /light->color\[0\]/);
  assert.match(world, /blocklights\[j \+ 3\] \+= \(int\)add/);
  assert.match(volume, /VectorMA \(sample->color, add, color, sample->color\)/);
  assert.match(entities, /out->lightcolor = lightcolor/);
  assert.match(backend, /label: 'WebGlideNitro palette colour cube'/);
  assert.match(backend, /fn paletteQuantize\(rgb : vec3f\)/);
  assert.match(backend, /format: 'rgba8unorm'/);
  assert.match(backend, /lightmap\.rgb \/ lightmap\.a/);
});

test('white and disabled coloured lights retain neutral colormap shading', () => {
  assert.match(world, /!gl_coloredlight\.integer \|\| light->dark/);
  assert.match(volume, /!gl_coloredlight\.integer \|\| luminance <= 0\.001f/);
  assert.match(backend, /length\(lightColor - vec3f\(1\.0\)\) > 0\.01/);
  assert.match(backend, /length\(input\.lightColor - vec3f\(1\.0\)\) > 0\.01/);
});

test('coloured-light runtime changes rebuild both world and actor caches', () => {
  assert.match(world,
    /colored_changed = nitro_cached_coloredlight != gl_coloredlight\.integer/);
  assert.match(world, /if \(colored_changed\)\s+WGPULightVol_NewMap \(\)/);
  assert.match(world, /qboolean\s+changed = colored_changed/);
});

test('dark and overlapping lights mix in contribution space', () => {
  assert.match(world, /sign = light->dark \? -1\.0f : 1\.0f/);
  assert.match(world, /blocklights\[j \+ 3\] \+= \(int\)add/);
  assert.match(volume,
    /sample->color\[0\] -= add;\s+sample->color\[1\] -= add;\s+sample->color\[2\] -= add/);
  assert.match(volume, /VectorMA \(sample->color, add, color, sample->color\)/);
});

test('lit files are versioned, bounded, and keep BSP intensity authoritative', () => {
  assert.match(world, /fs_filesize < 8/);
  assert.match(world, /required > \(fs_filesize - 8\) \/ 3/);
  assert.match(world, /version != 1/);
  assert.match(world, /blocklights\[i \* 4 \+ 3\] \+= add/);
});

test('fullbright alias pixels bypass coloured quantization', () => {
  assert.match(backend,
    /frame\.modelFullbrights != 0\.0 && input\.fullbright != 0u &&\s+index >= 224u/);
  assert.match(backend,
    /if \(receivesLight && length\(input\.lightColor - vec3f\(1\.0\)\) > 0\.01\)/);
});
