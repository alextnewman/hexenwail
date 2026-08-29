import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const read = (path) => readFileSync(path, 'utf8');
const header = read('engine/h2shared/wgpu_nitro.h');
const entities = read('engine/h2shared/wgpu_entity.c');
const world = read('engine/h2shared/wgpu_world.c');
const backend = read('engine/web/webgpu_nitro.js');
const softwareModel = read('engine/h2shared/model.c');
const webglModel = read('engine/h2shared/gl_model.c');
const nitroModel = read('engine/h2shared/nitro_model.c');
const menu = read('engine/hexen2/menu.c');

test('Nitro sprites always preserve their authored clear index', () => {
  assert.match(
    entities,
    /unsigned int\s+texflags = NITROTEX_ALPHA;[\s\S]*if \(model->flags & EF_HOLEY\)[\s\S]*texflags \|= NITROTEX_HOLEY;/,
  );
});

test('Nitro preserves authored DRF translucency independently of liquid alpha', () => {
  assert.match(header, /#define NITRO_DRF_ALPHA\s+0\.5f/);

  for (const source of [entities, world]) {
    assert.doesNotMatch(
      source,
      /if \(entity->drawflags & DRF_TRANSLUCENT\)[\s\S]{0,80}r_wateralpha\.value/,
    );
  }

  assert.equal(
    (entities.match(/alpha = NITRO_DRF_ALPHA;/g) ?? []).length,
    2,
    'alias and sprite entities use the authored half blend',
  );
  assert.equal(
    (world.match(/alpha = NITRO_DRF_ALPHA;/g) ?? []).length,
    1,
    'brush entities use the authored half blend',
  );
});

test('magic missile models restore their packed colour and alpha semantics', () => {
  for (const [name, loader] of [
    ['software', softwareModel],
    ['WebGlide', webglModel],
    ['Nitro', nitroModel],
  ]) {
    assert.equal(
      (loader.match(/mod->flags & EF_MAGICMISSILE[\s\S]{0,80}EF_SPECIAL_TRANS/g) ?? []).length,
      2,
      `${name} marks both magic-missile alias formats as special translucent`,
    );
    assert.doesNotMatch(loader, /q_strcasecmp \(mod->name, "models\/ball\.mdl"\)/);
  }
  assert.match(header, /#define NITROTEX_SPECIAL_TRANS\s+64u/);
  assert.match(entities, /model->flags & EF_SPECIAL_TRANS[\s\S]{0,80}NITROTEX_SPECIAL_TRANS/);
  assert.match(backend, /packedIndex >> 4u/);
  assert.match(backend, /colorPercent\[packedIndex & 15u\] \/ 255\.0/);
});

test('Moody restores colored dynamic glows', () => {
  for (const name of ['r_nitro_coloredlight', 'r_nitro_extra_dynamic_lights']) {
    assert.match(menu, new RegExp(`Cvar_VariableValue\\("${name}"\\) != 0`));
    assert.match(menu, new RegExp(`Cvar_SetValue \\("${name}", 1\\)`));
  }
});
