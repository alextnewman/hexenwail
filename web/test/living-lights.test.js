import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const client = readFileSync('engine/hexen2/cl_main.c', 'utf8');
const nitro = readFileSync('engine/hexen2/r_webgpu.c', 'utf8');

test('Nitro luminous entities feed the shared dynamic-light pool', () => {
  assert.match(nitro, /static void Nitro_AddEntityGlowLight/);
  assert.match(nitro, /flags & EF_ILLUMINATE/);
  assert.match(nitro, /flags & XF_TORCH_GLOW/);
  assert.match(nitro, /flags & XF_MISSILE_GLOW/);
  assert.match(nitro, /flags & \(XF_GLOW \| EF_GLOW\)/);
  assert.match(nitro, /Nitro_AddEntityGlowLight \(entity, i\)/);
  assert.doesNotMatch(client, /Nitro_AddEntityGlowLight/);
});

test('glow rhythms prefer authored light styles with deterministic fallbacks', () => {
  const helper = nitro.match(
    /static float Nitro_GlowLightScale[\s\S]*?\n}\n\nstatic void Nitro_AddEntityGlowLight/,
  )?.[0];
  assert.ok(helper, 'glow light modulation helper is defined');
  assert.match(helper, /cl_lightstyle\[style\]\.length/);
  assert.match(helper, /cl_lightstyle\[style\]\.map\[0\] >= '1'/);
  assert.match(helper, /\* 22\.0f \/ 255\.0f/);
  assert.match(helper, /key \* 0\.6180339f/);
  assert.doesNotMatch(helper, /rand\s*\(/);
});
