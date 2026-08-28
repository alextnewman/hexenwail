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
const nitroEntity = readFileSync(join(repoRoot, 'engine/h2shared/wgpu_entity.c'), 'utf8');
const nitroWorld = readFileSync(join(repoRoot, 'engine/h2shared/wgpu_world.c'), 'utf8');
const nitroFront = readFileSync(join(repoRoot, 'engine/hexen2/r_webgpu.c'), 'utf8');

test('renderer preference defaults to the primary WebGlideNitro bundle', () => {
  const prefsBlock = app.match(/preferences: \{([\s\S]*?)\},\n {2}touchOnlyEnvironment/)?.[1];
  assert.ok(prefsBlock, 'the preferences literal is defined');
  assert.match(prefsBlock, /renderer:\s*'nitro'/,
    'renderer preference must default to WebGlideNitro');
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
  // user actually sees, and tell them how to get to the parked software
  // renderer.
  assert.match(loader, /Renderer/);
  assert.match(loader, /Software \(parked reference\)/);
});

test('the launcher exposes copyable raw performance capture', () => {
  assert.match(html, /id="perf-setting"/);
  assert.match(html, /<option value="0">Off<\/option>/);
  assert.match(html, /<option value="1">On \(128-frame windows\)<\/option>/);
  assert.match(html, /id="perf-output"/);
  assert.match(html, /id="perf-copy-button"/);
  assert.doesNotMatch(html, /Frame-time graph/);
});

test('the launcher exposes Nitro as primary and keeps parked renderers selectable', () => {
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
  const card = html.match(/<h2>Renderer<\/h2>([\s\S]*?)<\/section>/)?.[1];
  assert.ok(card, 'the Renderer card is defined');
  assert.match(card, /Software renderer/);
  assert.match(card, /parked/);
  assert.match(card, /WebGlide/);
  assert.match(card, /abortive/);
  assert.match(card, /WebGlideNitro/);
  assert.match(card, /primary renderer/);
  assert.match(card, /WebGlideNitro \(default\)/);
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
  assert.match(handler, /WebGlideNitro primary native WebGPU renderer/);
});

test('the assemble script picks up the WebGlide bundle when the webgl2 build is present', () => {
  // Optional so a local `make dist` (software-only) still works.
  assert.match(assembleScript, /engine\/build-webgl2\/bin/);
  assert.match(assembleScript, /hexenwail-webglide\.js/);
  assert.match(assembleScript, /hexenwail-webglide\.wasm/);
  assert.match(assembleScript, /if \[ -d "\$GL_BUILD_BIN" \]/);
  assert.match(assembleScript, /assembling the PWA artifact without WebGlide/);
  // The old spelling must not linger in either the copy or the
  // filenames -- CMake now names the bundle "hexenwail-webglide".
  assert.doesNotMatch(assembleScript, /hexenwail-gl\./);
  assert.match(assembleScript, /hexenwail-webgpu\.js/);
  assert.match(assembleScript, /hexenwail-webgpu\.wasm/);
});

test('the validate script requires Nitro and rejects half-shipped parked bundles', () => {
  // Informational when both files are absent (matches the .data /
  // .worker.js contract already in the script); hard failure when only
  // one half of the pair is present, because that is always a broken
  // build.
  assert.match(validateScript, /hexenwail-webglide\.js/);
  assert.match(validateScript, /hexenwail-webglide\.wasm/);
  assert.match(validateScript, /only one half of the WebGlide bundle is present/);
  assert.match(validateScript, /require "hexenwail-nitro\.js"/);
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
  assert.match(nitroBranch, /\$\{COMMONDIR\}\/nitro_model\.c/,
    'Nitro owns a software-derived loader with authored lighting metadata');
  assert.doesNotMatch(nitroBranch, /\$\{COMMONDIR\}\/model\.c/,
    'Nitro must not compile both implementations of the model API');
  assert.doesNotMatch(nitroBranch, /\$\{COMMONDIR\}\/gl2_/,
    'WebGlideNitro must not compile any WebGlide source');
  const webglideBranch = cmake.match(/if\(WEB_RENDERER STREQUAL "webgl2"\)([\s\S]*?)\nelseif\(WEB_RENDERER STREQUAL "webgpu"\)/)?.[1];
  const softwareBranch = cmake.match(/elseif\(WEB_RENDERER STREQUAL "webgpu"\)[\s\S]*?\nelse\(\)([\s\S]*?)\nendif\(\)\n\nset\(CLIENT_SOURCES/)?.[1];
  assert.match(webglideBranch, /\$\{COMMONDIR\}\/model\.c/);
  assert.match(softwareBranch, /\$\{COMMONDIR\}\/model\.c/);
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

test('WebGlideNitro draws entities, sprites and the view weapon its own way', () => {
  // The entity source is Nitro's own, wired only into the webgpu branch.
  const nitroBranch = cmake.match(/elseif\(WEB_RENDERER STREQUAL "webgpu"\)([\s\S]*?)\nelse\(\)/)?.[1];
  assert.ok(nitroBranch, 'WEB_RENDERER=webgpu has its own renderer-source branch');
  assert.match(nitroBranch, /wgpu_entity\.c/,
    'the entity path is a Nitro source, not a shared or WebGlide one');
  assert.doesNotMatch(nitroEntity, /#include\s+"(gl2_|r_webgl2)/,
    'Nitro must not include a WebGlide header');
  assert.doesNotMatch(nitroEntity, /\bGL2_\w+\s*\(/,
    'Nitro must not call into WebGlide');
  assert.doesNotMatch(nitroEntity, /\bgl[A-Z]\w+\s*\(/,
    'nothing here may be a GL call');

  // Ordering: opaque entities before anything translucent, and the view
  // weapon last of all.
  const renderView = nitroFront.match(/void R_RenderView \(void\)([\s\S]*?)\n\}/)?.[1];
  assert.ok(renderView, 'R_RenderView is defined');
  const opaque = renderView.indexOf('WGPUEntity_DrawEntitiesOnList (false)');
  const endOpaque = renderView.indexOf('WGPUEntity_EndOpaque');
  const translucent = renderView.indexOf('WGPUEntity_DrawEntitiesOnList (true)');
  const viewModel = renderView.indexOf('WGPUEntity_DrawViewModel');
  assert.ok(opaque > 0 && endOpaque > opaque && translucent > endOpaque
    && viewModel > translucent,
    'opaque entities, then translucent ones, then the view weapon');

  // Indexed semantics survive the trip: entity lighting picks a colormap
  // row and Hexen II's colorshade stays an index-to-index remap.
  assert.match(nitroEntity, /colormap row/i);
  assert.match(nitroEntity, /colorshade/);
  assert.match(webgpuNitro, /tintTexture/);
  assert.match(webgpuNitro, /Nitro_SetTintTable/);
  assert.match(webgpuNitro, /modelShader/);
  assert.match(webgpuNitro, /const modelPrimitive = \{ topology: 'triangle-list', cullMode: 'none' \}/,
    'model winding flips with entity scale, so the depth test decides '
    + 'visibility rather than a cull mode');

  // Brush entities reuse the immutable world buffer through a dynamic
  // uniform offset rather than a per-entity copy of the map.
  assert.match(webgpuNitro, /hasDynamicOffset: true/);
  assert.match(webgpuNitro, /ENTITY_STRIDE: 256/);
  assert.doesNotMatch(webgpuNitro, /type: 'read-only-storage'/,
    'the entity arena must stay a uniform buffer: compat mode allows no '
    + 'storage buffers in the vertex stage');

  // WebGPU has no glDepthRange, so the view weapon gets a viewport-depth
  // slice instead, and the viewport is put back afterwards.
  assert.match(webgpuNitro, /VIEWMODEL_DEPTH: 0\.3/);
  assert.match(webgpuNitro, /setViewport\(0, 0, sceneWidth, sceneHeight, 0, 1\)/);

  // The per-map gap report must not still claim entities are missing.
  const gaps = nitroWorld.match(/void WGPUWorld_ReportGaps \(void\)([\s\S]*?)\n\}/)?.[1];
  assert.ok(gaps, 'WGPUWorld_ReportGaps is defined');
  assert.match(gaps, /world, entities and particles/);
  assert.doesNotMatch(gaps, /not drawn: brush entities/);
  assert.doesNotMatch(gaps, /animated light styles/);
  assert.match(nitroFront, /WGPU_AnimateLight \(\)/);
  assert.match(nitroWorld, /void WGPUWorld_UpdateLightstyles \(void\)/);
  assert.match(nitroWorld, /cached_style/);
});

test('WebGlideNitro implements the authored visual effects', () => {
  assert.match(nitroFront, /WGPUWorld_PushDlights \(\)/);
  assert.match(nitroFront, /Fog_SetupFrame \(\)/);
  assert.match(nitroWorld, /surf->dlightbits/);
  assert.match(nitroWorld, /NITROTEX_TURB/);
  assert.match(webgpuNitro, /sin\(texel\.y \* 0\.125/);
  assert.match(webgpuNitro, /exp2\(-frame\.fogDensity/);
  assert.match(webgpuNitro, /index >= 224u/);
  assert.match(nitroEntity, /NITROMODEL_GLOW/);
  assert.match(nitroEntity, /NITROMODEL_SHADOW/);
  assert.match(nitroEntity, /WGPUWorld_LightPoint \(sample, lightspot\)/,
    'alias shadows must establish a receiver below the model');
  assert.match(nitroEntity,
    /XF_TORCH_GLOW \| XF_GLOW \| XF_MISSILE_GLOW \| EF_GLOW/,
    'luminous models must not cast projected shadows');
  assert.match(nitroEntity, /shadevector\[0\] = cos \(-an\)/,
    'alias shadows must use the legacy directional projection');
  assert.match(nitroEntity, /radius = sqrtf\(radius\)/);
  assert.match(nitroEntity, /mins\[i\] = entity->origin\[i\] - radius/,
    'alias culling must use transform-safe spherical bounds');
  assert.match(nitroEntity, /shade \|= 1u << 16/,
    'only explicitly luminous models may mark their high skin indices fullbright');
  assert.match(webgpuNitro, /input\.fullbright != 0u/,
    'incidental high skin indices must remain colormap-lit');
  assert.match(webgpuNitro, /modelShadowMaskPipeline/);
  assert.match(webgpuNitro, /'greater-equal', 'shadowBack', 0/,
    'the shadow mask must reject pixels with no receiver');
  assert.match(webgpuNitro, /'less-equal', 'shadowFront'/,
    'the shadow draw must reject nearer occluding geometry');
  assert.match(webgpuNitro, /GPUColorWrite\.ALL, 'zero'/,
    'occluded mask fragments must not leak into later shadow batches');
  assert.match(webgpuNitro, /pass\.setStencilReference\(1\)/);
  assert.match(webgpuNitro, /format: 'depth24plus-stencil8'/);
  assert.match(webgpuNitro, /stencilLoadOp: 'clear'/);
  assert.match(webgpuNitro, /passOp: 'increment-clamp'/,
    'collapsed shadow triangles must shade each pixel only once');
  assert.match(webgpuNitro, /alphaFilter === 'blend'/,
    'translucent liquids must be deferred until opaque models are drawn');
});

test('the nitro build is reachable from the scripts, CI and the offline shell', () => {
  assert.match(buildScript, /nitro\)/);
  assert.match(buildScript, /renderer="webgpu"/);
  assert.match(assembleScript, /hexenwail-nitro\.js/);
  assert.match(assembleScript, /hexenwail-nitro\.wasm/);
  assert.match(validateScript, /require "hexenwail-nitro\.js"/);
  assert.match(validateScript, /require "hexenwail-nitro\.wasm"/);
  assert.match(wasmAction, /wasm-build\.sh nitro engine\/build-nitro/);
  assert.match(wasmAction, /missing the WebGlideNitro bundle/);
  assert.match(serviceWorker, /'\.\/hexenwail-nitro\.js'/);
  const core = serviceWorker.match(/const CORE_ASSETS = \[([\s\S]*?)\];/)?.[1];
  assert.ok(core, 'CORE_ASSETS is defined');
  assert.match(core, /hexenwail-nitro/);
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
