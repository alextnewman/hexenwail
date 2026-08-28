import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { PhoneControls, PHONE_CONTROL_KEYCODES } from '../lib/phone-controls.js';

function makeElement(action, rect = { left: 0, top: 0, width: 120, height: 120 }) {
  const listeners = new Map();
  const style = new Map();
  return {
    dataset: action ? { phoneAction: action } : {},
    style: { setProperty: (name, value) => style.set(name, value), get: (name) => style.get(name) },
    closest(selector) {
      return selector.includes(this.dataset.phoneAction) || selector === '[data-phone-action]' ? this : null;
    },
    getBoundingClientRect: () => rect,
    addEventListener(name, callback) { listeners.set(name, callback); },
    removeEventListener(name) { listeners.delete(name); },
    setPointerCapture() {},
    releasePointerCapture() {},
    dispatch(name, event) { listeners.get(name)?.(event); },
  };
}

function pointer(target, pointerId, x, y) {
  return {
    target,
    pointerId,
    clientX: x,
    clientY: y,
    preventDefault() { this.defaultPrevented = true; },
    stopPropagation() { this.stopped = true; },
  };
}

test('stick owns one pointer and transitions movement keys without sticking', () => {
  const root = makeElement(null);
  const stick = makeElement('stick');
  const events = [];
  const controls = new PhoneControls(root, { key: (key, down) => events.push([key, down]), look() {} });
  controls.attach();

  root.dispatch('pointerdown', pointer(stick, 1, 60, 60));
  root.dispatch('pointermove', pointer(stick, 1, 60, 0));
  root.dispatch('pointermove', pointer(stick, 1, 120, 60));
  root.dispatch('pointerup', pointer(stick, 1, 120, 60));

  assert.deepEqual(events, [
    [PHONE_CONTROL_KEYCODES.forward, true],
    [PHONE_CONTROL_KEYCODES.forward, false],
    [PHONE_CONTROL_KEYCODES.right, true],
    [PHONE_CONTROL_KEYCODES.right, false],
  ]);
});

test('multi-touch buttons and look region keep independent pointer ownership', () => {
  const root = makeElement(null);
  const attack = makeElement('attack');
  const look = makeElement('look');
  const keys = [];
  const looks = [];
  const controls = new PhoneControls(root, {
    key: (key, down) => keys.push([key, down]),
    look: (dx, dy) => looks.push([dx, dy]),
  }, { lookSensitivity: 2, maxLookDelta: 10 });
  controls.attach();

  root.dispatch('pointerdown', pointer(attack, 7, 10, 10));
  root.dispatch('pointerdown', pointer(look, 8, 100, 100));
  root.dispatch('pointermove', pointer(look, 8, 140, 90));
  root.dispatch('pointerup', pointer(attack, 7, 10, 10));

  assert.deepEqual(keys, [[PHONE_CONTROL_KEYCODES.attack, true], [PHONE_CONTROL_KEYCODES.attack, false]]);
  assert.deepEqual(looks, [[20, -20]], 'look deltas are clamped before sensitivity scaling');
});

test('menu back button presses the controller B key', () => {
  const root = makeElement(null);
  const back = makeElement('menuBack');
  const events = [];
  const controls = new PhoneControls(root, { key: (key, down) => events.push([key, down]), look() {} });
  controls.attach();

  root.dispatch('pointerdown', pointer(back, 11, 10, 10));
  root.dispatch('pointerup', pointer(back, 11, 10, 10));

  assert.deepEqual(events, [
    [PHONE_CONTROL_KEYCODES.menuBack, true],
    [PHONE_CONTROL_KEYCODES.menuBack, false],
  ]);
});

test('gameplay controls use movement and action bindings that match their labels', () => {
  assert.deepEqual({
    forward: PHONE_CONTROL_KEYCODES.forward,
    back: PHONE_CONTROL_KEYCODES.back,
    left: PHONE_CONTROL_KEYCODES.left,
    right: PHONE_CONTROL_KEYCODES.right,
    attack: PHONE_CONTROL_KEYCODES.attack,
    jump: PHONE_CONTROL_KEYCODES.jump,
    use: PHONE_CONTROL_KEYCODES.use,
  }, {
    forward: 272, // K_TOUCH_FORWARD
    back: 273, // K_TOUCH_BACK
    left: 274, // K_TOUCH_LEFT
    right: 275, // K_TOUCH_RIGHT
    attack: 276, // K_TOUCH_ATTACK
    jump: 277, // K_TOUCH_JUMP
    use: 278, // K_TOUCH_USE
  });
});

test('each touch action rejects a second pointer', () => {
  const root = makeElement(null);
  const attack = makeElement('attack');
  const stick = makeElement('stick');
  const events = [];
  const controls = new PhoneControls(root, { key: (key, down) => events.push([key, down]), look() {} });
  controls.attach();

  root.dispatch('pointerdown', pointer(attack, 1, 10, 10));
  root.dispatch('pointerdown', pointer(attack, 2, 10, 10));
  root.dispatch('pointerup', pointer(attack, 1, 10, 10));
  root.dispatch('pointerdown', pointer(stick, 3, 60, 60));
  root.dispatch('pointerdown', pointer(stick, 4, 60, 60));
  root.dispatch('pointermove', pointer(stick, 4, 60, 0));

  assert.deepEqual(events, [
    [PHONE_CONTROL_KEYCODES.attack, true],
    [PHONE_CONTROL_KEYCODES.attack, false],
  ]);
});

test('releaseAll clears button and stick keys after cancellation or backgrounding', () => {
  const root = makeElement(null);
  const stick = makeElement('stick');
  const jump = makeElement('jump');
  const events = [];
  const controls = new PhoneControls(root, { key: (key, down) => events.push([key, down]), look() {} });
  controls.attach();

  root.dispatch('pointerdown', pointer(stick, 1, 60, 60));
  root.dispatch('pointermove', pointer(stick, 1, 60, 0));
  root.dispatch('pointerdown', pointer(jump, 2, 0, 0));
  controls.releaseAll();

  assert.deepEqual(events, [
    [PHONE_CONTROL_KEYCODES.forward, true],
    [PHONE_CONTROL_KEYCODES.jump, true],
    [PHONE_CONTROL_KEYCODES.forward, false],
    [PHONE_CONTROL_KEYCODES.jump, false],
  ]);
});

test('phone mode DOM includes playing layout, touch visibility rules, and quit hook', () => {
  const repoRoot = process.cwd();
  const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  assert.match(html, /body\[data-engine-state="running"\]/);
  assert.match(html, /id="phone-controls"/);
  assert.match(html, /id="exit-button"/);
  assert.match(html, /id="phone-exit-button"/);
  assert.match(html, /data-touch-only="true"/);
  assert.match(html, /data-phone-mode="true"/);
  assert.match(html, /data-phone-action="jump"[^>]*>Jump<\/button>/);
  assert.match(html, /data-phone-action="attack"[^>]*>Atk<\/button>/);
  assert.match(html, /data-phone-action="use"[^>]*>Use<\/button>/);
  assert.match(html, /data-phone-action="prevWeapon"[^>]*>Previous<\/button>/);
  assert.match(html, /data-phone-action="nextWeapon"[^>]*>Next<\/button>/);
  assert.match(html, /data-phone-action="menu"[^>]*>Menu<\/button>/);
  assert.match(html, /data-phone-action="forward"[^>]*>▲<\/button>/);
  assert.match(html, /data-phone-action="left"[^>]*>◀<\/button>/);
  assert.match(html, /data-phone-action="right"[^>]*>▶<\/button>/);
  assert.match(html, /data-phone-action="back"[^>]*>▼<\/button>/);
  assert.match(html, /data-phone-action="menuBack"[^>]*>Back<\/button>/);
  assert.match(html, /data-phone-action="menuSelect"[^>]*>Select<\/button>/);
  assert.match(html, /data-phone-action="menu"[^>]*>Resume<\/button>/);
  assert.match(html, /body\[data-touch-menu="true"\] \.phone-game-control \{ display: none; \}/);
  assert.match(app, /addEventListener\('hexenwailtouchmode'/);
  assert.match(html, /@media \(pointer: coarse\) and \(hover: none\) \{/);
  assert.match(app, /isLikelyTouchOnlyEnvironment/);
  assert.match(app, /isTouchControlsVisible/);
  assert.match(app, /Web_TouchControlsVisible/);
  assert.match(app, /isPhoneModeEnvironment/);
  assert.match(app, /PHONE_VIEWPORT_QUERY/);
  assert.match(app, /gamepadconnected/);
  assert.match(app, /hexenwailquit/);
  assert.match(app, /Web_ResizeCanvas/);
  assert.match(app, /const hadController = Boolean\(navigator\.serviceWorker\.controller\)/);
  assert.match(app, /addEventListener\('pageshow', checkForServiceWorkerUpdate\)/);
  assert.equal([...app.matchAll(/startEngineFromUserAction\(/g)].length, 2,
    'engine startup should only be defined and invoked by the launch-button handler');
});

test('phone mode keys off the panel short side so iPads are never trapped in it', () => {
  const repoRoot = process.cwd();
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  const query = app.match(/const PHONE_VIEWPORT_QUERY = '([^']+)'/)?.[1];
  assert.ok(query, 'PHONE_VIEWPORT_QUERY is declared as a string literal');

  const limits = [...query.matchAll(/max-(?:width|height): (\d+)px/g)].map((match) => Number(match[1]));
  assert.equal(limits.length, 2, 'both orientations are covered');
  for (const limit of limits) {
    // Phone short side <= ~450 CSS px; smallest iPad short side ~740 CSS px.
    assert.ok(limit >= 450 && limit < 700, `phone short-side limit ${limit} must exclude iPads`);
  }
  assert.doesNotMatch(query, /pointer|hover/,
    'phone mode is a panel-size question; an attached mouse does not make a phone panel bigger');
});

test('phone mode drives the immersive layout consistently in all three places', () => {
  const repoRoot = process.cwd();
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  const html = readFileSync(join(repoRoot, 'web/index.html'), 'utf8');
  // Forcing immersive, hiding Show launcher, and keeping immersive on
  // fullscreen exit must share one condition, or the button becomes a no-op.
  assert.match(app, /document\.body\.dataset\.immersive = \(state\.immersive \|\| state\.phoneMode\)/);
  assert.match(app, /\} else if \(!state\.phoneMode\) \{/);
  assert.match(html, /body\[data-phone-mode="true"\] #windowed-button \{ display: none; \}/);
});

test('coarse pointer changes are subscribed to now that phone mode ignores them', () => {
  const repoRoot = process.cwd();
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  const queries = app.match(/for \(const query of \[([\s\S]*?)\]\) \{/)?.[1];
  assert.ok(queries, 'the watched media query list is defined');
  assert.match(queries, /'\(any-pointer: coarse\)'/);
  assert.match(queries, /'\(any-pointer: fine\)'/);
  assert.match(queries, /'\(any-hover: hover\)'/);
});

test('canvas resizes coalesce so a burst of transitions schedules one pass', () => {
  const repoRoot = process.cwd();
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  const body = app.match(/function scheduleCanvasResize\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(body, 'scheduleCanvasResize is defined');
  assert.match(body, /if \(state\.canvasResizePending\) return;/,
    'each caller must not queue its own rAF chain into the engine');
  assert.match(body, /state\.canvasResizePending = true;/);
  assert.match(body, /state\.canvasResizePending = false;/);
});

test('touch-control auto detection depends on pointer capability, not viewport size', () => {
  const repoRoot = process.cwd();
  const app = readFileSync(join(repoRoot, 'web/app.js'), 'utf8');
  const body = app.match(/function isLikelyTouchOnlyEnvironment\(\) \{([\s\S]*?)\n\}/)?.[1];
  assert.ok(body, 'isLikelyTouchOnlyEnvironment is defined');
  assert.doesNotMatch(body, /isPhoneModeEnvironment|PHONE_VIEWPORT_QUERY/,
    'a bare iPad is as touch-only as a phone, so screen size must not gate touch controls');
  assert.match(body, /any-pointer: coarse/);
  assert.match(body, /hasConnectedGamepad\(\)/);
});
