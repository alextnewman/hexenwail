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
const webgpuPresenter = readFileSync(join(repoRoot, 'engine/web/webgpu_present.js'), 'utf8');
const webgpuNitro = readFileSync(join(repoRoot, 'engine/web/webgpu_nitro.js'), 'utf8');
const buildScript = readFileSync(join(repoRoot, 'scripts/wasm-build.sh'), 'utf8');
const serviceWorker = readFileSync(join(repoRoot, 'web/sw.js'), 'utf8');
const wasmAction = readFileSync(join(repoRoot, '.github/actions/wasm-build/action.yml'), 'utf8');

test('renderer preference defaults to the shipping software bundle', () => {
  const prefsBlock = app.match(/preferences: \{([\s\S]*?)\},\n {2}touchOnlyEnvironment/)?.[1];
  assert.ok(prefsBlock, 'the preferences literal is defined');
  assert.match(prefsBlock, /renderer:\s*'software'/,
    'renderer preference must default to the shipping software bundle');
});

test('loadPreferences accepts renderer values and migrates the perf preference', () => {
  const load = app.match(/function loadPreferences\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(load, 'loadPreferences is defined');
  assert.match(load, /\['software', 'webglide', 'webgpu', 'nitro'\]\.includes\(saved\.renderer\)/,
    'unknown renderer strings must fall back to the default');
  assert.match(load, /typeof saved\.perfCapture === 'boolean'/);
  assert.match(load, /Number\(saved\.perfOverlay\) > 0/,
    'an enabled legacy overlay preference should migrate to capture');
  assert.match(load, /state\.preferences\.renderer = saved\.renderer/);
});

test('launcher passes the configured perf capture to the engine on startup', () => {
  const engineArgs = app.match(/function getEngineArguments\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(engineArgs, 'engine arguments are assembled from launcher preferences');
  assert.match(engineArgs, /\+scr_perf/,
    'the launch args must enable the engine capture cvar');
  assert.match(engineArgs, /state\.preferences\.perfCapture/);
  assert.match(engineArgs, /\? '1' : '0'/,
    'the launcher must explicitly disable an archived capture setting');
});

test('ensureEngineScriptLoaded routes the WebGlide preference to the GPU bundle URL', () => {
  const loader = app.match(/async function ensureEngineScriptLoaded\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(loader, 'ensureEngineScriptLoaded is defined');
  // The two engine bundles ship under distinct basenames so the
  // Emscripten .js finds its own .wasm sibling by basename; the loader
  // picks one based on the persisted preference.
  assert.match(loader, /state\.preferences\.renderer === 'webglide'/);
  assert.match(loader, /state\.preferences\.renderer === 'webgpu'/);
  assert.match(loader, /state\.preferences\.renderer === 'nitro'/);
  assert.match(loader, /'\.\/hexenwail-webglide\.js'/);
  assert.match(loader, /'\.\/hexenwail-webgpu\.js'/);
  assert.match(loader, /'\.\/hexenwail-nitro\.js'/);
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

test('the launcher exposes copyable raw performance capture', () => {
  assert.match(html, /id="perf-setting"/);
  assert.match(html, /<option value="0">Off<\/option>/);
  assert.match(html, /<option value="1">On \(128-frame windows\)<\/option>/);
  assert.match(html, /id="perf-output"/);
  assert.match(html, /id="perf-copy-button"/);
  assert.doesNotMatch(html, /Frame-time graph/);
});

test('the launcher exposes an experimental, non-default WebGlide toggle', () => {
  assert.match(html, /id="renderer-setting"/);
  assert.match(html, /<option value="software">/);
  assert.match(html, /<option value="webglide">/);
  assert.match(html, /<option value="webgpu">/);
  assert.match(html, /<option value="nitro">/);
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
  // WebGlideNitro is a third, separate path. Its first slice draws the
  // static world only, so the card must say so rather than let a user
  // assume it is a playable renderer.
  assert.match(card, /WebGlideNitro/);
  assert.match(card, /static world/);
  assert.match(card, /technology preview/);
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
  assert.match(handler, /WebGlideNitro native WebGPU renderer/);
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

test('the WebGPU presenter stays a software scan-out path, distinct from Nitro', () => {
  assert.match(cmake, /set\(WEB_PRESENTER "webgl2"/);
  assert.match(cmake, /WEB_PRESENTER STREQUAL "webgpu"/);
  assert.match(cmake, /add_compile_definitions\(WEBGPU_PRESENT\)/);
  assert.match(cmake, /web_canvas_wgpu\.c/);
  assert.match(cmake, /webgpu_present\.js/);
  // The presenter is selected by WEB_PRESENTER and keeps WEBSOFT; only
  // WEB_RENDERER=webgpu may define WEBGPUQUAKE. Guard the coupling so a
  // future edit cannot quietly turn the presenter into "the Nitro build".
  assert.match(cmake, /WEB_PRESENTER only applies to WEB_RENDERER=software/);
  const nitroBranch = cmake.match(/elseif\(WEB_RENDERER STREQUAL "webgpu"\)([\s\S]*?)\nelse\(\)/)?.[1];
  assert.ok(nitroBranch, 'WEB_RENDERER=webgpu has its own renderer-source branch');
  assert.match(nitroBranch, /add_compile_definitions\(WEBGPUQUAKE\)/,
    'WEBGPUQUAKE is positive and belongs to WEB_RENDERER=webgpu alone');
  assert.doesNotMatch(nitroBranch, /\$\{COMMONDIR\}\/gl2_/,
    'WebGlideNitro must not compile any WebGlide source');
  assert.match(wasmAction, /wasm-build\.sh webgpu engine\/build-webgpu/);
  assert.doesNotMatch(webgpuPresenter, /\bsmooth:\s*u32/,
    'WGSL reserves the word smooth');
});

test('WebGlideNitro is a native WebGPU renderer, not a GL translation layer', () => {
  // The build wiring: its own WEB_RENDERER value, its own JS library, its
  // own bundle basename, and no software rasterizer sources.
  assert.match(cmake, /set_property\(CACHE WEB_RENDERER PROPERTY STRINGS software webgl2 webgpu\)/);
  assert.match(cmake, /webgpu_nitro\.js/);
  assert.match(cmake, /set\(WEB_OUTPUT_NAME hexenwail-nitro\)/);
  assert.match(cmake, /r_webgpu\.c/);
  assert.match(cmake, /wgpu_world\.c/);
  assert.match(cmake, /draw_webgpu\.c/);
  assert.match(cmake, /vid_webgpu\.c/);

  // The backend must be real WebGPU, and must reuse the launcher handoff
  // rather than requesting a second device for the same canvas.
  assert.match(webgpuNitro, /Module\.hexenwailWebGPU/);
  assert.match(webgpuNitro, /createRenderPipeline/);
  assert.match(webgpuNitro, /drawIndexed/);
  assert.doesNotMatch(webgpuNitro, /requestAdapter/,
    'the device comes from the launcher probe, not from the engine');
  assert.doesNotMatch(webgpuNitro, /\bgl[A-Z]\w+\(/,
    'nothing here may be a translated GL call');

  // Static geometry: the world vertex buffer is mapped at creation and
  // never gets COPY_DST, which is what makes it immutable.
  assert.match(webgpuNitro, /mappedAtCreation:\s*true/);

  // Indexed colour is preserved end to end: r8uint diffuse, a colormap
  // row chosen by the lightmap, and only then the palette.
  assert.match(webgpuNitro, /r8uint/);
  assert.match(webgpuNitro, /colormapTexture/);
  assert.match(webgpuNitro, /paletteTexture/);
});

test('the nitro build is reachable from the scripts, CI and the offline shell', () => {
  assert.match(buildScript, /nitro\)/);
  assert.match(buildScript, /renderer="webgpu"/);
  assert.match(assembleScript, /hexenwail-nitro\.js/);
  assert.match(assembleScript, /hexenwail-nitro\.wasm/);
  assert.match(validateScript, /only one half of the WebGlideNitro bundle is present/);
  assert.match(wasmAction, /wasm-build\.sh nitro engine\/build-nitro/);
  assert.match(wasmAction, /missing the WebGlideNitro bundle/);
  // Optional, not core: precaching it would slow every fresh install and
  // a 404 from a software-only build would abort the whole SW install.
  assert.match(serviceWorker, /'\.\/hexenwail-nitro\.js'/);
  const core = serviceWorker.match(/const CORE_ASSETS = \[([\s\S]*?)\];/)?.[1];
  assert.ok(core, 'CORE_ASSETS is defined');
  assert.doesNotMatch(core, /hexenwail-nitro/);
});

test('the canonical docs do not make WebGlide a Nitro gate or performance baseline', () => {
  // Owner instruction, on the record: WebGlide performance is not a
  // criterion for anything. Nitro is measured on the target iPad against
  // its own captures, so a future edit must not quietly restore the gate.
  const flat = (text) => text.replace(/\s+/g, ' ');
  const nitroDoc = readFileSync(join(repoRoot, 'docs/web/WEBGLIDE_NITRO.md'), 'utf8');
  const webglideDoc = readFileSync(join(repoRoot, 'docs/web/WEBGLIDE.md'), 'utf8');
  assert.doesNotMatch(nitroDoc, /^## The gate$/m,
    'the WebGlide gate section is removed, not renamed');
  assert.match(nitroDoc, /^## How Nitro is measured$/m);
  assert.match(flat(nitroDoc), /WebGlide is not a performance baseline, a gate, or an optimisation criterion/);
  assert.match(flat(nitroDoc), /measured directly on the target iPad, against Nitro's own captures/);
  // WebGlide is framed as abortive, yet stays buildable: deleting it is a
  // separate decision the owner has not made.
  assert.match(flat(webglideDoc), /abortive experiment/);
  assert.match(flat(webglideDoc), /must keep compiling/);
  assert.match(cmake, /WEB_RENDERER STREQUAL "webgl2"/);
});
