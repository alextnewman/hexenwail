import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const client = readFileSync('engine/hexen2/cl_main.c', 'utf8');

test('luminous entities feed the shared dynamic-light pool', () => {
  assert.match(client, /static void CL_AddEntityGlowLight/);
  assert.match(client, /flags & EF_ILLUMINATE/);
  assert.match(client, /flags & XF_TORCH_GLOW/);
  assert.match(client, /flags & XF_MISSILE_GLOW/);
  assert.match(client, /flags & \(XF_GLOW \| EF_GLOW\)/);
  assert.match(client, /CL_AddEntityGlowLight \(ent, i\)/);
});

test('glow rhythms prefer authored light styles with deterministic fallbacks', () => {
  const helper = client.match(
    /static float CL_GlowLightScale[\s\S]*?\n}\n\nstatic void CL_AddEntityGlowLight/,
  )?.[0];
  assert.ok(helper, 'glow light modulation helper is defined');
  assert.match(helper, /cl_lightstyle\[style\]\.length/);
  assert.match(helper, /cl_lightstyle\[style\]\.map\[0\] >= '1'/);
  assert.match(helper, /\* 22\.0f \/ 256\.0f/);
  assert.match(helper, /key \* 0\.6180339f/);
  assert.doesNotMatch(helper, /rand\s*\(/);
});
