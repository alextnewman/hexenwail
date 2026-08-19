import test from 'node:test';
import assert from 'node:assert/strict';
import { hasRequiredBaseAssets, mapImportedPath, sanitizeRelativePath } from '../lib/paths.js';

test('sanitizeRelativePath rejects traversal and absolute paths', () => {
  assert.equal(sanitizeRelativePath('../data1/pak0.pak'), null);
  assert.equal(sanitizeRelativePath('/data1/pak0.pak'), null);
  assert.equal(sanitizeRelativePath('C:/data1/pak0.pak'), null);
  assert.equal(sanitizeRelativePath('data1/./pak0.pak'), 'data1/pak0.pak');
});

test('mapImportedPath recognizes common Hexen II asset layouts', () => {
  assert.equal(mapImportedPath('pak0.pak'), 'data1/pak0.pak');
  assert.equal(mapImportedPath('pak1.pak'), 'data1/pak1.pak');
  assert.equal(mapImportedPath('pak3.pak'), 'portals/pak3.pak');
  assert.equal(mapImportedPath('Hexen II/data1/pak1.pak'), 'data1/pak1.pak');
  assert.equal(mapImportedPath('music/track02.ogg'), 'data1/music/track02.ogg');
  assert.equal(mapImportedPath('Mods/portals/progs.dat'), 'portals/progs.dat');
  assert.equal(mapImportedPath('README.txt'), null);
});

test('hasRequiredBaseAssets detects the mandatory retail pak set', () => {
  assert.equal(hasRequiredBaseAssets(['data1/pak0.pak']), false);
  assert.equal(hasRequiredBaseAssets(['data1/pak0.pak', 'data1/pak1.pak']), true);
});

test('mapImportedPath routes every decodable loose music format to data1/music', () => {
  assert.equal(mapImportedPath('casa1.ogg'), 'data1/music/casa1.ogg');
  assert.equal(mapImportedPath('casa1.mp3'), 'data1/music/casa1.mp3');
  assert.equal(mapImportedPath('casa1.flac'), 'data1/music/casa1.flac');
  assert.equal(mapImportedPath('casa1.wav'), 'data1/music/casa1.wav');
  assert.equal(mapImportedPath('CASA1.MP3'), 'data1/music/CASA1.MP3');
  // Opus and the tracker formats have no Emscripten port, so the engine
  // cannot decode them; do not pretend to accept them.
  assert.equal(mapImportedPath('casa1.opus'), null);
  assert.equal(mapImportedPath('casa1.xm'), null);
});

test('mapImportedPath keeps an explicit music directory over the loose-file fallback', () => {
  assert.equal(mapImportedPath('portals/music/tulku1.mp3'), 'portals/music/tulku1.mp3');
  assert.equal(mapImportedPath('music/tulku1.flac'), 'data1/music/tulku1.flac');
});
