import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
const inputBackend = readFileSync(join(repoRoot, 'engine/h2shared/in_web.c'), 'utf8');

test('immersive layout is driven by its own attribute, not by phone mode', () => {
  assert.match(html, /body\[data-engine-state="running"\]\[data-immersive="true"\] \.topbar/);
  assert.match(html, /body\[data-engine-state="running"\]\[data-immersive="true"\] \.viewport/);
  assert.match(html, /body\[data-engine-state="running"\]\[data-immersive="true"\] \.phone-top-button/);
  assert.doesNotMatch(html, /body\[data-engine-state="running"\]\[data-phone-mode="true"\] \.topbar/,
    'hiding the launcher chrome must not depend on phone mode any more');
  assert.match(app, /document\.body\.dataset\.immersive = state\.immersive \|\| state\.phoneMode/);
});

test('starting the game is what enters fullscreen play', () => {
  const startBody = app.match(/async function startEngineFromUserAction\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(startBody, 'startEngineFromUserAction is defined');
  assert.match(startBody, /enterFullscreenPlay\(\)/,
    'fullscreen must be requested from the launch gesture so it can never be entered without a game');
  assert.match(app, /function enterNativeFullscreen[\s\S]*?ui\.viewport \?\? ui\.canvas/,
    'the game surface, not the bare canvas, is the fullscreen element');
});

test('fullscreen transitions resize the canvas and restore the launcher', () => {
  assert.match(app, /document\.addEventListener\('fullscreenchange', handleFullscreenChange\)/);
  assert.match(app, /document\.addEventListener\('webkitfullscreenchange', handleFullscreenChange\)/);
  const handler = app.match(/function handleFullscreenChange\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(handler, 'handleFullscreenChange is defined');
  assert.match(handler, /setImmersive\(false\)/, 'leaving fullscreen must bring the launcher chrome back');
  assert.match(handler, /scheduleCanvasResize\(\)/, 'the engine canvas must be re-measured after the transition');
});

test('the fullscreen toggle is only usable while the engine is running', () => {
  assert.match(html, /<button id="fullscreen-button" disabled>/);
  assert.match(app, /ui\.fullscreenButton\.disabled = !playing/);
  assert.match(app, /ui\.fullscreenButton\?\.addEventListener\('click', \(\) => toggleFullscreenPlay\(\)\)/);
  assert.match(html, /id="windowed-button"/, 'immersive play needs an in-game way back to the launcher');
});

test('pointer lock is only requested once the engine owns the canvas', () => {
  const capture = app.match(/function tryCaptureInput\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(capture, 'tryCaptureInput is defined');
  assert.match(capture, /if \(!state\.engineStarted \|\| state\.runtimeExited\) return;/);
  assert.match(capture, /requestPointerLock/);
});

test('the web input backend maps backquote to Escape and owns cursor visibility', () => {
  assert.match(inputBackend, /in_key_backquote_escape/);
  assert.match(inputBackend, /key == '`' && in_key_backquote_escape\.integer/);
  assert.match(inputBackend, /key = K_ESCAPE/);
  assert.match(inputBackend, /Cvar_RegisterVariable\(&in_key_backquote_escape\)/);
  assert.match(inputBackend, /void IN_Commands \(void\) \{ Web_SetCursorHidden\(Key_GetDest\(\) == key_game \? 1 : 0\); \}/);
  assert.match(inputBackend, /canvas\.style\.cursor = \$0 \? 'none' : 'default'/);
});
