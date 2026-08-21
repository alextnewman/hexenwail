import test from 'node:test';
import assert from 'node:assert/strict';

import { probeWebGPU } from '../lib/webgpu-probe.js';

function fixture(overrides = {}) {
  const configured = [];
  const device = {
    limits: {
      maxTextureDimension2D: 8192,
      maxTextureArrayLayers: 256,
      maxBindGroups: 4,
      maxBufferSize: 268435456,
    },
    lost: Promise.resolve({ reason: 'destroyed', message: 'test shutdown' }),
    destroy() {},
    ...overrides.device,
  };
  const adapter = {
    requestDevice: async () => device,
    ...overrides.adapter,
  };
  const navigatorObject = {
    gpu: {
      requestAdapter: async () => adapter,
      getPreferredCanvasFormat: () => 'bgra8unorm',
      ...overrides.gpu,
    },
  };
  const context = {
    configure: (value) => configured.push(value),
    ...overrides.context,
  };
  const canvas = {
    getContext: (name) => name === 'webgpu' ? context : null,
    ...overrides.canvas,
  };
  return { navigatorObject, canvas, device, adapter, context, configured };
}

test('probe acquires and configures a high-performance WebGPU device', async () => {
  const value = fixture();
  const result = await probeWebGPU(value);

  assert.equal(result.ok, true);
  assert.equal(result.handoff.device, value.device);
  assert.equal(result.handoff.context, value.context);
  assert.equal(result.handoff.format, 'bgra8unorm');
  assert.equal(result.handoff.limits.maxTextureArrayLayers, 256);
  assert.deepEqual(value.configured, [{
    device: value.device,
    format: 'bgra8unorm',
    alphaMode: 'opaque',
  }]);
});

test('probe reports browsers without WebGPU', async () => {
  const result = await probeWebGPU({
    canvas: {},
    navigatorObject: {},
  });
  assert.deepEqual(result, {
    ok: false,
    reason: 'WebGPU is unavailable in this browser.',
  });
});

test('probe reports a missing adapter', async () => {
  const value = fixture({ gpu: { requestAdapter: async () => null } });
  const result = await probeWebGPU(value);
  assert.equal(result.ok, false);
  assert.match(result.reason, /did not provide an adapter/);
});

test('probe records asynchronous device loss', async () => {
  let loseDevice;
  const lost = new Promise((resolve) => { loseDevice = resolve; });
  const value = fixture({ device: { lost } });
  const result = await probeWebGPU(value);
  let observed;
  result.handoff.onLost = (info) => { observed = info; };
  loseDevice({ reason: 'destroyed', message: 'test shutdown' });
  await lost;
  await Promise.resolve();

  assert.equal(result.handoff.lost, true);
  assert.equal(result.handoff.lossReason, 'destroyed');
  assert.equal(observed.message, 'test shutdown');
});
