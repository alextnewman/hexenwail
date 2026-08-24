import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const aliasRenderer = readFileSync(join(repoRoot, 'engine/h2shared/gl2_alias.c'), 'utf8');
const webglRenderer = readFileSync(join(repoRoot, 'engine/hexen2/r_webgl2.c'), 'utf8');
const nitroEntityRenderer = readFileSync(join(repoRoot, 'engine/h2shared/wgpu_entity.c'), 'utf8');
const worldRenderer = readFileSync(join(repoRoot, 'engine/h2shared/gl2_world.c'), 'utf8');
const audioBackend = readFileSync(join(repoRoot, 'engine/h2shared/snd_web.c'), 'utf8');

test('WebGlide converts alias 16.16 skin coordinates to normalized UVs', () => {
  const sScale = aliasRenderer.match(
    /out->s\s*=\s*st->s\s*\*\s*\(1\.0f\s*\/\s*([0-9.]+)f\)\s*\*\s*iw\s*;/,
  );
  const tScale = aliasRenderer.match(
    /out->t\s*=\s*st->t\s*\*\s*\(1\.0f\s*\/\s*([0-9.]+)f\)\s*\*\s*ih\s*;/,
  );
  assert.ok(sScale && tScale, 'both alias UV axes remove the fixed-point scale');
  const skinSize = 96;
  const texelS = 48;
  const texelT = 24;
  const fixedS = texelS * 65536;
  const fixedT = texelT * 65536;
  assert.equal(fixedS / Number(sScale[1]) / skinSize, texelS / skinSize);
  assert.equal(fixedT / Number(tScale[1]) / skinSize, texelT / skinSize);
});

test('WebAudio lifecycle promises cannot escape as unhandled rejections', () => {
  assert.match(audioBackend, /context\.resume\(\)\.catch/);
  assert.match(audioBackend, /context\.suspend\(\)\.catch/);
  assert.match(audioBackend, /Module\.webAudioContext\.close\(\)\.catch/);
  assert.match(audioBackend, /Module\.webAudioResumeController\.abort\(\)/);
});

test('WebGlide never treats hunk-owned sprite payloads as cache allocations', () => {
  const spriteDraw = aliasRenderer.match(
    /void GL2_DrawSpriteModel \(entity_t \*entity\)([\s\S]*?)\n\}/,
  )?.[1];
  assert.ok(spriteDraw, 'sprite draw function is present');
  assert.match(spriteDraw, /model->type != mod_sprite/);
  assert.match(spriteDraw, /psprite\s*=\s*\(const msprite_t \*\)\s*model->cache\.data/);
  assert.doesNotMatch(spriteDraw, /Mod_Extradata\s*\(/);
});

test('WebGlide streams dynamic geometry through non-overlapping buffer ranges', () => {
  assert.match(worldRenderer, /gl2_world_ibo_offset \+ count > gl2_total_indices/);
  assert.match(worldRenderer,
    /glBufferSubData \(GL_ELEMENT_ARRAY_BUFFER,\s*\n\s*\(GLintptr\)gl2_world_ibo_offset/);
  assert.match(worldRenderer, /gl2_world_ibo_offset \+= count/);

  assert.match(aliasRenderer,
    /gl2_model_vbo_offset \+ gl2_batch_count > GL2_MAX_BATCH_VERTS/);
  assert.match(aliasRenderer,
    /glBufferSubData \(GL_ARRAY_BUFFER,\s*\n\s*\(GLintptr\)gl2_model_vbo_offset/);
  assert.match(aliasRenderer,
    /glDrawArrays \(GL_TRIANGLES, gl2_model_vbo_offset, gl2_batch_count\)/);
});

test('GPU renderers publish player light levels before view-model draw guards', () => {
  const webglViewModel = webglRenderer.match(
    /static void GL2_DrawViewModel \(void\)([\s\S]*?)\n\}/,
  )?.[1];
  const nitroViewModel = nitroEntityRenderer.match(
    /void WGPUEntity_DrawViewModel \(void\)([\s\S]*?)\n\}/,
  )?.[1];
  assert.ok(webglViewModel && nitroViewModel, 'both view-model functions are present');

  for (const viewModel of [webglViewModel, nitroViewModel]) {
    const lightLevel = viewModel.indexOf('cl.light_level =');
    const drawGuard = viewModel.indexOf('cl.v.health <= 0');
    assert.ok(lightLevel >= 0, 'player light level is published');
    assert.ok(lightLevel < drawGuard, 'light level is published before optional drawing exits');
  }
});
