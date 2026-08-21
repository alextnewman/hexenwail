import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
const assembleScript = readFileSync(join(repoRoot, 'scripts/wasm-assemble-artifact.sh'), 'utf8');
const validateScript = readFileSync(join(repoRoot, 'scripts/wasm-validate-artifact.sh'), 'utf8');
const cmake = readFileSync(join(repoRoot, 'engine/CMakeLists.txt'), 'utf8');

test('renderer preference defaults to the shipping software bundle', () => {
  const prefsBlock = app.match(/preferences: \{([\s\S]*?)\},\n {2}touchOnlyEnvironment/)?.[1];
  assert.ok(prefsBlock, 'the preferences literal is defined');
  assert.match(prefsBlock, /renderer:\s*'software'/,
    'renderer preference must default to the shipping software bundle');
});

test('loadPreferences accepts only the known renderer values and perf levels', () => {
  const load = app.match(/function loadPreferences\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(load, 'loadPreferences is defined');
  assert.match(load, /\['software', 'webglide', 'webgpu'\]\.includes\(saved\.renderer\)/,
    'unknown renderer strings must fall back to the default');
  assert.match(load, /Number\.isInteger\(perfOverlay\) && perfOverlay >= 0 && perfOverlay <= 3/,
    'the perf toggle must only accept the supported overlay levels');
  assert.match(load, /state\.preferences\.renderer = saved\.renderer/);
});

test('launcher passes the configured perf overlay to the engine on startup', () => {
  const engineArgs = app.match(/function getEngineArguments\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(engineArgs, 'engine arguments are assembled from launcher preferences');
  assert.match(engineArgs, /\+scr_perf/,
    'the launch args must set the archived scr_perf cvar when the overlay is enabled');
  assert.match(engineArgs, /String\(state\.preferences\.perfOverlay\)/);
});

test('ensureEngineScriptLoaded routes the WebGlide preference to the GPU bundle URL', () => {
  const loader = app.match(/async function ensureEngineScriptLoaded\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(loader, 'ensureEngineScriptLoaded is defined');
  // The two engine bundles ship under distinct basenames so the
  // Emscripten .js finds its own .wasm sibling by basename; the loader
  // picks one based on the persisted preference.
  assert.match(loader, /state\.preferences\.renderer === 'webglide'/);
  assert.match(loader, /state\.preferences\.renderer === 'webgpu'/);
  assert.match(loader, /'\.\/hexenwail-webglide\.js'/);
  assert.match(loader, /'\.\/hexenwail-webgpu\.js'/);
  assert.match(loader, /'\.\/hexenwail\.js'/);
  // The choice is logged through the runtime log so a bug report shows
  // which bundle was in use.
  assert.match(loader, /logToConsole\('\[launcher\]',/);
  assert.match(loader, /Loading engine bundle/);
});

test('a missing WebGlide bundle fails loudly and names the toggle', () => {
  const loader = app.match(/async function ensureEngineScriptLoaded\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(loader, 'ensureEngineScriptLoaded is defined');
  // The error path must not leave a dead launcher: name the card the
  // user actually sees, and tell them how to get back to the software
  // renderer.
  assert.match(loader, /Renderer \(experimental\)/);
  assert.match(loader, /Software \(default, stable\)/);
});

test('the launcher exposes a perf overlay toggle', () => {
  assert.match(html, /id="perf-setting"/);
  assert.match(html, /<option value="0">Off<\/option>/);
  assert.match(html, /<option value="1">FPS \+ frame time<\/option>/);
  assert.match(html, /<option value="2">Detailed breakdown<\/option>/);
  assert.match(html, /<option value="3">Frame-time graph<\/option>/);
});

test('the launcher exposes an experimental, non-default WebGlide toggle', () => {
  assert.match(html, /id="renderer-setting"/);
  assert.match(html, /<option value="software">/);
  assert.match(html, /<option value="webglide">/);
  assert.match(html, /<option value="webgpu">/);
  // The old "gl"/"glide" values must not reappear in the DOM: user-facing
  // wording is WebGlide, and the persisted preference string is
  // "webglide". Renaming the old spelling silently would leave a stale
  // preference in a browser localStorage.
  assert.doesNotMatch(html, /<option value="glide">/);
  assert.doesNotMatch(html, /<option value="gl">/);
  // Honest copy: experimental, off by default, may not work. WebGlide
  // is the mid-90s 3Dfx *look* realised on modern hardware, not a
  // Voodoo-limits emulator, so the copy must not promise retro accuracy
  // or omit the modern-shaders framing.
  const card = html.match(/<h2>Renderer \(experimental\)<\/h2>([\s\S]*?)<\/section>/)?.[1];
  assert.ok(card, 'the Renderer (experimental) card is defined');
  assert.match(card, /Software renderer/);
  assert.match(card, /shipping/);
  assert.match(card, /WebGlide/);
  assert.match(card, /experimental/);
  assert.match(card, /3Dfx/,
    'the card must call out the mid-90s 3Dfx look so users know what to expect');
  assert.match(card, /modern shaders/,
    'the card must frame WebGlide as modern shaders chasing the 3Dfx look, not a Voodoo-limits emulator');
  assert.match(card, /may render incorrectly/);
  assert.match(card, /target-feasibility step/);
});

test('changing the toggle mid-play does not yank the tab out from a running game', () => {
  const handler = app.match(/ui\.rendererSetting\?\.addEventListener\('change', \(\) => \{([\s\S]*?)\n {2}\}\);/)?.[1];
  assert.ok(handler, 'the renderer-setting change handler is defined');
  assert.match(handler, /savePreferences\(\)/,
    'the preference must be persisted before anything else, so it survives a manual reload');
  assert.match(handler, /state\.engineStarted && !state\.runtimeExited/,
    'a running game must gate the auto-reload path');
  assert.match(handler, /location\.reload\(\)/,
    'no active game means it is safe to reload the launcher automatically');
  assert.match(handler, /Renderer change queued/,
    'a mid-play change must surface an explicit "reload to apply" affordance');
  // The user-facing label in the runtime log and hint text must be the
  // canonical name.
  assert.match(handler, /WebGlide experimental GPU renderer/);
  assert.match(handler, /experimental WebGPU presenter/);
});

test('the assemble script picks up the WebGlide bundle when the webgl2 build is present', () => {
  // Optional so a local `make dist` (software-only) still works.
  assert.match(assembleScript, /engine\/build-webgl2\/bin/);
  assert.match(assembleScript, /hexenwail-webglide\.js/);
  assert.match(assembleScript, /hexenwail-webglide\.wasm/);
  assert.match(assembleScript, /if \[ -d "\$GL_BUILD_BIN" \]/);
  assert.match(assembleScript, /assembling a software-only PWA artifact/);
  // The old spelling must not linger in either the copy or the
  // filenames -- CMake now names the bundle "hexenwail-webglide".
  assert.doesNotMatch(assembleScript, /hexenwail-gl\./);
  assert.match(assembleScript, /hexenwail-webgpu\.js/);
  assert.match(assembleScript, /hexenwail-webgpu\.wasm/);
});

test('the validate script accepts a software-only artifact but rejects a half-shipped pair', () => {
  // Informational when both files are absent (matches the .data /
  // .worker.js contract already in the script); hard failure when only
  // one half of the pair is present, because that is always a broken
  // build.
  assert.match(validateScript, /hexenwail-webglide\.js/);
  assert.match(validateScript, /hexenwail-webglide\.wasm/);
  assert.match(validateScript, /only one half of the WebGlide bundle is present/);
  assert.match(validateScript, /software-only artifact/);
  assert.doesNotMatch(validateScript, /hexenwail-gl\./);
  assert.match(validateScript, /hexenwail-webgpu\.js/);
  assert.match(validateScript, /hexenwail-webgpu\.wasm/);
  assert.match(validateScript, /only one half of the WebGPU presenter bundle is present/);
});

test('WebGPU is an isolated software presenter, not a renamed Nitro renderer', () => {
  assert.match(cmake, /set\(WEB_PRESENTER "webgl2"/);
  assert.match(cmake, /WEB_PRESENTER STREQUAL "webgpu"/);
  assert.match(cmake, /add_compile_definitions\(WEBGPU_PRESENT\)/);
  assert.match(cmake, /web_canvas_wgpu\.c/);
  assert.match(cmake, /webgpu_present\.js/);
  assert.doesNotMatch(cmake, /add_compile_definitions\(WEBGPUQUAKE\)/,
    'the presenter feasibility build must not claim to be the Nitro renderer');
});
