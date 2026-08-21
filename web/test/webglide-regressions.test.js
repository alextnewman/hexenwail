import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const aliasRenderer = readFileSync(join(repoRoot, 'engine/h2shared/gl2_alias.c'), 'utf8');
const audioBackend = readFileSync(join(repoRoot, 'engine/h2shared/snd_web.c'), 'utf8');

test('WebGlide converts alias 16.16 skin coordinates to normalized UVs', () => {
  const sScale = aliasRenderer.match(
    /out->s\s*=\s*st->s\s*\*\s*\(1\.0f\s*\/\s*([0-9.]+)f\)\s*\*\s*iw\s*;/,
  );
  const tScale = aliasRenderer.match(
    /out->t\s*=\s*st->t\s*\*\s*\(1\.0f\s*\/\s*([0-9.]+)f\)\s*\*\s*ih\s*;/,
  );
  assert.ok(sScale && tScale, 'both alias UV axes remove the fixed-point scale');
  const fixedS = 48 * 65536;
  const fixedT = 24 * 65536;
  assert.equal(fixedS / Number(sScale[1]) / 96, 0.5);
  assert.equal(fixedT / Number(tScale[1]) / 96, 0.25);
});

test('WebAudio lifecycle promises cannot escape as unhandled rejections', () => {
  assert.match(audioBackend, /context\.resume\(\)\.catch/);
  assert.match(audioBackend, /context\.suspend\(\)\.catch/);
  assert.match(audioBackend, /Module\.webAudioContext\.close\(\)\.catch/);
  assert.match(audioBackend, /Module\.webAudioResumeController\.abort\(\)/);
});
