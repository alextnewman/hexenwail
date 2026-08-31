/*
 * Emscripten JavaScript library for the software renderer's WebGPU presenter.
 * The live device/context handoff is populated by web/lib/webgpu-probe.js.
 */

const HexenwailWebGPULibrary = {
  $WebGPUCanvas__deps: [],
  $WebGPUCanvas: {
    state: null,

    shader: `
struct Params {
  sourceSize: vec2f,
  useSmoothing: u32,
  _pad: u32,
}

@group(0) @binding(0) var indexedTexture: texture_2d<u32>;
@group(0) @binding(1) var paletteTexture: texture_2d<f32>;
@group(0) @binding(2) var<uniform> params: Params;

struct VertexOutput {
  @builtin(position) position: vec4f,
  @location(0) uv: vec2f,
}

@vertex
fn vertexMain(@builtin(vertex_index) vertexIndex: u32) -> VertexOutput {
  let x = f32((vertexIndex << 1u) & 2u);
  let y = f32(vertexIndex & 2u);
  var output: VertexOutput;
  output.position = vec4f(vec2f(x, y) * 2.0 - 1.0, 0.0, 1.0);
  output.uv = vec2f(x, 1.0 - y);
  return output;
}

fn lookup(texel: vec2i) -> vec3f {
  let limit = vec2i(params.sourceSize) - vec2i(1);
  let index = textureLoad(indexedTexture, clamp(texel, vec2i(0), limit), 0).r;
  return textureLoad(paletteTexture, vec2i(i32(index), 0), 0).rgb;
}

@fragment
fn fragmentMain(input: VertexOutput) -> @location(0) vec4f {
  let pixel = input.uv * params.sourceSize;
  if (params.useSmoothing == 0u) {
    return vec4f(lookup(vec2i(floor(pixel))), 1.0);
  }

  let base = floor(pixel - vec2f(0.5));
  var fraction = pixel - vec2f(0.5) - base;
  let scale = max(vec2f(1.0), vec2f(1.0) /
    max(fwidth(pixel), vec2f(0.00001)));
  fraction = clamp(vec2f(0.5) + (fraction - vec2f(0.5)) * scale,
    vec2f(0.0), vec2f(1.0));
  let texel = vec2i(base);
  let top = mix(lookup(texel), lookup(texel + vec2i(1, 0)), fraction.x);
  let bottom = mix(lookup(texel + vec2i(0, 1)),
    lookup(texel + vec2i(1, 1)), fraction.x);
  return vec4f(mix(top, bottom, fraction.y), 1.0);
}`,

    rebuildBindGroup() {
      const state = this.state;
      if (!state?.indexedTexture || !state.paletteTexture) return;
      state.bindGroup = state.device.createBindGroup({
        layout: state.pipeline.getBindGroupLayout(0),
        entries: [
          { binding: 0, resource: state.indexedTexture.createView() },
          { binding: 1, resource: state.paletteTexture.createView() },
          { binding: 2, resource: { buffer: state.paramsBuffer } },
        ],
      });
    },
  },

  WebGPUCanvas_Init__deps: ['$WebGPUCanvas'],
  WebGPUCanvas_Init: function() {
    const handoff = Module.hexenwailWebGPU;
    if (!handoff?.device || !handoff.context || handoff.lost) return 0;

    const { device, context, format } = handoff;
    let pipeline;
    let paramsBuffer;
    let paletteTexture;
    try {
      device.pushErrorScope('validation');
      const module = device.createShaderModule({
        label: 'YouHexen2 indexed presenter',
        code: WebGPUCanvas.shader,
      });
      pipeline = device.createRenderPipeline({
        label: 'YouHexen2 indexed presenter',
        layout: 'auto',
        vertex: { module, entryPoint: 'vertexMain' },
        fragment: {
          module,
          entryPoint: 'fragmentMain',
          targets: [{ format }],
        },
        primitive: { topology: 'triangle-list' },
      });
      paramsBuffer = device.createBuffer({
        label: 'YouHexen2 presenter parameters',
        size: 16,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
      });
      paletteTexture = device.createTexture({
        label: 'YouHexen2 palette',
        size: { width: 256, height: 1 },
        format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      device.popErrorScope().then((error) => {
        if (!error) return;
        handoff.validationError = error.message;
        console.error(`Hexenwail WebGPU presenter validation failed: ${error.message}`);
      });
    } catch (error) {
      console.error(`Hexenwail WebGPU presenter initialization failed: ${error.message}`);
      return 0;
    }
    WebGPUCanvas.state = {
      device, context, pipeline, paramsBuffer, paletteTexture,
      indexedTexture: null, bindGroup: null,
      width: 0, height: 0, smooth: 0,
    };
    return 1;
  },

  WebGPUCanvas_Shutdown__deps: ['$WebGPUCanvas'],
  WebGPUCanvas_Shutdown: function() {
    const state = WebGPUCanvas.state;
    state?.indexedTexture?.destroy();
    state?.paletteTexture?.destroy();
    state?.paramsBuffer?.destroy();
    WebGPUCanvas.state = null;
  },

  WebGPUCanvas_SetSource__deps: ['$WebGPUCanvas'],
  WebGPUCanvas_SetSource: function(width, height) {
    const state = WebGPUCanvas.state;
    if (!state || width <= 0 || height <= 0) return;
    if (state.width === width && state.height === height) return;
    state.indexedTexture?.destroy();
    state.indexedTexture = state.device.createTexture({
      label: 'YouHexen2 indexed framebuffer',
      size: { width, height },
      format: 'r8uint',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    state.width = width;
    state.height = height;
    WebGPUCanvas.rebuildBindGroup();
  },

  WebGPUCanvas_SetPalette__deps: ['$WebGPUCanvas'],
  WebGPUCanvas_SetPalette: function(palettePointer) {
    const state = WebGPUCanvas.state;
    if (!state) return;
    const rgba = new Uint8Array(256 * 4);
    const palette = HEAPU8.subarray(palettePointer, palettePointer + 256 * 3);
    for (let i = 0; i < 256; ++i) {
      rgba[i * 4] = palette[i * 3];
      rgba[i * 4 + 1] = palette[i * 3 + 1];
      rgba[i * 4 + 2] = palette[i * 3 + 2];
      rgba[i * 4 + 3] = 255;
    }
    state.device.queue.writeTexture(
      { texture: state.paletteTexture },
      rgba,
      { bytesPerRow: 256 * 4, rowsPerImage: 1 },
      { width: 256, height: 1 },
    );
  },

  WebGPUCanvas_SetFilter__deps: ['$WebGPUCanvas'],
  WebGPUCanvas_SetFilter: function(smooth) {
    if (WebGPUCanvas.state) WebGPUCanvas.state.smooth = smooth ? 1 : 0;
  },

  WebGPUCanvas_Present__deps: ['$WebGPUCanvas'],
  WebGPUCanvas_Present: function(pixels, rowbytes, _canvasWidth, _canvasHeight,
    dstX, dstY, dstWidth, dstHeight) {
    const state = WebGPUCanvas.state;
    if (Module.hexenwailWebGPU?.lost) {
      abort('WebGPU device lost');
    }
    if (Module.hexenwailWebGPU?.validationError) {
      abort(`WebGPU presenter validation failed: ${Module.hexenwailWebGPU.validationError}`);
    }
    if (!state?.bindGroup) return;

    const source = HEAPU8.subarray(
      pixels, pixels + rowbytes * state.height);
    state.device.queue.writeTexture(
      { texture: state.indexedTexture },
      source,
      { offset: 0, bytesPerRow: rowbytes, rowsPerImage: state.height },
      { width: state.width, height: state.height },
    );

    const params = new ArrayBuffer(16);
    const floats = new Float32Array(params);
    const integers = new Uint32Array(params);
    floats[0] = state.width;
    floats[1] = state.height;
    integers[2] = state.smooth;
    state.device.queue.writeBuffer(state.paramsBuffer, 0, params);

    const encoder = state.device.createCommandEncoder({
      label: 'YouHexen2 presenter commands',
    });
    const pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: state.context.getCurrentTexture().createView(),
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
        loadOp: 'clear',
        storeOp: 'store',
      }],
    });
    pass.setViewport(dstX, dstY, dstWidth, dstHeight, 0, 1);
    pass.setPipeline(state.pipeline);
    pass.setBindGroup(0, state.bindGroup);
    pass.draw(3);
    pass.end();
    state.device.queue.submit([encoder.finish()]);
  },
};

mergeInto(LibraryManager.library, HexenwailWebGPULibrary);
