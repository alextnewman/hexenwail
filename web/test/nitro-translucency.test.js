import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

const read = (path) => readFileSync(path, 'utf8');
const header = read('engine/h2shared/wgpu_nitro.h');
const entities = read('engine/h2shared/wgpu_entity.c');
const world = read('engine/h2shared/wgpu_world.c');

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
