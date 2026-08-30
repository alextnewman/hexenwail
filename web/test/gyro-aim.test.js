import test from 'node:test';
import assert from 'node:assert/strict';
import { GyroAim } from '../lib/gyro-aim.js';

function environment(overrides = {}) {
  const listeners = new Map();
  return {
    DeviceMotionEvent: class {},
    navigator: { getGamepads: () => [] },
    screen: { orientation: { angle: 0 } },
    addEventListener: (name, callback) => listeners.set(name, callback),
    removeEventListener: (name) => listeners.delete(name),
    requestAnimationFrame: () => 1,
    cancelAnimationFrame() {},
    listeners,
    ...overrides,
  };
}

test('device gyro follows native device axes instead of screen rotation', () => {
  const looks = [];
  const env = environment({ screen: { orientation: { angle: 90 } } });
  const gyro = new GyroAim({ look: (...values) => looks.push(values) },
    { deadZoneDegrees: 0 }, env);
  gyro.setEnabled(true);
  env.listeners.get('devicemotion')({
    rotationRate: { beta: 10, gamma: 20 },
    interval: 20,
    timeStamp: 100,
  });

  assert.equal(looks.length, 1);
  assert.ok(Math.abs(looks[0][0] - (20 * 0.02 / 0.022)) < 1e-9);
  assert.ok(Math.abs(looks[0][1] - (10 * 0.02 / 0.022)) < 1e-9);
});

test('device motion permission is requested before events are attached', async () => {
  let requests = 0;
  const env = environment({
    DeviceMotionEvent: class {
      static async requestPermission() {
        requests += 1;
        return 'granted';
      }
    },
  });
  const gyro = new GyroAim({ look() {} }, {}, env);
  gyro.setEnabled(true);
  assert.equal(env.listeners.has('devicemotion'), false);

  assert.equal(await gyro.requestPermission(), 'granted');
  assert.equal(requests, 1);
  assert.equal(env.listeners.has('devicemotion'), true);
});

test('compatible gamepad angular velocity is integrated when exposed', () => {
  const looks = [];
  const env = environment({
    navigator: { getGamepads: () => [{ pose: { angularVelocity: [1, 2, 0] } }] },
  });
  const gyro = new GyroAim({
    look: (...values) => looks.push(values),
    deviceActive: () => false,
    gamepadActive: () => true,
  }, { deadZoneDegrees: 0 }, env);
  gyro.setEnabled(true);
  gyro.pollGamepads(100);
  gyro.pollGamepads(120);

  assert.equal(looks.length, 1);
  assert.ok(Math.abs(looks[0][0] - (-2 * 180 / Math.PI * 0.02 / 0.022)) < 1e-9);
  assert.ok(Math.abs(looks[0][1] - (1 * 180 / Math.PI * 0.02 / 0.022)) < 1e-9);
});

test('inactive input sources do not move the view', () => {
  const looks = [];
  const env = environment({
    navigator: { getGamepads: () => [{ pose: { angularVelocity: [1, 2, 0] } }] },
  });
  const gyro = new GyroAim({
    look: (...values) => looks.push(values),
    deviceActive: () => false,
    gamepadActive: () => false,
  }, {}, env);
  gyro.setEnabled(true);
  gyro.handleDeviceMotion({ rotationRate: { beta: 20, gamma: 20 }, interval: 20, timeStamp: 100 });
  gyro.pollGamepads(100);
  gyro.pollGamepads(120);

  assert.deepEqual(looks, []);
});

test('gyro Y inversion is independent and leaves yaw unchanged', () => {
  const looks = [];
  const env = environment();
  const gyro = new GyroAim({ look: (...values) => looks.push(values) },
    { deadZoneDegrees: 0, invertY: true }, env);
  gyro.setEnabled(true);
  env.listeners.get('devicemotion')({
    rotationRate: { beta: 10, gamma: 20 },
    interval: 20,
    timeStamp: 100,
  });

  assert.ok(Math.abs(looks[0][0] - (20 * 0.02 / 0.022)) < 1e-9);
  assert.ok(Math.abs(looks[0][1] - (-10 * 0.02 / 0.022)) < 1e-9);
});
