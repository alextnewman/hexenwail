import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const manifest = JSON.parse(readFileSync(join(repoRoot, 'web/manifest.webmanifest'), 'utf8'));
const swText = readFileSync(join(repoRoot, 'web/sw.js'), 'utf8');
const assembleScript = readFileSync(join(repoRoot, 'scripts/wasm-assemble-artifact.sh'), 'utf8');

test('manifest uses relative project-pages-safe paths', () => {
  assert.match(manifest.start_url, /^\.\/?$/);
  assert.match(manifest.scope, /^\.\/?$/);
  for (const icon of manifest.icons) {
    assert.match(icon.src, /^\.\//);
    assert.equal(existsSync(join(repoRoot, 'web', icon.src.replace(/^\.\//, ''))), true, `missing icon ${icon.src}`);
  }
});

test('service worker precaches repo-managed launcher assets', () => {
  const matches = [...swText.matchAll(/'\.\/([^']+)'/g)].map((match) => match[1]);
  const repoManaged = matches.filter((asset) => !asset.startsWith('hexenwail.') && !asset.startsWith('hexenwail-webglide.'));
  for (const asset of repoManaged) {
    const relativePath = asset === '' ? 'index.html' : asset;
    assert.equal(existsSync(join(repoRoot, 'web', relativePath)), true, `missing precache asset ${asset}`);
  }
  assert.ok(matches.includes('hexenwail.js'));
  assert.ok(matches.includes('hexenwail.wasm'));
  assert.ok(matches.includes('lib/phone-controls.js'));
});

test('service worker keeps the WebGlide GPU bundle out of the install precache', () => {
  // The WebGlide bundle is runtime-cached on first use rather than
  // precached. Two reasons: precaching megabytes of experimental code
  // would slow the first paint for the ~all users who never toggle it
  // on; and cache.addAll is atomic, so a 404 on a software-only local
  // build would abort the whole SW install and leave the launcher
  // without an offline shell.
  const coreBlock = swText.match(/const CORE_ASSETS = \[([\s\S]*?)\];/)?.[1];
  assert.ok(coreBlock, 'CORE_ASSETS array is defined');
  assert.doesNotMatch(coreBlock, /hexenwail-webglide/,
    'CORE_ASSETS must not precache the experimental WebGlide bundle');
  const optionalBlock = swText.match(/const OPTIONAL_ASSETS = \[([\s\S]*?)\];/)?.[1];
  assert.ok(optionalBlock, 'OPTIONAL_ASSETS array is defined');
  assert.match(optionalBlock, /'\.\/hexenwail-webglide\.js'/);
  assert.match(optionalBlock, /'\.\/hexenwail-webglide\.wasm'/);
  // Runtime-cached on first use, so both core and optional URLs feed the
  // same cache-first fetch path; anything else would break offline play
  // after a single WebGlide session.
  assert.match(swText, /if \(!CORE_ASSET_URLS\.includes\(request\.url\) && !OPTIONAL_ASSET_URLS\.includes\(request\.url\)\)/);
});

test('service worker cache is deployment-versioned and refreshes core assets', () => {
  assert.match(swText, /CACHE_VERSION = `\$\{CACHE_PREFIX\}__HEXENWAIL_BUILD_VERSION__`/);
  assert.match(swText, /new Request\(url, \{ cache: 'reload' \}\)/);
  assert.match(swText, /match\(request\)/);
  assert.match(assembleScript, /GITHUB_SHA/);
  assert.match(assembleScript, /s\/__HEXENWAIL_BUILD_VERSION__\/\$BUILD_VERSION\/g/);
});
