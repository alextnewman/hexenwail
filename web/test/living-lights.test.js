import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const client = readFileSync('engine/hexen2/cl_main.c', 'utf8');
const nitro = readFileSync('engine/hexen2/r_webgpu.c', 'utf8');
const nitroModel = readFileSync('engine/h2shared/nitro_model.c', 'utf8');
const qc = readFileSync('engine/h2shared/pr_cmds.c', 'utf8');
const server = readFileSync('engine/hexen2/sv_main.c', 'utf8');
const parser = readFileSync('engine/hexen2/cl_parse.c', 'utf8');

test('Nitro luminous entities feed the shared dynamic-light pool', () => {
  assert.match(nitro, /static void Nitro_AddEntityGlowLight/);
  assert.match(nitro, /flags & EF_ILLUMINATE/);
  assert.match(nitro, /flags & XF_TORCH_GLOW/);
  assert.match(nitro, /flags & XF_MISSILE_GLOW/);
  assert.match(nitro, /flags & \(XF_GLOW \| EF_GLOW\)/);
  assert.match(nitro, /Nitro_AddEntityGlowLight \(entity, i, true\)/);
  assert.match(nitro, /entity = cl_static_entities/);
  assert.match(nitro, /key = MAX_EDICTS \+ i/);
  assert.match(nitro, /Nitro_DlightSlotAvailable \(key\)/);
  assert.match(nitro, /Nitro_AddEntityGlowLight \(entity, key, false\)/);
  assert.match(nitro, /flags = entity->model->ex_flags/);
  assert.match(nitro, /settings = entity->model->glow_settings/);
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

test('the Nitro model loader preserves every authored glow classification', () => {
  const classifiedModels = [
    'rflmtrch', 'cflmtrch', 'castrch', 'rometrch', 'egtorch', 'flame', 'eflmtrch',
    'i_bmana', 'i_gmana', 'i_btmana',
    'drgnball', 'eidoball', 'lavaball', 'glowball', 'fireball', 'famshot',
    'pestshot', 'mumshot', 'scrbstp1', 'scrbpbody', 'iceshot2', 'iceshot',
    'flaming', 'sucwp1p', 'sucwp2p', 'goop', 'purfir1', 'golemmis', 'shard',
    'shardice', 'snakearr', 'spit', 'spike',
  ];

  assert.match(nitroModel, /static void Mod_SetAliasModelExtraFlags/);
  assert.equal(
    [...nitroModel.matchAll(/Mod_SetAliasModelExtraFlags \(mod\);/g)].length,
    2,
    'both MDL formats receive authored metadata before their defaults are saved',
  );
  for (const model of classifiedModels) {
    assert.match(nitroModel, new RegExp(`models/${model}`), `${model} remains classified`);
  }
  assert.match(nitroModel, /XF_TORCH_GLOW_EGYPT/);
  assert.match(nitroModel, /XF_MISSILE_GLOW/);
  assert.match(nitroModel, /glow_settings\[LIGHT_STYLE\] = 11/);
  assert.match(nitroModel, /Mod_SaveAliasModelDefaults \(mod\)/);
  assert.match(nitroModel, /Mod_RestoreAliasModelDefaults/);
  assert.doesNotMatch(
    nitroModel,
    /GL_LoadTexture|gl_texturenum|Mod_LoadFullbrightTexture|Mod_ComputeFlipbookRatio/,
    'the software-derived loader must not acquire GL rendering machinery',
  );
});

test('Nitro receives game-authored model effects without dropping spatial metadata', () => {
  const nitroEffectsGuard = /defined\(GLQUAKE\) \|\| defined\(WEBGPUQUAKE\)/;

  assert.match(qc, nitroEffectsGuard);
  assert.match(server, nitroEffectsGuard);
  assert.match(parser, nitroEffectsGuard);
  assert.match(qc, /glow_settings\[ORB_OFFSET_X\] = view_ofs\[0\]/);
  assert.match(qc, /glow_settings\[ORB_RADIUS\] = health/);
  assert.match(qc, /glow_settings\[LIGHT_RADIUS\] = max_health/);
  assert.match(qc, /glow_settings\[LIGHT_STYLE\] = atoi/);
});
