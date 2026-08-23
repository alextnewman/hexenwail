/*
 * webgpu_nitro.js -- the WebGlideNitro backend: a native WebGPU renderer.
 *
 * This is NOT a GL translation layer and it is not the software renderer's
 * WebGPU presenter (engine/web/webgpu_present.js).  It owns real pipelines,
 * real vertex/index buffers and real bind groups, and the engine hands it
 * batched world geometry -- see docs/web/WEBGLIDE_NITRO.md.
 *
 * Three things it deliberately does that a GL port cannot:
 *
 *   - Per-texture bind groups and their parameter uniforms are built once,
 *     at texture load time, and are immutable afterwards.  Drawing a batch
 *     costs one setBindGroup and one drawIndexed.
 *   - The world's vertex buffer is created mappedAtCreation and never gets
 *     COPY_DST: it is immutable for the lifetime of the map.
 *   - A frame is one command encoder and two render passes (scene, canvas),
 *     submitted once.  The C side calls into JS four times per frame.
 *
 * Palette semantics are the software renderer's, not WebGlide's: the world
 * shader looks the texel index up in the authored colormap row that the
 * lightmap intensity selects, exactly as d_scan.c does, and only then
 * resolves the palette.
 *
 * Copyright (C) 2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

const HexenwailNitroLibrary = {
  $Nitro__deps: [],
  $Nitro: {
    state: null,

    /* Vertex strides, mirrored by wgpu_nitro.h. */
    WORLD_STRIDE: 32,
    UI_STRIDE: 20,

    /* Texture parameter flags, mirrored by wgpu_nitro.h. */
    TEX_HOLEY: 1,
    TEX_ALPHA: 2,
    TEX_WRAP: 4,

    worldShader: `
struct Frame {
  mvp : mat4x4f,
  fullbright : f32,
  pad0 : f32,
  pad1 : f32,
  pad2 : f32,
}

struct TexParams {
  size : vec2f,
  flags : u32,
  pad : u32,
}

@group(0) @binding(0) var<uniform> frame : Frame;
@group(0) @binding(1) var paletteTexture : texture_2d<f32>;
@group(0) @binding(2) var colormapTexture : texture_2d<u32>;
@group(0) @binding(3) var lightmapTexture : texture_2d_array<f32>;
@group(0) @binding(4) var lightmapSampler : sampler;

@group(1) @binding(0) var diffuseTexture : texture_2d<u32>;
@group(1) @binding(1) var<uniform> texParams : TexParams;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
  @location(1) lightmap : vec3f,
}

@vertex
fn vertexMain(@location(0) position : vec3f,
              @location(1) uv : vec2f,
              @location(2) lightmap : vec3f) -> VertexOutput {
  var output : VertexOutput;
  output.position = frame.mvp * vec4f(position, 1.0);
  output.uv = uv;
  output.lightmap = lightmap;
  return output;
}

@fragment
fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {
  let size = vec2i(texParams.size);
  let texel = vec2i(floor(input.uv * texParams.size));
  let wrapped = ((texel % size) + size) % size;
  let index = textureLoad(diffuseTexture, wrapped, 0).r;

  if ((texParams.flags & 1u) != 0u && index == 0u) {
    discard;
  }
  if ((texParams.flags & 2u) != 0u && index == 255u) {
    discard;
  }

  /* d_scan.c indexes colormap[(light & 0xFF00) + texel]; light is
   * (255 * 256 - blocklight) >> 2, so the row an 8-bit lightmap sample
   * selects is (255 - sample) >> 2, i.e. (1 - intensity) * 63.75. */
  var row = 0;
  if (frame.fullbright == 0.0 && input.lightmap.z >= 0.0) {
    let light = textureSampleLevel(lightmapTexture, lightmapSampler,
        input.lightmap.xy, i32(input.lightmap.z), 0.0).r;
    row = clamp(i32(floor((1.0 - light) * 63.75)), 0, 63);
  }
  let shaded = textureLoad(colormapTexture, vec2i(i32(index), row), 0).r;
  return vec4f(textureLoad(paletteTexture, vec2i(i32(shaded), 0), 0).rgb, 1.0);
}`,

    scanoutShader: `
struct ScanParams {
  gamma : f32,
  contrast : f32,
  pad0 : f32,
  pad1 : f32,
  tint : vec4f,
}

@group(0) @binding(0) var sceneTexture : texture_2d<f32>;
@group(0) @binding(1) var sceneSampler : sampler;
@group(0) @binding(2) var<uniform> scan : ScanParams;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
}

@vertex
fn vertexMain(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
  let x = f32((vertexIndex << 1u) & 2u);
  let y = f32(vertexIndex & 2u);
  var output : VertexOutput;
  output.position = vec4f(vec2f(x, y) * 2.0 - 1.0, 0.0, 1.0);
  output.uv = vec2f(x, 1.0 - y);
  return output;
}

@fragment
fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {
  var rgb = textureSampleLevel(sceneTexture, sceneSampler, input.uv, 0.0).rgb;
  rgb = mix(rgb, scan.tint.rgb, scan.tint.a);
  rgb = (rgb - vec3f(0.5)) * scan.contrast + vec3f(0.5);
  rgb = pow(max(rgb, vec3f(0.0)), vec3f(scan.gamma));
  return vec4f(clamp(rgb, vec3f(0.0), vec3f(1.0)), 1.0);
}`,

    uiShader: `
struct UIParams {
  screen : vec2f,
  gamma : f32,
  contrast : f32,
}

struct TexParams {
  size : vec2f,
  flags : u32,
  pad : u32,
}

@group(0) @binding(0) var<uniform> ui : UIParams;
@group(0) @binding(1) var paletteTexture : texture_2d<f32>;

@group(1) @binding(0) var diffuseTexture : texture_2d<u32>;
@group(1) @binding(1) var<uniform> texParams : TexParams;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
  @location(1) color : vec4f,
}

@vertex
fn vertexMain(@location(0) position : vec2f,
              @location(1) uv : vec2f,
              @location(2) color : vec4f) -> VertexOutput {
  var output : VertexOutput;
  let clip = vec2f(position.x / ui.screen.x * 2.0 - 1.0,
                   1.0 - position.y / ui.screen.y * 2.0);
  output.position = vec4f(clip, 0.0, 1.0);
  output.uv = uv;
  output.color = color;
  return output;
}

@fragment
fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {
  let size = vec2i(texParams.size);
  var texel = vec2i(floor(input.uv * texParams.size));
  if ((texParams.flags & 4u) != 0u) {
    texel = ((texel % size) + size) % size;
  } else {
    texel = clamp(texel, vec2i(0), size - vec2i(1));
  }
  let index = textureLoad(diffuseTexture, texel, 0).r;

  if ((texParams.flags & 1u) != 0u && index == 0u) {
    discard;
  }
  if ((texParams.flags & 2u) != 0u && index == 255u) {
    discard;
  }

  var rgb = textureLoad(paletteTexture, vec2i(i32(index), 0), 0).rgb * input.color.rgb;
  rgb = (rgb - vec3f(0.5)) * ui.contrast + vec3f(0.5);
  rgb = pow(max(rgb, vec3f(0.0)), vec3f(ui.gamma));
  return vec4f(clamp(rgb, vec3f(0.0), vec3f(1.0)), input.color.a);
}`,

    fail(message) {
      const handoff = Module.hexenwailWebGPU;
      if (handoff && !handoff.validationError) handoff.validationError = message;
      if (typeof err === 'function') err(`WebGlideNitro: ${message}`);
      else console.error(`WebGlideNitro: ${message}`);
    },

    /* Anything the device rejects is a bug in this renderer, not a
     * recoverable condition: surface it once, loudly, with the WGSL or
     * layout message intact. */
    watchErrors(device, label) {
      device.popErrorScope().then((error) => {
        if (error) Nitro.fail(`${label}: ${error.message}`);
      }).catch(() => {});
    },

    checkDevice() {
      const handoff = Module.hexenwailWebGPU;
      if (handoff?.lost) abort('WebGPU device lost');
      if (handoff?.validationError) {
        abort(`WebGlideNitro validation failed: ${handoff.validationError}`);
      }
      return Nitro.state;
    },

    /* The frame bind group holds the palette, the colormap and the map's
     * lightmap array; only the last of those ever changes, and only when a
     * map is loaded or unloaded. */
    rebuildFrameBindGroup() {
      const state = Nitro.state;
      state.frameBindGroup = state.device.createBindGroup({
        label: 'WebGlideNitro frame',
        layout: state.frameGroupLayout,
        entries: [
          { binding: 0, resource: { buffer: state.frameUniform } },
          { binding: 1, resource: state.paletteTexture.createView() },
          { binding: 2, resource: state.colormapTexture.createView() },
          { binding: 3, resource: state.lightmapView },
          { binding: 4, resource: state.lightmapSampler },
        ],
      });
    },

    /* Scene colour + depth, reallocated only when the resolution ladder
     * actually moves. */
    ensureScene(width, height) {
      const state = Nitro.state;
      if (state.sceneWidth === width && state.sceneHeight === height) return;
      state.sceneColor?.destroy();
      state.sceneDepth?.destroy();
      state.sceneColor = state.device.createTexture({
        label: 'WebGlideNitro scene colour',
        size: { width, height },
        format: 'rgba8unorm',
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING,
      });
      state.sceneDepth = state.device.createTexture({
        label: 'WebGlideNitro scene depth',
        size: { width, height },
        format: 'depth24plus',
        usage: GPUTextureUsage.RENDER_ATTACHMENT,
      });
      state.sceneColorView = state.sceneColor.createView();
      state.sceneDepthView = state.sceneDepth.createView();
      state.scanoutBindGroup = state.device.createBindGroup({
        label: 'WebGlideNitro scan-out',
        layout: state.scanoutGroupLayout,
        entries: [
          { binding: 0, resource: state.sceneColorView },
          { binding: 1, resource: state.sceneSampler },
          { binding: 2, resource: { buffer: state.scanoutUniform } },
        ],
      });
      state.sceneWidth = width;
      state.sceneHeight = height;
    },

    ensureUploadBuffer(name, bytes, usage) {
      const state = Nitro.state;
      const current = state[name];
      if (current && current.size >= bytes) return current;
      current?.destroy();
      const size = Math.max(bytes, current ? current.size * 2 : 65536);
      state[name] = state.device.createBuffer({
        label: `WebGlideNitro ${name}`,
        size: (size + 3) & ~3,
        usage: usage | GPUBufferUsage.COPY_DST,
      });
      return state[name];
    },
  },

  /*
   * Lifecycle
   */

  Nitro_Init__deps: ['$Nitro'],
  Nitro_Init: function () {
    const handoff = Module.hexenwailWebGPU;
    if (!handoff?.device || !handoff.context || handoff.lost) return 0;

    const { device, context, format } = handoff;
    try {
      device.pushErrorScope('validation');

      const frameGroupLayout = device.createBindGroupLayout({
        label: 'WebGlideNitro frame',
        entries: [
          { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform' } },
          { binding: 1, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'unfilterable-float' } },
          { binding: 2, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'uint' } },
          { binding: 3, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'float', viewDimension: '2d-array' } },
          { binding: 4, visibility: GPUShaderStage.FRAGMENT,
            sampler: { type: 'filtering' } },
        ],
      });
      /* One layout for every indexed texture in the game, shared by the
       * world and the 2D pipelines, so a texture's bind group is built
       * once at load time and never rebuilt. */
      const textureGroupLayout = device.createBindGroupLayout({
        label: 'WebGlideNitro texture',
        entries: [
          { binding: 0, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'uint' } },
          { binding: 1, visibility: GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform' } },
        ],
      });
      const scanoutGroupLayout = device.createBindGroupLayout({
        label: 'WebGlideNitro scan-out',
        entries: [
          { binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: {} },
          { binding: 1, visibility: GPUShaderStage.FRAGMENT,
            sampler: { type: 'filtering' } },
          { binding: 2, visibility: GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform' } },
        ],
      });
      const uiGroupLayout = device.createBindGroupLayout({
        label: 'WebGlideNitro 2D',
        entries: [
          { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform' } },
          { binding: 1, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'unfilterable-float' } },
        ],
      });

      const worldModule = device.createShaderModule({
        label: 'WebGlideNitro world', code: Nitro.worldShader,
      });
      const scanoutModule = device.createShaderModule({
        label: 'WebGlideNitro scan-out', code: Nitro.scanoutShader,
      });
      const uiModule = device.createShaderModule({
        label: 'WebGlideNitro 2D', code: Nitro.uiShader,
      });

      const worldPipeline = device.createRenderPipeline({
        label: 'WebGlideNitro world',
        layout: device.createPipelineLayout({
          bindGroupLayouts: [frameGroupLayout, textureGroupLayout],
        }),
        vertex: {
          module: worldModule,
          entryPoint: 'vertexMain',
          buffers: [{
            arrayStride: Nitro.WORLD_STRIDE,
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x3' },
              { shaderLocation: 1, offset: 12, format: 'float32x2' },
              { shaderLocation: 2, offset: 20, format: 'float32x3' },
            ],
          }],
        },
        fragment: {
          module: worldModule,
          entryPoint: 'fragmentMain',
          targets: [{ format: 'rgba8unorm' }],
        },
        /* Quake's world winding, the same one WebGlide culls with. */
        primitive: { topology: 'triangle-list', frontFace: 'ccw', cullMode: 'front' },
        depthStencil: {
          format: 'depth24plus',
          depthWriteEnabled: true,
          depthCompare: 'less-equal',
        },
      });

      const scanoutPipeline = device.createRenderPipeline({
        label: 'WebGlideNitro scan-out',
        layout: device.createPipelineLayout({ bindGroupLayouts: [scanoutGroupLayout] }),
        vertex: { module: scanoutModule, entryPoint: 'vertexMain' },
        fragment: {
          module: scanoutModule, entryPoint: 'fragmentMain', targets: [{ format }],
        },
        primitive: { topology: 'triangle-list' },
      });

      const uiPipeline = device.createRenderPipeline({
        label: 'WebGlideNitro 2D',
        layout: device.createPipelineLayout({
          bindGroupLayouts: [uiGroupLayout, textureGroupLayout],
        }),
        vertex: {
          module: uiModule,
          entryPoint: 'vertexMain',
          buffers: [{
            arrayStride: Nitro.UI_STRIDE,
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x2' },
              { shaderLocation: 1, offset: 8, format: 'float32x2' },
              { shaderLocation: 2, offset: 16, format: 'unorm8x4' },
            ],
          }],
        },
        fragment: {
          module: uiModule,
          entryPoint: 'fragmentMain',
          targets: [{
            format,
            blend: {
              color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
              alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' },
            },
          }],
        },
        primitive: { topology: 'triangle-list' },
      });

      const frameUniform = device.createBuffer({
        label: 'WebGlideNitro frame uniform',
        size: 80,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
      });
      const scanoutUniform = device.createBuffer({
        label: 'WebGlideNitro scan-out uniform',
        size: 32,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
      });
      const uiUniform = device.createBuffer({
        label: 'WebGlideNitro 2D uniform',
        size: 16,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
      });
      const paletteTexture = device.createTexture({
        label: 'WebGlideNitro palette',
        size: { width: 256, height: 1 },
        format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      const colormapTexture = device.createTexture({
        label: 'WebGlideNitro colormap',
        size: { width: 256, height: 64 },
        format: 'r8uint',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      /* A 1x1x1 stand-in so the world bind group is complete before a map
       * is loaded; unlit geometry never samples it anyway. */
      const placeholderLightmap = device.createTexture({
        label: 'WebGlideNitro lightmap placeholder',
        size: { width: 1, height: 1, depthOrArrayLayers: 1 },
        format: 'r8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      const lightmapSampler = device.createSampler({
        label: 'WebGlideNitro lightmap sampler',
        magFilter: 'linear', minFilter: 'linear',
        addressModeU: 'clamp-to-edge', addressModeV: 'clamp-to-edge',
      });
      const sceneSampler = device.createSampler({
        label: 'WebGlideNitro scan-out sampler',
        magFilter: 'linear', minFilter: 'linear',
        addressModeU: 'clamp-to-edge', addressModeV: 'clamp-to-edge',
      });

      Nitro.state = {
        device, context, format,
        frameGroupLayout, textureGroupLayout, scanoutGroupLayout, uiGroupLayout,
        worldPipeline, scanoutPipeline, uiPipeline,
        frameUniform, scanoutUniform, uiUniform,
        paletteTexture, colormapTexture, lightmapSampler, sceneSampler,
        placeholderLightmap,
        lightmapTexture: null, lightmapView: placeholderLightmap.createView({
          dimension: '2d-array',
        }),
        frameBindGroup: null,
        uiBindGroup: null,
        textures: [null],
        world: null,
        sceneColor: null, sceneDepth: null,
        sceneColorView: null, sceneDepthView: null,
        sceneWidth: 0, sceneHeight: 0,
        scanoutBindGroup: null,
        worldIndexBuffer: null, uiVertexBuffer: null,
        encoder: null, sceneReady: false,
        scanout: null,
        canvasWidth: 0, canvasHeight: 0,
      };

      Nitro.state.frameBindGroup = null;
      Nitro.rebuildFrameBindGroup();
      Nitro.state.uiBindGroup = device.createBindGroup({
        label: 'WebGlideNitro 2D',
        layout: uiGroupLayout,
        entries: [
          { binding: 0, resource: { buffer: uiUniform } },
          { binding: 1, resource: paletteTexture.createView() },
        ],
      });

      Nitro.watchErrors(device, 'initialization');
    } catch (error) {
      Nitro.fail(`initialization failed: ${error.message}`);
      Nitro.state = null;
      return 0;
    }
    return 1;
  },

  Nitro_Shutdown__deps: ['$Nitro'],
  Nitro_Shutdown: function () {
    const state = Nitro.state;
    if (!state) return;
    for (const entry of state.textures) {
      entry?.texture?.destroy();
      entry?.params?.destroy();
    }
    state.world?.vertexBuffer?.destroy();
    state.worldIndexBuffer?.destroy();
    state.uiVertexBuffer?.destroy();
    state.lightmapTexture?.destroy();
    state.placeholderLightmap?.destroy();
    state.sceneColor?.destroy();
    state.sceneDepth?.destroy();
    state.frameUniform.destroy();
    state.scanoutUniform.destroy();
    state.uiUniform.destroy();
    state.paletteTexture.destroy();
    state.colormapTexture.destroy();
    Nitro.state = null;
  },

  Nitro_Resize__deps: ['$Nitro'],
  Nitro_Resize: function (width, height) {
    const state = Nitro.state;
    if (!state) return;
    state.canvasWidth = width;
    state.canvasHeight = height;
  },

  /*
   * Palette and colormap: the indexed pipeline's two constants.
   */

  Nitro_SetPalette__deps: ['$Nitro'],
  Nitro_SetPalette: function (palettePointer) {
    const state = Nitro.state;
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
      { texture: state.paletteTexture }, rgba,
      { bytesPerRow: 256 * 4, rowsPerImage: 1 }, { width: 256, height: 1 });
  },

  Nitro_SetColormap__deps: ['$Nitro'],
  Nitro_SetColormap: function (colormapPointer, rows) {
    const state = Nitro.state;
    if (!state || rows <= 0) return;
    const count = Math.min(rows, 64);
    const source = HEAPU8.subarray(colormapPointer, colormapPointer + count * 256);
    state.device.queue.writeTexture(
      { texture: state.colormapTexture }, source,
      { bytesPerRow: 256, rowsPerImage: count }, { width: 256, height: count });
  },

  /*
   * Indexed textures.  The bind group and its parameter uniform are built
   * here, once, and are immutable from then on.
   */

  Nitro_CreateTexture__deps: ['$Nitro'],
  Nitro_CreateTexture: function (namePointer, width, height, pixelPointer, flags) {
    const state = Nitro.state;
    if (!state || width <= 0 || height <= 0) return 0;
    const name = namePointer ? UTF8ToString(namePointer) : 'texture';
    let texture;
    let params;
    try {
      state.device.pushErrorScope('validation');
      texture = state.device.createTexture({
        label: `WebGlideNitro ${name}`,
        size: { width, height },
        format: 'r8uint',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      state.device.queue.writeTexture(
        { texture },
        HEAPU8.subarray(pixelPointer, pixelPointer + width * height),
        { bytesPerRow: width, rowsPerImage: height },
        { width, height });

      params = state.device.createBuffer({
        label: `WebGlideNitro ${name} params`,
        size: 16,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
      });
      const values = new ArrayBuffer(16);
      new Float32Array(values, 0, 2).set([width, height]);
      new Uint32Array(values, 8, 1)[0] = flags >>> 0;
      state.device.queue.writeBuffer(params, 0, values);

      const bindGroup = state.device.createBindGroup({
        label: `WebGlideNitro ${name}`,
        layout: state.textureGroupLayout,
        entries: [
          { binding: 0, resource: texture.createView() },
          { binding: 1, resource: { buffer: params } },
        ],
      });
      Nitro.watchErrors(state.device, `texture ${name}`);
      state.textures.push({ texture, params, bindGroup });
    } catch (error) {
      Nitro.fail(`texture ${name} failed: ${error.message}`);
      texture?.destroy();
      params?.destroy();
      return 0;
    }
    return state.textures.length - 1;
  },

  /*
   * The static world: an immutable vertex buffer and a lightmap array,
   * both built once per map.
   */

  Nitro_CreateWorld__deps: ['$Nitro'],
  Nitro_CreateWorld: function (vertexPointer, vertexCount, maxIndices,
    lightmapLayers, lightmapSize) {
    const state = Nitro.state;
    if (!state || vertexCount <= 0) return 0;
    try {
      state.device.pushErrorScope('validation');
      const bytes = vertexCount * Nitro.WORLD_STRIDE;
      /* No COPY_DST: the world's geometry never changes, so the buffer is
       * written once through its initial mapping and is immutable after. */
      const vertexBuffer = state.device.createBuffer({
        label: 'WebGlideNitro world vertices',
        size: (bytes + 3) & ~3,
        usage: GPUBufferUsage.VERTEX,
        mappedAtCreation: true,
      });
      new Uint8Array(vertexBuffer.getMappedRange(0, bytes)).set(
        HEAPU8.subarray(vertexPointer, vertexPointer + bytes));
      vertexBuffer.unmap();

      state.worldIndexBuffer?.destroy();
      state.worldIndexBuffer = state.device.createBuffer({
        label: 'WebGlideNitro world indices',
        size: Math.max((maxIndices * 4 + 3) & ~3, 4),
        usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST,
      });

      state.lightmapTexture?.destroy();
      state.lightmapTexture = null;
      if (lightmapLayers > 0 && lightmapSize > 0) {
        state.lightmapTexture = state.device.createTexture({
          label: 'WebGlideNitro lightmap array',
          size: { width: lightmapSize, height: lightmapSize,
            depthOrArrayLayers: lightmapLayers },
          format: 'r8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
        });
        state.lightmapView = state.lightmapTexture.createView({ dimension: '2d-array' });
      } else {
        state.lightmapView = state.placeholderLightmap.createView({ dimension: '2d-array' });
      }
      Nitro.rebuildFrameBindGroup();

      state.world?.vertexBuffer?.destroy();
      state.world = { vertexBuffer, vertexCount, lightmapSize };
      Nitro.watchErrors(state.device, 'world geometry');
    } catch (error) {
      Nitro.fail(`world geometry failed: ${error.message}`);
      state.world = null;
      return 0;
    }
    return 1;
  },

  Nitro_UploadLightmap__deps: ['$Nitro'],
  Nitro_UploadLightmap: function (layer, texelPointer) {
    const state = Nitro.state;
    if (!state?.lightmapTexture || !state.world) return;
    const size = state.world.lightmapSize;
    state.device.queue.writeTexture(
      { texture: state.lightmapTexture, origin: { x: 0, y: 0, z: layer } },
      HEAPU8.subarray(texelPointer, texelPointer + size * size),
      { bytesPerRow: size, rowsPerImage: size },
      { width: size, height: size, depthOrArrayLayers: 1 });
  },

  Nitro_DestroyWorld__deps: ['$Nitro'],
  Nitro_DestroyWorld: function () {
    const state = Nitro.state;
    if (!state) return;
    state.world?.vertexBuffer?.destroy();
    state.world = null;
    state.lightmapTexture?.destroy();
    state.lightmapTexture = null;
    state.lightmapView = state.placeholderLightmap.createView({ dimension: '2d-array' });
    Nitro.rebuildFrameBindGroup();
  },

  /*
   * The frame: one encoder, one scene pass, one canvas pass, one submit.
   */

  Nitro_BeginFrame__deps: ['$Nitro'],
  Nitro_BeginFrame: function () {
    const state = Nitro.checkDevice();
    if (!state) return;
    state.encoder = state.device.createCommandEncoder({
      label: 'WebGlideNitro frame',
    });
    state.sceneReady = false;
    state.scanout = null;
  },

  /*
   * params (see wgpuscene_t in wgpu_nitro.h):
   *   [0..15]  mvp, column major
   *   [16..19] polyblend tint rgba
   *   [20]     gamma        [21] contrast
   *   [22]     scene width  [23] scene height   (integers)
   *   [24..27] destination rect on the canvas   (integers)
   *   [28]     flags: 1 = fullbright
   * batches are int quads of { textureId, firstIndex, indexCount, unused }.
   */
  Nitro_DrawScene__deps: ['$Nitro'],
  Nitro_DrawScene: function (paramsPointer, indexPointer, indexCount,
    batchPointer, batchCount) {
    const state = Nitro.checkDevice();
    if (!state?.encoder || !state.world) return;

    const floats = HEAPF32.subarray(paramsPointer >> 2, (paramsPointer >> 2) + 29);
    const integers = HEAP32.subarray(paramsPointer >> 2, (paramsPointer >> 2) + 29);
    const sceneWidth = Math.max(integers[22], 1);
    const sceneHeight = Math.max(integers[23], 1);

    Nitro.ensureScene(sceneWidth, sceneHeight);
    state.scanout = {
      x: integers[24], y: integers[25],
      width: Math.max(integers[26], 1), height: Math.max(integers[27], 1),
    };

    const frame = new Float32Array(20);
    frame.set(floats.subarray(0, 16));
    frame[16] = (integers[28] & 1) ? 1.0 : 0.0;
    state.device.queue.writeBuffer(state.frameUniform, 0, frame);

    const scan = new Float32Array(8);
    scan[0] = floats[20];
    scan[1] = floats[21];
    scan.set(floats.subarray(16, 20), 4);
    state.device.queue.writeBuffer(state.scanoutUniform, 0, scan);

    const pass = state.encoder.beginRenderPass({
      label: 'WebGlideNitro scene',
      colorAttachments: [{
        view: state.sceneColorView,
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
        loadOp: 'clear',
        storeOp: 'store',
      }],
      depthStencilAttachment: {
        view: state.sceneDepthView,
        depthClearValue: 1.0,
        depthLoadOp: 'clear',
        depthStoreOp: 'discard',
      },
    });
    state.sceneReady = true;

    if (indexCount > 0 && batchCount > 0 && state.worldIndexBuffer) {
      state.device.queue.writeBuffer(state.worldIndexBuffer, 0,
        HEAPU8, indexPointer, indexCount * 4);
      pass.setPipeline(state.worldPipeline);
      pass.setBindGroup(0, state.frameBindGroup);
      pass.setVertexBuffer(0, state.world.vertexBuffer);
      pass.setIndexBuffer(state.worldIndexBuffer, 'uint32');

      const batches = HEAP32.subarray(batchPointer >> 2,
        (batchPointer >> 2) + batchCount * 4);
      let boundTexture = -1;
      for (let i = 0; i < batchCount; ++i) {
        const entry = state.textures[batches[i * 4]];
        const first = batches[i * 4 + 1];
        const count = batches[i * 4 + 2];
        if (!entry || count <= 0) continue;
        if (batches[i * 4] !== boundTexture) {
          pass.setBindGroup(1, entry.bindGroup);
          boundTexture = batches[i * 4];
        }
        pass.drawIndexed(count, 1, first, 0, 0);
      }
    }
    pass.end();
  },

  /*
   * params: [0] screen width, [1] screen height, [2] gamma, [3] contrast.
   * runs are int quads of { textureId, firstVertex, vertexCount, unused }.
   */
  Nitro_EndFrame__deps: ['$Nitro'],
  Nitro_EndFrame: function (paramsPointer, vertexPointer, vertexCount,
    runPointer, runCount) {
    const state = Nitro.checkDevice();
    if (!state?.encoder) return;

    let view;
    try {
      view = state.context.getCurrentTexture().createView();
    } catch (error) {
      Nitro.fail(`canvas texture unavailable: ${error.message}`);
      state.encoder = null;
      return;
    }

    const pass = state.encoder.beginRenderPass({
      label: 'WebGlideNitro canvas',
      colorAttachments: [{
        view,
        clearValue: { r: 0, g: 0, b: 0, a: 1 },
        loadOp: 'clear',
        storeOp: 'store',
      }],
    });

    if (state.sceneReady && state.scanout) {
      const { x, y, width, height } = state.scanout;
      pass.setViewport(x, y, width, height, 0, 1);
      pass.setPipeline(state.scanoutPipeline);
      pass.setBindGroup(0, state.scanoutBindGroup);
      pass.draw(3);
      pass.setViewport(0, 0, state.canvasWidth || width, state.canvasHeight || height, 0, 1);
    }

    if (vertexCount > 0 && runCount > 0) {
      const bytes = vertexCount * Nitro.UI_STRIDE;
      const buffer = Nitro.ensureUploadBuffer('uiVertexBuffer', bytes,
        GPUBufferUsage.VERTEX);
      state.device.queue.writeBuffer(buffer, 0, HEAPU8, vertexPointer, bytes);
      state.device.queue.writeBuffer(state.uiUniform, 0,
        HEAPF32.subarray(paramsPointer >> 2, (paramsPointer >> 2) + 4));

      pass.setPipeline(state.uiPipeline);
      pass.setBindGroup(0, state.uiBindGroup);
      pass.setVertexBuffer(0, buffer);

      const runs = HEAP32.subarray(runPointer >> 2, (runPointer >> 2) + runCount * 4);
      let boundTexture = -1;
      for (let i = 0; i < runCount; ++i) {
        const entry = state.textures[runs[i * 4]];
        const first = runs[i * 4 + 1];
        const count = runs[i * 4 + 2];
        if (!entry || count <= 0) continue;
        if (runs[i * 4] !== boundTexture) {
          pass.setBindGroup(1, entry.bindGroup);
          boundTexture = runs[i * 4];
        }
        pass.draw(count, 1, first, 0);
      }
    }

    pass.end();
    state.device.queue.submit([state.encoder.finish()]);
    state.encoder = null;
  },
};

mergeInto(LibraryManager.library, HexenwailNitroLibrary);
