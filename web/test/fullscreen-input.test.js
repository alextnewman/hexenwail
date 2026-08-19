import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const repoRoot = process.cwd();
const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
const inputBackend = readFileSync(join(repoRoot, 'engine/h2shared/in_web.c'), 'utf8');
const keys = readFileSync(join(repoRoot, 'engine/hexen2/keys.c'), 'utf8');
const menu = readFileSync(join(repoRoot, 'engine/hexen2/menu.c'), 'utf8');

test('immersive layout is driven by its own attribute, not by phone mode', () => {
  assert.match(html, /body\[data-engine-state="running"\]\[data-immersive="true"\] \.topbar/);
  assert.match(html, /body\[data-engine-state="running"\]\[data-immersive="true"\] \.viewport/);
  assert.match(html, /body\[data-engine-state="running"\]\[data-immersive="true"\] \.phone-top-button/);
  assert.doesNotMatch(html, /body\[data-engine-state="running"\]\[data-phone-mode="true"\] \.topbar/,
    'hiding the launcher chrome must not depend on phone mode any more');
  assert.match(app, /document\.body\.dataset\.immersive = \(state\.immersive \|\| state\.phoneMode\)/);
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
  const commands = inputBackend.match(/void IN_Commands \(void\)\n\{([\s\S]*?)\n\}/)?.[1];
  assert.ok(commands, 'IN_Commands is defined');
  assert.match(commands, /Web_SetCursorHidden\(Key_GetDest\(\) == key_game \? 1 : 0\);/);
  assert.match(inputBackend, /canvas\.style\.cursor = \$0 \? 'none' : 'default'/);
});

test('the gamepad is polled by the native web input driver, not an SDL layer', () => {
  assert.doesNotMatch(inputBackend, /SDL_/, 'the web port has no SDL backend to borrow from');
  // The Gamepad API is poll-only, so the driver has to sample it from the
  // host frame rather than wait for events.
  const commands = inputBackend.match(/void IN_Commands \(void\)\n\{([\s\S]*?)\n\}/)?.[1];
  assert.ok(commands, 'IN_Commands is defined');
  assert.match(commands, /Web_PollGamepad\(\);/);
  assert.match(inputBackend, /emscripten_sample_gamepad_data\(\)/);
  assert.match(inputBackend, /emscripten_get_num_gamepads\(\)/);
  assert.match(inputBackend, /emscripten_get_gamepad_status\(/);
});

test('the gamepad driver uses the W3C standard mapping and reaches the menus', () => {
  // Standard-mapping button order: face, shoulders, triggers, back/start,
  // thumbs, D-pad. Getting this order wrong silently mis-binds everything.
  const table = inputBackend.match(/static const int gp_button_keys\[GPB_COUNT\] = \{([\s\S]*?)\};/)?.[1];
  assert.ok(table, 'the standard-mapping button table is defined');
  const keys = table.split(/[\s,]+/).filter((token) => token.startsWith('K_GP_'));
  assert.deepEqual(keys, [
    'K_GP_A', 'K_GP_B', 'K_GP_X', 'K_GP_Y',
    'K_GP_LSHOULDER', 'K_GP_RSHOULDER',
    'K_GP_LTRIGGER', 'K_GP_RTRIGGER',
    'K_GP_BACK', 'K_GP_START',
    'K_GP_LTHUMB', 'K_GP_RTHUMB',
    'K_GP_DPAD_UP', 'K_GP_DPAD_DOWN', 'K_GP_DPAD_LEFT', 'K_GP_DPAD_RIGHT',
  ]);

  // Cursor input is not usable on iPadOS, so a controller alone has to be
  // able to drive the menus: D-pad/stick navigate, triggers confirm.
  assert.match(inputBackend, /static void Web_GPNav \(int slot, qboolean down\)/);
  assert.match(inputBackend, /GP_NAV_REPEAT_DELAY/);
  const menuKeys = inputBackend.match(/static int Web_GPKeyForButton \(int index, qboolean gamekey\)\n\{([\s\S]*?)\n\}/)?.[1];
  assert.ok(menuKeys, 'Web_GPKeyForButton is defined');
  assert.match(menuKeys, /return K_ENTER;/);

  // The default binds use +altmodifier, which only in_sdl.c used to register.
  assert.match(inputBackend, /Cmd_AddCommand\("\+altmodifier", Web_GPAltModifierDown\)/);
  assert.match(inputBackend, /Cmd_AddCommand\("-altmodifier", Web_GPAltModifierUp\)/);

  // The Controller Options menu toggles the cvar by name.
  assert.match(inputBackend, /cvar_t in_gamepad = \{"gamepad",/);
});

test('the gamepad Start button always reaches the menu', () => {
  // A controller has no Escape key, and `~` needs a keyboard, so Start is the
  // only way out of the console on a pad. Dispatching it through the binding
  // table left a dead end: "togglemenu" from the console only closes the
  // console again, and "unbindall" at the head of config.cfg could remove the
  // default outright.
  const start = keys.match(/\tif \(key == K_GP_START\)\n\t\{([\s\S]*?)\n\t\}/)?.[1];
  assert.ok(start, 'Key_Event handles K_GP_START itself');
  assert.doesNotMatch(start, /keybindings/, 'Start must not depend on what it is bound to');
  for (const dest of ['key_message', 'key_menu', 'key_menubind', 'key_game', 'key_console']) {
    assert.match(start, new RegExp(`case ${dest}:`), `Start is handled from ${dest}`);
  }
  assert.match(start, /if \(key_dest == key_console\)\n\t{4}Key_SetDest \(key_game\);\n\t{3}M_ToggleMenu_f \(\);/,
    'from the console, Start leaves the console and opens the menu');
  assert.match(keys, /keyreserved\[K_GP_START\] = true;/,
    'reserved, so neither a rebind nor "unbindall" can take the menu away');

  // The bind prompt must not record a binding that can never fire.
  const keybind = menu.match(/void M_Keybind \(int key\)\n\{([\s\S]*?)\n\}/)?.[1];
  assert.ok(keybind, 'M_Keybind is defined');
  assert.match(keybind, /key != K_ESCAPE && key != '`' && key != K_GP_START/);

  // ...and the alt-modifier layer must not divert it either.
  assert.match(keys, /joy_altmodifier_pressed && key >= K_GP_A && key < K_GP_START/);
});
