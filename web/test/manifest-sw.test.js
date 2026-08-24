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
  const repoManaged = matches.filter((asset) => !asset.startsWith('hexenwail.')
    && !asset.startsWith('hexenwail-nitro.'));
  for (const asset of repoManaged) {
    const relativePath = asset === '' ? 'index.html' : asset;
    assert.equal(existsSync(join(repoRoot, 'web', relativePath)), true, `missing precache asset ${asset}`);
  }
  assert.ok(matches.includes('hexenwail-nitro.js'));
  assert.ok(matches.includes('hexenwail-nitro.wasm'));
  assert.ok(matches.includes('lib/phone-controls.js'));
});

test('service worker precaches the supported software and Nitro bundles', () => {
  const coreBlock = swText.match(/const CORE_ASSETS = \[([\s\S]*?)\];/)?.[1];
  assert.ok(coreBlock, 'CORE_ASSETS array is defined');
  assert.doesNotMatch(coreBlock, /hexenwail-webglide/,
    'CORE_ASSETS must not precache any retired WebGlide bundle');
  assert.doesNotMatch(coreBlock, /hexenwail-webgpu/,
    'CORE_ASSETS must not precache legacy WebGPU presenter assets');
  assert.match(coreBlock, /hexenwail-nitro\.js/);
  assert.match(coreBlock, /hexenwail-nitro\.wasm/);
  const optionalBlock = swText.match(/const OPTIONAL_ASSETS = \[([\s\S]*?)\];/)?.[1];
  assert.ok(optionalBlock, 'OPTIONAL_ASSETS array is defined');
  assert.match(optionalBlock, /'\.\/hexenwail\.js'/);
  assert.match(optionalBlock, /'\.\/hexenwail\.wasm'/);
  assert.doesNotMatch(optionalBlock, /hexenwail-webglide/);
  assert.doesNotMatch(optionalBlock, /hexenwail-webgpu/);
  assert.doesNotMatch(optionalBlock, /'\.\/hexenwail-nitro\.js'/);
  assert.match(swText, /if \(!CORE_ASSET_URLS\.includes\(request\.url\) && !OPTIONAL_ASSET_URLS\.includes\(request\.url\)\)/);
});

test('service worker cache is deployment-versioned and refreshes core assets', () => {
  assert.match(swText, /CACHE_VERSION = `\$\{CACHE_PREFIX\}__HEXENWAIL_BUILD_VERSION__`/);
  assert.match(swText, /new Request\(url, \{ cache: 'reload' \}\)/);
  assert.match(swText, /match\(request\)/);
  assert.match(assembleScript, /GITHUB_SHA/);
  assert.match(assembleScript, /s\/__HEXENWAIL_BUILD_VERSION__\/\$BUILD_VERSION\/g/);
});
