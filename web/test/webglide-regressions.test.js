import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const aliasRenderer = readFileSync(join(repoRoot, 'engine/h2shared/gl2_alias.c'), 'utf8');
const audioBackend = readFileSync(join(repoRoot, 'engine/h2shared/snd_web.c'), 'utf8');

test('WebGlide converts alias 16.16 skin coordinates to normalized UVs', () => {
  assert.match(aliasRenderer, /out->s = st->s \* \(1\.0f \/ 65536\.0f\) \* iw;/);
  assert.match(aliasRenderer, /out->t = st->t \* \(1\.0f \/ 65536\.0f\) \* ih;/);
});

test('WebAudio lifecycle promises cannot escape as unhandled rejections', () => {
  assert.match(audioBackend, /context\.resume\(\)\.catch/);
  assert.match(audioBackend, /context\.suspend\(\)\.catch/);
  assert.match(audioBackend, /Module\.webAudioContext\.close\(\)\.catch/);
  assert.match(audioBackend, /Module\.webAudioResumeController\.abort\(\)/);
});
