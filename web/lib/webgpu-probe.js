function failure(reason) {
  return { ok: false, reason };
}

export async function probeWebGPU({ canvas, navigatorObject = globalThis.navigator } = {}) {
  if (!canvas) return failure('The game canvas is unavailable.');
  if (!navigatorObject?.gpu) return failure('WebGPU is unavailable in this browser.');

  let adapter;
  try {
    adapter = await navigatorObject.gpu.requestAdapter({
      powerPreference: 'high-performance',
    });
  } catch (error) {
    return failure(`WebGPU adapter request failed: ${error.message}`);
  }
  if (!adapter) return failure('WebGPU did not provide an adapter.');

  let device;
  try {
    device = await adapter.requestDevice();
  } catch (error) {
    return failure(`WebGPU device request failed: ${error.message}`);
  }

  const context = canvas.getContext('webgpu');
  if (!context) {
    device.destroy();
    return failure('The game canvas could not create a WebGPU context.');
  }

  const format = navigatorObject.gpu.getPreferredCanvasFormat();
  try {
    context.configure({
      device,
      format,
      alphaMode: 'opaque',
    });
  } catch (error) {
    device.destroy();
    return failure(`WebGPU canvas configuration failed: ${error.message}`);
  }

  const handoff = {
    adapter,
    device,
    context,
    format,
    lost: false,
    limits: {
      maxTextureDimension2D: device.limits.maxTextureDimension2D,
      maxTextureArrayLayers: device.limits.maxTextureArrayLayers,
      maxBindGroups: device.limits.maxBindGroups,
      maxBufferSize: device.limits.maxBufferSize,
    },
  };
  device.lost.then((info) => {
    handoff.lost = true;
    handoff.lossReason = info?.reason ?? 'unknown';
    handoff.lossMessage = info?.message ?? '';
    handoff.onLost?.(info);
  });

  return { ok: true, handoff };
}
