import test from 'node:test';
import assert from 'node:assert/strict';
import {
  createSaveBundle,
  createStoredZip,
  getPakCompatibilityWarnings,
  isSavePath,
  planSaveImport,
  validateSaveBundle,
} from '../lib/save-bundle.js';

const save = { path: 'data1/s0/clients.gip', bytes: new TextEncoder().encode('save state') };

test('classifies engine save slots while excluding commercial assets', () => {
  assert.equal(isSavePath('data1/s0/info.dat'), true);
  assert.equal(isSavePath('portals/ms9/level.gip'), true);
  assert.equal(isSavePath('hw/s1/clients.gip'), true);
  assert.equal(isSavePath('data1/pak0.pak'), false);
  assert.equal(isSavePath('data1/music/track01.ogg'), false);
  assert.equal(isSavePath('data1/progs.dat'), false);
});

test('creates and validates a versioned ZIP save bundle', async () => {
  const { bytes, manifest } = await createSaveBundle([save], { createdAt: '2026-08-07T00:00:00.000Z' });
  assert.equal(manifest.files[0].path, 'saves/data1/s0/clients.gip');
  const restored = await validateSaveBundle(bytes);
  assert.equal(restored.manifest.formatVersion, 1);
  assert.deepEqual(restored.files[0], save);
});

test('rejects mismatched, duplicate, unlisted, and unsafe bundle files', async () => {
  const { manifest } = await createSaveBundle([save], { createdAt: '2026-08-07T00:00:00.000Z' });
  const changed = structuredClone(manifest);
  changed.files[0].size = 999;
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(changed)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /size mismatch/);
  const badHash = structuredClone(manifest);
  badHash.files[0].sha256 = '0'.repeat(64);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(badHash)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /hash mismatch/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
    { name: 'extra.txt', bytes: new Uint8Array() },
  ])), /undeclared|files not declared/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /Duplicate ZIP path/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify(manifest)) },
    { name: '../saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /unsafe/);
  await assert.rejects(() => validateSaveBundle(createStoredZip([
    { name: 'hexenwail-save.json', bytes: new TextEncoder().encode(JSON.stringify({ ...manifest, formatVersion: 2 })) },
    { name: 'saves/data1/s0/clients.gip', bytes: save.bytes },
  ])), /Unsupported save bundle version/);
});

test('plans merge and replace without deleting assets', () => {
  const existing = ['data1/pak0.pak', 'data1/s0/info.dat', 'data1/s1/clients.gip'];
  assert.deepEqual(planSaveImport(existing, [save], 'merge'), { writes: ['data1/s0/clients.gip'], deletes: [] });
  assert.deepEqual(planSaveImport(existing, [save], 'replace'), { writes: ['data1/s0/clients.gip'], deletes: ['data1/s0/info.dat', 'data1/s1/clients.gip'] });
});

test('reports missing and incompatible PAKs', () => {
  const required = [{ path: 'data1/pak0.pak', size: 10, sha256: 'a'.repeat(64) }, { path: 'portals/pak3.pak', size: 20 }];
  assert.deepEqual(getPakCompatibilityWarnings(required, [{ path: 'data1/pak0.pak', size: 9, sha256: 'b'.repeat(64) }]), [
    'Game data differs from this save bundle: data1/pak0.pak',
    'Missing required game data: portals/pak3.pak',
  ]);
});
