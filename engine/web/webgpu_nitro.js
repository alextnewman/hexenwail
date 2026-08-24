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
 *     submitted once.  The C side calls into JS four times per frame,
 *     however many entities are on screen: the whole scene arrives as one
 *     description.
 *   - Brush entities draw out of that same immutable world buffer, with
 *     their transform, alpha and flat light level in one 256-byte block of
 *     a uniform arena addressed by dynamic offset.  A door costs an offset,
 *     not a buffer.
 *
 * Palette semantics are the software renderer's, not WebGlide's: the world
 * shader looks the texel index up in the authored colormap row that the
 * lightmap intensity selects, exactly as d_scan.c does, and only then
 * resolves the palette.  Models carry the colormap row d_polyse.c would
 * have interpolated, and Hexen II's colorshade stays an index-to-index
 * remap through gfx/tinttab.lmp rather than becoming an RGB multiply.
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
    PARTICLE_STRIDE: 20,
    MODEL_STRIDE: 28,

    /* Per-entity uniform block, padded to WebGPU's minimum dynamic uniform
     * buffer offset alignment so the whole arena uploads in one write. */
    ENTITY_STRIDE: 256,

    /* Texture parameter flags, mirrored by wgpu_nitro.h. */
    TEX_HOLEY: 1,
    TEX_ALPHA: 2,
    TEX_WRAP: 4,
    TEX_TURB: 8,

    /* Model batch flags, mirrored by wgpu_nitro.h. */
    MODEL_BLEND_ALPHA: 1,
    MODEL_BLEND_ADD: 2,
    MODEL_VIEWMODEL: 4,
    MODEL_GLOW: 8,
    MODEL_SHADOW: 16,

    /* The view weapon is drawn into the near 30% of the depth range so it
     * cannot poke through the wall the player is standing against.  WebGPU
     * has no glDepthRange: the range belongs to the viewport. */
    VIEWMODEL_DEPTH: 0.3,

    worldShader: `
struct Frame {
  mvp : mat4x4f,
  fullbright : f32,
  modelFullbrights : f32,
  pad1 : f32,
  pad2 : f32,
  eye : vec3f,
  time : f32,
  fogColor : vec3f,
  fogDensity : f32,
}

struct TexParams {
  size : vec2f,
  flags : u32,
  pad : u32,
}

/* One block per drawn thing: the world is block 0, every brush entity gets
 * its own.  Bound with a dynamic offset, so a door costs an offset, not a
 * buffer. */
struct Entity {
  mvp : mat4x4f,
  alpha : f32,
  light : f32,
  pad0 : f32,
  pad1 : f32,
}

@group(0) @binding(0) var<uniform> frame : Frame;
@group(0) @binding(1) var paletteTexture : texture_2d<f32>;
@group(0) @binding(2) var colormapTexture : texture_2d<u32>;
@group(0) @binding(3) var lightmapTexture : texture_2d_array<f32>;
@group(0) @binding(4) var lightmapSampler : sampler;

@group(1) @binding(0) var diffuseTexture : texture_2d<u32>;
@group(1) @binding(1) var<uniform> texParams : TexParams;

@group(2) @binding(0) var<uniform> entity : Entity;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
  @location(1) lightmap : vec3f,
  @location(2) fogDistance : f32,
}

@vertex
fn vertexMain(@location(0) position : vec3f,
              @location(1) uv : vec2f,
              @location(2) lightmap : vec3f) -> VertexOutput {
  var output : VertexOutput;
  output.position = entity.mvp * vec4f(position, 1.0);
  output.uv = uv;
  output.lightmap = lightmap;
  output.fogDistance = abs(output.position.w);
  return output;
}

@fragment
fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {
  let size = vec2i(texParams.size);
  var uv = input.uv;
  if ((texParams.flags & 8u) != 0u) {
    let phase = frame.time * 1.5;
    uv += vec2f(sin(input.uv.y * texParams.size.y * 0.125 + phase),
                sin(input.uv.x * texParams.size.x * 0.125 + phase)) *
          (2.0 / texParams.size);
  }
  let texel = vec2i(floor(uv * texParams.size));
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
  if (frame.fullbright == 0.0) {
    if (entity.light >= 0.0) {
      /* A Hexen II model light style: one flat row for the whole
       * submodel, which is what makes an MLS_ABSLIGHT lift uniform. */
      row = clamp(i32(floor((1.0 - entity.light) * 63.75)), 0, 63);
    } else if (input.lightmap.z >= 0.0) {
      let light = textureSampleLevel(lightmapTexture, lightmapSampler,
          input.lightmap.xy, i32(input.lightmap.z), 0.0).r;
      row = clamp(i32(floor((1.0 - light) * 63.75)), 0, 63);
    }
  }
  let shaded = textureLoad(colormapTexture, vec2i(i32(index), row), 0).r;
  let rgb = textureLoad(paletteTexture, vec2i(i32(shaded), 0), 0).rgb;
  let fog = clamp(1.0 - exp2(-frame.fogDensity * frame.fogDensity *
      input.fogDistance * input.fogDistance), 0.0, 1.0);
  return vec4f(mix(rgb, frame.fogColor, fog), entity.alpha);
}`,

    /*
     * Hexen II's authored sky is two 128x128 indexed layers.  Projecting
     * from the eye direction prevents the texture from looking pasted onto
     * the BSP polygons; independent scroll rates make the punched-out cloud
     * layer move over the solid storm layer.  Index zero is the cloud mask,
     * so the final colour remains an exact palette lookup.
     */
    skyShader: `
struct Frame {
  mvp : mat4x4f,
  fullbright : f32,
  modelFullbrights : f32,
  pad1 : f32,
  pad2 : f32,
  eye : vec3f,
  time : f32,
  fogColor : vec3f,
  fogDensity : f32,
}

struct Entity {
  mvp : mat4x4f,
  alpha : f32,
  light : f32,
  pad0 : f32,
  pad1 : f32,
}

@group(0) @binding(1) var paletteTexture : texture_2d<f32>;
@group(0) @binding(0) var<uniform> frame : Frame;
@group(1) @binding(0) var solidTexture : texture_2d<u32>;
@group(1) @binding(1) var cloudTexture : texture_2d<u32>;
@group(2) @binding(0) var<uniform> entity : Entity;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) worldPosition : vec3f,
}

@vertex
fn vertexMain(@location(0) position : vec3f) -> VertexOutput {
  var output : VertexOutput;
  output.position = entity.mvp * vec4f(position, 1.0);
  output.worldPosition = position;
  return output;
}

fn skyIndex(texture : texture_2d<u32>, coordinate : vec2f) -> u32 {
  let size = vec2i(textureDimensions(texture));
  let texel = vec2i(floor(coordinate));
  let wrapped = ((texel % size) + size) % size;
  return textureLoad(texture, wrapped, 0).r;
}

@fragment
fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {
  var direction = input.worldPosition - frame.eye;
  direction.z *= 3.0;
  let projection = direction.xy * (6.0 * 63.0 /
      max(length(direction), 0.001));
  let solid = skyIndex(solidTexture, projection + vec2f(frame.time * 8.0));
  let cloud = skyIndex(cloudTexture, projection + vec2f(frame.time * 16.0));
  let index = select(solid, cloud, cloud != 0u);
  let rgb = textureLoad(paletteTexture, vec2i(i32(index), 0), 0).rgb;
  let distance = length(direction);
  let fog = clamp(1.0 - exp2(-frame.fogDensity * frame.fogDensity *
      distance * distance), 0.0, 1.0);
  return vec4f(mix(rgb, frame.fogColor, fog), 1.0);
}`,

    /*
     * Models and sprites.  The CPU has already posed, transformed and lit
     * the vertices, so the vertex stage is a single matrix multiply; what
     * matters is that the fragment stage is still indexed.  light is the
     * colormap row d_polyse.c would have interpolated, and shade.y is the
     * gfx/tinttab.lmp row Hexen II's colorshade selects -- an index to
     * index remap, exactly as R_AliasDrawModel builds globalcolormap.
     */
    modelShader: `
struct Frame {
  mvp : mat4x4f,
  fullbright : f32,
  modelFullbrights : f32,
  pad1 : f32,
  pad2 : f32,
  eye : vec3f,
  time : f32,
  fogColor : vec3f,
  fogDensity : f32,
}

struct TexParams {
  size : vec2f,
  flags : u32,
  pad : u32,
}

@group(0) @binding(0) var<uniform> frame : Frame;
@group(0) @binding(1) var paletteTexture : texture_2d<f32>;
@group(0) @binding(2) var colormapTexture : texture_2d<u32>;
@group(0) @binding(5) var tintTexture : texture_2d<u32>;

@group(1) @binding(0) var diffuseTexture : texture_2d<u32>;
@group(1) @binding(1) var<uniform> texParams : TexParams;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
  @location(1) light : f32,
  @location(2) alpha : f32,
  @location(3) @interpolate(flat) tint : u32,
  @location(4) fogDistance : f32,
}

@vertex
fn vertexMain(@location(0) position : vec3f,
              @location(1) uv : vec2f,
              @location(2) light : f32,
              @location(3) shade : vec4u) -> VertexOutput {
  var output : VertexOutput;
  output.position = frame.mvp * vec4f(position, 1.0);
  output.uv = uv;
  output.light = light;
  output.alpha = f32(shade.x) / 255.0;
  output.tint = shade.y;
  output.fogDistance = abs(output.position.w);
  return output;
}

@fragment
fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {
  let size = vec2i(texParams.size);
  let texel = clamp(vec2i(floor(input.uv * texParams.size)),
                    vec2i(0), size - vec2i(1));
  let index = textureLoad(diffuseTexture, texel, 0).r;

  if ((texParams.flags & 1u) != 0u && index == 0u) {
    discard;
  }
  if ((texParams.flags & 2u) != 0u && index == 255u) {
    discard;
  }

  var shaded = index;
  /* A negative row is the unlit sentinel sprites use: d_sprite.c never
   * touches the colormap, so neither does this. */
  if (input.light >= 0.0 && frame.fullbright == 0.0 &&
      !(frame.modelFullbrights != 0.0 && index >= 224u)) {
    let row = clamp(i32(input.light), 0, 63);
    shaded = textureLoad(colormapTexture, vec2i(i32(index), row), 0).r;
  }
  if (input.tint != 0u) {
    shaded = textureLoad(tintTexture,
        vec2i(i32(shaded), i32(input.tint)), 0).r;
  }
  let rgb = textureLoad(paletteTexture, vec2i(i32(shaded), 0), 0).rgb;
  let fog = clamp(1.0 - exp2(-frame.fogDensity * frame.fogDensity *
      input.fogDistance * input.fogDistance), 0.0, 1.0);
  return vec4f(mix(rgb, frame.fogColor, fog), input.alpha);
}`,

    effectShader: `
struct Frame {
  mvp : mat4x4f,
  fullbright : f32,
  modelFullbrights : f32,
  pad1 : f32,
  pad2 : f32,
  eye : vec3f,
  time : f32,
  fogColor : vec3f,
  fogDensity : f32,
}
@group(0) @binding(0) var<uniform> frame : Frame;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
  @location(1) color : vec4f,
  @location(2) fogDistance : f32,
}

@vertex
fn vertexMain(@location(0) position : vec3f,
              @location(1) uv : vec2f,
              /* Location 2 is model light; effects carry packed RGBA at 3. */
              @location(3) color : vec4f) -> VertexOutput {
  var output : VertexOutput;
  output.position = frame.mvp * vec4f(position, 1.0);
  output.uv = uv;
  output.color = color;
  output.fogDistance = abs(output.position.w);
  return output;
}

@fragment
fn glowMain(input : VertexOutput) -> @location(0) vec4f {
  let radius = length(input.uv - vec2f(0.5)) * 2.0;
  let falloff = 1.0 - smoothstep(0.0, 1.0, radius);
  let fog = exp2(-frame.fogDensity * frame.fogDensity *
      input.fogDistance * input.fogDistance);
  return vec4f(input.color.rgb * falloff * fog, input.color.a * falloff);
}

@fragment
fn shadowMain(input : VertexOutput) -> @location(0) vec4f {
  let fog = clamp(1.0 - exp2(-frame.fogDensity * frame.fogDensity *
      input.fogDistance * input.fogDistance), 0.0, 1.0);
  return vec4f(mix(vec3f(0.0), frame.fogColor, fog),
      input.color.a * (1.0 - fog));
}`,

    particleShader: `
struct Frame {
  mvp : mat4x4f,
  fullbright : f32,
  modelFullbrights : f32,
  pad1 : f32,
  pad2 : f32,
  eye : vec3f,
  time : f32,
  fogColor : vec3f,
  fogDensity : f32,
}

struct ParticleParams {
  right : vec4f,
  up : vec4f,
}

@group(0) @binding(0) var<uniform> frame : Frame;
@group(1) @binding(0) var<uniform> particle : ParticleParams;

struct VertexOutput {
  @builtin(position) position : vec4f,
  @location(0) color : vec4f,
  @location(1) fogDistance : f32,
}

@vertex
fn vertexMain(@builtin(vertex_index) vertexIndex : u32,
              @location(0) origin : vec3f,
              @location(1) scale : f32,
              @location(2) color : vec4f) -> VertexOutput {
  const corners = array<vec2f, 6>(
    vec2f(-0.5, -0.5), vec2f(-0.5, 0.5), vec2f(0.5, 0.5),
    vec2f(-0.5, -0.5), vec2f(0.5, 0.5), vec2f(0.5, -0.5));
  let corner = corners[vertexIndex];
  let position = origin + particle.right.xyz * corner.x * scale
                        + particle.up.xyz * corner.y * scale;
  var output : VertexOutput;
  output.position = frame.mvp * vec4f(position, 1.0);
  output.color = color;
  output.fogDistance = abs(output.position.w);
  return output;
}

@fragment
fn fragmentMain(input : VertexOutput) -> @location(0) vec4f {
  let fog = clamp(1.0 - exp2(-frame.fogDensity * frame.fogDensity *
      input.fogDistance * input.fogDistance), 0.0, 1.0);
  return vec4f(mix(input.color.rgb, frame.fogColor, fog), input.color.a);
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

    /* The frame bind group holds the palette, the colormap, the tint table
     * and the map's lightmap array; only the last of those ever changes,
     * and only when a map is loaded or unloaded. */
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
          { binding: 5, resource: state.tintTexture.createView() },
        ],
      });
    },

    /* The per-entity uniform arena.  One bind group serves every block in
     * it, because the blocks are addressed by dynamic offset; it is only
     * rebuilt when the arena itself has to grow. */
    ensureEntityBuffer(bytes) {
      const state = Nitro.state;
      const before = state.entityBuffer;
      const buffer = Nitro.ensureUploadBuffer('entityBuffer',
        Math.max(bytes, Nitro.ENTITY_STRIDE), GPUBufferUsage.UNIFORM);
      if (buffer !== before || !state.entityBindGroup) {
        state.entityBindGroup = state.device.createBindGroup({
          label: 'WebGlideNitro entity',
          layout: state.entityGroupLayout,
          entries: [
            { binding: 0,
              resource: { buffer, offset: 0, size: Nitro.ENTITY_STRIDE } },
          ],
        });
      }
      return buffer;
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
        format: 'depth24plus-stencil8',
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
          /* gfx/tinttab.lmp: 256 rows of index-to-index remap, one per
           * Hexen II colorshade.  Only the model pipelines read it. */
          { binding: 5, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'uint' } },
        ],
      });
      /* One layout for every indexed texture in the game, shared by the
       * world, model and 2D pipelines, so a texture's bind group is built
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
      const skyGroupLayout = device.createBindGroupLayout({
        label: 'WebGlideNitro sky',
        entries: [
          { binding: 0, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'uint' } },
          { binding: 1, visibility: GPUShaderStage.FRAGMENT,
            texture: { sampleType: 'uint' } },
        ],
      });
      /* Dynamic offset, not a storage buffer: Safari's compatibility
       * limits allow zero storage buffers in the vertex stage, and the
       * target device is an iPad. */
      const entityGroupLayout = device.createBindGroupLayout({
        label: 'WebGlideNitro entity',
        entries: [
          { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform', hasDynamicOffset: true,
                      minBindingSize: Nitro.ENTITY_STRIDE } },
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
      const particleGroupLayout = device.createBindGroupLayout({
        label: 'WebGlideNitro particles',
        entries: [
          { binding: 0, visibility: GPUShaderStage.VERTEX,
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
      const skyModule = device.createShaderModule({
        label: 'WebGlideNitro sky', code: Nitro.skyShader,
      });
      const scanoutModule = device.createShaderModule({
        label: 'WebGlideNitro scan-out', code: Nitro.scanoutShader,
      });
      const particleModule = device.createShaderModule({
        label: 'WebGlideNitro particles', code: Nitro.particleShader,
      });
      const uiModule = device.createShaderModule({
        label: 'WebGlideNitro 2D', code: Nitro.uiShader,
      });
      const modelModule = device.createShaderModule({
        label: 'WebGlideNitro models', code: Nitro.modelShader,
      });
      const effectModule = device.createShaderModule({
        label: 'WebGlideNitro model effects', code: Nitro.effectShader,
      });

      const ALPHA_BLEND = {
        color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
        alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' },
      };
      const ADD_BLEND = {
        color: { srcFactor: 'src-alpha', dstFactor: 'one' },
        alpha: { srcFactor: 'one', dstFactor: 'one' },
      };

      const worldLayout = device.createPipelineLayout({
        bindGroupLayouts: [frameGroupLayout, textureGroupLayout, entityGroupLayout],
      });
      const worldVertex = {
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
      };
      /* Quake's world winding, the same one WebGlide culls with. */
      const worldPrimitive = {
        topology: 'triangle-list', frontFace: 'ccw', cullMode: 'front',
      };

      const worldPipeline = device.createRenderPipeline({
        label: 'WebGlideNitro world',
        layout: worldLayout,
        vertex: worldVertex,
        fragment: {
          module: worldModule,
          entryPoint: 'fragmentMain',
          targets: [{ format: 'rgba8unorm' }],
        },
        primitive: worldPrimitive,
        depthStencil: {
          format: 'depth24plus-stencil8',
          depthWriteEnabled: true,
          depthCompare: 'less-equal',
        },
      });

      const skyPipeline = device.createRenderPipeline({
        label: 'WebGlideNitro sky',
        layout: device.createPipelineLayout({
          bindGroupLayouts: [frameGroupLayout, skyGroupLayout, entityGroupLayout],
        }),
        vertex: {
          module: skyModule,
          entryPoint: 'vertexMain',
          buffers: worldVertex.buffers,
        },
        fragment: {
          module: skyModule,
          entryPoint: 'fragmentMain',
          targets: [{ format: 'rgba8unorm' }],
        },
        primitive: worldPrimitive,
        depthStencil: {
          format: 'depth24plus-stencil8',
          depthWriteEnabled: true,
          depthCompare: 'less-equal',
        },
      });

      /* Translucent brush entities -- the water a func_train carries, a
       * DRF_TRANSLUCENT door -- share the module and the geometry; only
       * the blend state and the depth write differ. */
      const worldBlendPipeline = device.createRenderPipeline({
        label: 'WebGlideNitro world blended',
        layout: worldLayout,
        vertex: worldVertex,
        fragment: {
          module: worldModule,
          entryPoint: 'fragmentMain',
          targets: [{ format: 'rgba8unorm', blend: ALPHA_BLEND }],
        },
        primitive: worldPrimitive,
        depthStencil: {
          format: 'depth24plus-stencil8',
          depthWriteEnabled: false,
          depthCompare: 'less-equal',
        },
      });

      const modelLayout = device.createPipelineLayout({
        bindGroupLayouts: [frameGroupLayout, textureGroupLayout],
      });
      const modelVertex = {
        module: modelModule,
        entryPoint: 'vertexMain',
        buffers: [{
          arrayStride: Nitro.MODEL_STRIDE,
          attributes: [
            { shaderLocation: 0, offset: 0, format: 'float32x3' },
            { shaderLocation: 1, offset: 12, format: 'float32x2' },
            { shaderLocation: 2, offset: 20, format: 'float32' },
            { shaderLocation: 3, offset: 24, format: 'uint8x4' },
          ],
        }],
      };
      /* Models are not culled.  Alias winding varies with the entity's
       * scale sign and sprites are built to face the eye, so the depth
       * test is the thing that decides visibility, as it does in the
       * software rasteriser's own back-face-free model path. */
      const modelPrimitive = { topology: 'triangle-list', cullMode: 'none' };
      const modelPipeline = (label, blend, depthWrite) =>
        device.createRenderPipeline({
          label,
          layout: modelLayout,
          vertex: modelVertex,
          fragment: {
            module: modelModule,
            entryPoint: 'fragmentMain',
            targets: [blend ? { format: 'rgba8unorm', blend }
                            : { format: 'rgba8unorm' }],
          },
          primitive: modelPrimitive,
          depthStencil: {
            format: 'depth24plus-stencil8',
            depthWriteEnabled: depthWrite,
            depthCompare: 'less-equal',
          },
        });

      const modelOpaquePipeline =
        modelPipeline('WebGlideNitro models', null, true);
      const modelAlphaPipeline =
        modelPipeline('WebGlideNitro models blended', ALPHA_BLEND, false);
      const modelAddPipeline =
        modelPipeline('WebGlideNitro models additive', ADD_BLEND, false);
      const effectVertex = {
        module: effectModule,
        entryPoint: 'vertexMain',
        buffers: [{
          arrayStride: Nitro.MODEL_STRIDE,
          attributes: [
            { shaderLocation: 0, offset: 0, format: 'float32x3' },
            { shaderLocation: 1, offset: 12, format: 'float32x2' },
            { shaderLocation: 2, offset: 20, format: 'float32' },
            { shaderLocation: 3, offset: 24, format: 'unorm8x4' },
          ],
        }],
      };
      const effectPipeline = (label, entryPoint, blend, stencil = false) =>
        device.createRenderPipeline({
          label,
          layout: modelLayout,
          vertex: effectVertex,
          fragment: {
            module: effectModule,
            entryPoint,
            targets: [{ format: 'rgba8unorm', blend }],
          },
          primitive: modelPrimitive,
          depthStencil: {
            format: 'depth24plus-stencil8',
            depthWriteEnabled: false,
            depthCompare: 'less-equal',
            ...(stencil ? {
              stencilFront: {
                compare: 'equal',
                failOp: 'keep',
                depthFailOp: 'keep',
                passOp: 'increment-clamp',
              },
              stencilBack: {
                compare: 'equal',
                failOp: 'keep',
                depthFailOp: 'keep',
                passOp: 'increment-clamp',
              },
              stencilReadMask: 0xff,
              stencilWriteMask: 0xff,
            } : {}),
          },
        });
      const modelGlowPipeline =
        effectPipeline('WebGlideNitro glows', 'glowMain', ADD_BLEND);
      const modelShadowPipeline =
        effectPipeline('WebGlideNitro shadows', 'shadowMain', ALPHA_BLEND, true);

      const scanoutPipeline = device.createRenderPipeline({
        label: 'WebGlideNitro scan-out',
        layout: device.createPipelineLayout({ bindGroupLayouts: [scanoutGroupLayout] }),
        vertex: { module: scanoutModule, entryPoint: 'vertexMain' },
        fragment: {
          module: scanoutModule, entryPoint: 'fragmentMain', targets: [{ format }],
        },
        primitive: { topology: 'triangle-list' },
      });

      const particlePipeline = device.createRenderPipeline({
        label: 'WebGlideNitro particles',
        layout: device.createPipelineLayout({
          bindGroupLayouts: [frameGroupLayout, particleGroupLayout],
        }),
        vertex: {
          module: particleModule,
          entryPoint: 'vertexMain',
          buffers: [{
            arrayStride: Nitro.PARTICLE_STRIDE,
            stepMode: 'instance',
            attributes: [
              { shaderLocation: 0, offset: 0, format: 'float32x3' },
              { shaderLocation: 1, offset: 12, format: 'float32' },
              { shaderLocation: 2, offset: 16, format: 'unorm8x4' },
            ],
          }],
        },
        fragment: {
          module: particleModule,
          entryPoint: 'fragmentMain',
          targets: [{
            format: 'rgba8unorm',
            blend: {
              color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
              alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' },
            },
          }],
        },
        primitive: { topology: 'triangle-list' },
        depthStencil: {
          format: 'depth24plus-stencil8',
          depthWriteEnabled: false,
          depthCompare: 'less-equal',
        },
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
        size: 128,
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
      const particleUniform = device.createBuffer({
        label: 'WebGlideNitro particle uniform',
        size: 32,
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
      /* gfx/tinttab.lmp, seeded with the identity so a data set that has no
       * tint table renders as if no entity ever had a colorshade rather
       * than turning every tinted item black. */
      const tintTexture = device.createTexture({
        label: 'WebGlideNitro tint table',
        size: { width: 256, height: 256 },
        format: 'r8uint',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      const identityTint = new Uint8Array(256 * 256);
      for (let row = 0; row < 256; ++row) {
        for (let i = 0; i < 256; ++i) identityTint[row * 256 + i] = i;
      }
      device.queue.writeTexture(
        { texture: tintTexture }, identityTint,
        { bytesPerRow: 256, rowsPerImage: 256 }, { width: 256, height: 256 });
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
        frameGroupLayout, textureGroupLayout, skyGroupLayout,
        scanoutGroupLayout, uiGroupLayout,
        particleGroupLayout, entityGroupLayout,
        worldPipeline, worldBlendPipeline, skyPipeline,
        particlePipeline, scanoutPipeline,
        uiPipeline,
        modelOpaquePipeline, modelAlphaPipeline, modelAddPipeline,
        modelGlowPipeline, modelShadowPipeline,
        frameUniform, scanoutUniform, uiUniform, particleUniform,
        paletteTexture, colormapTexture, tintTexture, lightmapSampler, sceneSampler,
        placeholderLightmap,
        lightmapTexture: null, lightmapView: placeholderLightmap.createView({
          dimension: '2d-array',
        }),
        frameBindGroup: null,
        uiBindGroup: null,
        textures: [null],
        freeTextures: [],
        world: null,
        sceneColor: null, sceneDepth: null,
        sceneColorView: null, sceneDepthView: null,
        sceneWidth: 0, sceneHeight: 0,
        scanoutBindGroup: null,
        worldIndexBuffer: null, particleBuffer: null, uiVertexBuffer: null,
        modelVertexBuffer: null, entityBuffer: null, entityBindGroup: null,
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
      Nitro.state.particleBindGroup = device.createBindGroup({
        label: 'WebGlideNitro particles',
        layout: particleGroupLayout,
        entries: [
          { binding: 0, resource: { buffer: particleUniform } },
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
      entry?.solid?.destroy();
      entry?.clouds?.destroy();
    }
    state.world?.vertexBuffer?.destroy();
    state.worldIndexBuffer?.destroy();
    state.particleBuffer?.destroy();
    state.uiVertexBuffer?.destroy();
    state.modelVertexBuffer?.destroy();
    state.entityBuffer?.destroy();
    state.lightmapTexture?.destroy();
    state.placeholderLightmap?.destroy();
    state.sceneColor?.destroy();
    state.sceneDepth?.destroy();
    state.frameUniform.destroy();
    state.scanoutUniform.destroy();
    state.uiUniform.destroy();
    state.particleUniform.destroy();
    state.paletteTexture.destroy();
    state.colormapTexture.destroy();
    state.tintTexture.destroy();
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
   * gfx/tinttab.lmp: 256 rows of index-to-index remap.  R_AliasDrawModel
   * rebuilds globalcolormap through row colorshade; the GPU does the same
   * lookup per fragment, which is what keeps a tinted item indexed instead
   * of turning it into an RGB multiply.
   */
  Nitro_SetTintTable__deps: ['$Nitro'],
  Nitro_SetTintTable: function (tablePointer) {
    const state = Nitro.state;
    if (!state || !tablePointer) return;
    state.device.queue.writeTexture(
      { texture: state.tintTexture },
      HEAPU8.subarray(tablePointer, tablePointer + 256 * 256),
      { bytesPerRow: 256, rowsPerImage: 256 }, { width: 256, height: 256 });
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
      /* Slot 0 is reserved as "no texture"; freed slots are reused so a map
       * change does not grow the table for ever. */
      const slot = state.freeTextures.length ? state.freeTextures.pop()
                                             : state.textures.length;
      state.textures[slot] = { texture, params, bindGroup, width, height };
      return slot;
    } catch (error) {
      Nitro.fail(`texture ${name} failed: ${error.message}`);
      texture?.destroy();
      params?.destroy();
      return 0;
    }
  },

  Nitro_CreateSky__deps: ['$Nitro'],
  Nitro_CreateSky: function (namePointer, width, height, solidPointer, cloudPointer) {
    const state = Nitro.state;
    if (!state || width <= 0 || height <= 0) return 0;
    const name = namePointer ? UTF8ToString(namePointer) : 'sky';
    let solid;
    let clouds;
    let errorScope = false;
    try {
      state.device.pushErrorScope('validation');
      errorScope = true;
      const createLayer = (label, pointer) => {
        const texture = state.device.createTexture({
          label: `WebGlideNitro ${name} ${label}`,
          size: { width, height },
          format: 'r8uint',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
        });
        state.device.queue.writeTexture(
          { texture },
          HEAPU8.subarray(pointer, pointer + width * height),
          { bytesPerRow: width, rowsPerImage: height },
          { width, height });
        return texture;
      };
      solid = createLayer('solid', solidPointer);
      clouds = createLayer('clouds', cloudPointer);
      const bindGroup = state.device.createBindGroup({
        label: `WebGlideNitro ${name} sky`,
        layout: state.skyGroupLayout,
        entries: [
          { binding: 0, resource: solid.createView() },
          { binding: 1, resource: clouds.createView() },
        ],
      });
      Nitro.watchErrors(state.device, `sky ${name}`);
      errorScope = false;
      const slot = state.freeTextures.length ? state.freeTextures.pop()
                                             : state.textures.length;
      state.textures[slot] = { sky: true, solid, clouds, bindGroup, width, height };
      return slot;
    } catch (error) {
      if (errorScope) state.device.popErrorScope().catch(() => {});
      Nitro.fail(`sky ${name} failed: ${error.message}`);
      solid?.destroy();
      clouds?.destroy();
      return 0;
    }
  },

  /*
   * Re-specify an existing texture's texels.  Player skins are the only
   * thing that needs this: their pixels are a translation of the base skin
   * through the entity's colormap, so a colour change has to land on the
   * same GPU texture rather than leaking a new one every frame.
   */
  Nitro_UpdateTexture__deps: ['$Nitro'],
  Nitro_UpdateTexture: function (id, pixelPointer) {
    const state = Nitro.state;
    const entry = state?.textures[id];
    if (!entry?.texture) return;
    const { width, height } = entry;
    state.device.queue.writeTexture(
      { texture: entry.texture },
      HEAPU8.subarray(pixelPointer, pixelPointer + width * height),
      { bytesPerRow: width, rowsPerImage: height },
      { width, height });
  },

  Nitro_DestroyTexture__deps: ['$Nitro'],
  Nitro_DestroyTexture: function (id) {
    const state = Nitro.state;
    const entry = (id > 0) ? state?.textures[id] : null;
    if (!entry) return;
    entry.texture?.destroy();
    entry.params?.destroy();
    entry.solid?.destroy();
    entry.clouds?.destroy();
    state.textures[id] = null;
    state.freeTextures.push(id);
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
   *   [28]     flags: 1 = fullbright, 2 = model fullbright pixels
   *   [29..32] particle right basis vector
   *   [33..36] particle up basis vector
   *   [37..39] sky eye position    [40] sky time
   *   [41..43] fog colour          [44] fog density
   *
   * data (see wgpuscenedata_t in wgpu_nitro.h) is fourteen ints:
   *   [0]  world index arena     [1]  index count
   *   [2]  world batches         [3]  batch count
   *   [4]  opaque world batches  (the first N of [2] are opaque)
   *   [5]  entity blocks         [6]  entity count
   *   [7]  model vertex arena    [8]  model vertex count
   *   [9]  model batches         [10] model batch count
   *   [11] opaque model batches  (the first N of [9] are opaque)
   *   [12] particles             [13] particle count
   *
   * World batches are int quads of { textureId, firstIndex, indexCount,
   * entity }, where entity indexes the block arena; block 0 is the world
   * itself.  Model batches are int quads of { textureId, firstVertex,
   * vertexCount, flags }.
   */
  Nitro_DrawScene__deps: ['$Nitro'],
  Nitro_DrawScene: function (paramsPointer, dataPointer) {
    const state = Nitro.checkDevice();
    if (!state?.encoder) return;

    const floats = HEAPF32.subarray(paramsPointer >> 2, (paramsPointer >> 2) + 45);
    const integers = HEAP32.subarray(paramsPointer >> 2, (paramsPointer >> 2) + 29);
    const data = HEAP32.subarray(dataPointer >> 2, (dataPointer >> 2) + 14);
    const indexPointer = data[0], indexCount = data[1];
    const batchPointer = data[2], batchCount = data[3];
    const opaqueBatches = data[4];
    const entityPointer = data[5], entityCount = data[6];
    const modelVertexPointer = data[7], modelVertexCount = data[8];
    const modelBatchPointer = data[9], modelBatchCount = data[10];
    const opaqueModelBatches = data[11];
    const particlePointer = data[12], particleCount = data[13];

    const sceneWidth = Math.max(integers[22], 1);
    const sceneHeight = Math.max(integers[23], 1);

    Nitro.ensureScene(sceneWidth, sceneHeight);
    state.scanout = {
      x: integers[24], y: integers[25],
      width: Math.max(integers[26], 1), height: Math.max(integers[27], 1),
    };

    const frame = new Float32Array(28);
    frame.set(floats.subarray(0, 16));
    frame[16] = (integers[28] & 1) ? 1.0 : 0.0;
    frame[17] = (integers[28] & 2) ? 1.0 : 0.0;
    frame.set(floats.subarray(37, 41), 20);
    frame.set(floats.subarray(41, 45), 24);
    state.device.queue.writeBuffer(state.frameUniform, 0, frame);

    const scan = new Float32Array(8);
    scan[0] = floats[20];
    scan[1] = floats[21];
    scan.set(floats.subarray(16, 20), 4);
    state.device.queue.writeBuffer(state.scanoutUniform, 0, scan);

    /* Every entity's transform, alpha and flat light level in one upload;
     * the draws below address a block by dynamic offset. */
    let entityBound = false;
    if (entityPointer && entityCount > 0) {
      const bytes = entityCount * Nitro.ENTITY_STRIDE;
      const buffer = Nitro.ensureEntityBuffer(bytes);
      state.device.queue.writeBuffer(buffer, 0, HEAPU8, entityPointer, bytes);
      entityBound = true;
    }

    /* Alias and sprite geometry is rebuilt every frame -- animation is
     * whole-frame, so there is nothing to keep -- and the arena grows to
     * whatever the busiest frame so far needed. */
    let modelBuffer = null;
    if (modelVertexPointer && modelVertexCount > 0) {
      const bytes = modelVertexCount * Nitro.MODEL_STRIDE;
      modelBuffer = Nitro.ensureUploadBuffer('modelVertexBuffer', bytes,
        GPUBufferUsage.VERTEX);
      state.device.queue.writeBuffer(modelBuffer, 0, HEAPU8,
        modelVertexPointer, bytes);
    }

    /* A brush entity re-walks a submodel that the world already listed, so
     * a frame can need more indices than the map's own worst case. */
    let indexBuffer = state.worldIndexBuffer;
    if (state.world && indexPointer && indexCount > 0) {
      indexBuffer = Nitro.ensureUploadBuffer('worldIndexBuffer', indexCount * 4,
        GPUBufferUsage.INDEX);
      state.device.queue.writeBuffer(indexBuffer, 0, HEAPU8, indexPointer,
        indexCount * 4);
    }

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
        stencilClearValue: 0,
        stencilLoadOp: 'clear',
        stencilStoreOp: 'discard',
      },
    });
    pass.setStencilReference(0);
    state.sceneReady = true;

    const worldReady = state.world && entityBound && indexBuffer &&
      indexCount > 0 && batchCount > 0;
    let boundPipeline = null;
    let boundTexture = -1;
    let boundEntity = -1;
    let boundVertices = null;

    /* World and brush-entity surfaces: same vertex buffer, same index
     * arena, one dynamic offset per entity. */
    const drawWorldBatches = (from, to, blended, alphaFilter = 'all') => {
      if (!worldReady || from >= to) return;
      const batches = HEAP32.subarray(batchPointer >> 2,
        (batchPointer >> 2) + batchCount * 4);
      if (boundVertices !== state.world.vertexBuffer) {
        pass.setVertexBuffer(0, state.world.vertexBuffer);
        pass.setIndexBuffer(indexBuffer, 'uint32');
        boundVertices = state.world.vertexBuffer;
      }
      for (let i = from; i < to; ++i) {
        const textureId = batches[i * 4];
        const entry = state.textures[textureId];
        const first = batches[i * 4 + 1];
        const count = batches[i * 4 + 2];
        const entity = batches[i * 4 + 3];
        if (!entry || count <= 0 || entity < 0 || entity >= entityCount) continue;
        const alpha = HEAPF32[(entityPointer + entity * Nitro.ENTITY_STRIDE + 64) >> 2];
        if ((alphaFilter === 'opaque' && alpha < 0.999) ||
            (alphaFilter === 'blend' && alpha >= 0.999)) continue;
        const pipeline = entry.sky ? state.skyPipeline
          : (blended || alpha < 0.999) ? state.worldBlendPipeline : state.worldPipeline;
        if (boundPipeline !== pipeline) {
          pass.setPipeline(pipeline);
          pass.setBindGroup(0, state.frameBindGroup);
          boundPipeline = pipeline;
          boundTexture = -1;
          boundEntity = -1;
        }
        if (textureId !== boundTexture) {
          pass.setBindGroup(1, entry.bindGroup);
          boundTexture = textureId;
        }
        if (entity !== boundEntity) {
          pass.setBindGroup(2, state.entityBindGroup,
            [entity * Nitro.ENTITY_STRIDE]);
          boundEntity = entity;
        }
        pass.drawIndexed(count, 1, first, 0, 0);
      }
    };

    /* Alias models and sprites: unindexed triangles whose transform is
     * already baked in, so the batch's own flags are all that changes. */
    const drawModelBatches = (from, to, blendFilter = 'all') => {
      if (!modelBuffer || from >= to) return;
      const batches = HEAP32.subarray(modelBatchPointer >> 2,
        (modelBatchPointer >> 2) + modelBatchCount * 4);
      let viewmodel = false;
      for (let i = from; i < to; ++i) {
        const textureId = batches[i * 4];
        const entry = state.textures[textureId];
        const first = batches[i * 4 + 1];
        const count = batches[i * 4 + 2];
        const flags = batches[i * 4 + 3];
        const blends = (flags & (Nitro.MODEL_BLEND_ALPHA | Nitro.MODEL_BLEND_ADD |
          Nitro.MODEL_GLOW | Nitro.MODEL_SHADOW)) !== 0;
        if (!entry || count <= 0) continue;
        if ((blendFilter === 'opaque' && blends) ||
            (blendFilter === 'blend' && !blends)) continue;
        const pipeline = (flags & Nitro.MODEL_GLOW)
          ? state.modelGlowPipeline
          : (flags & Nitro.MODEL_SHADOW) ? state.modelShadowPipeline
          : (flags & Nitro.MODEL_BLEND_ADD)
          ? state.modelAddPipeline
          : (flags & Nitro.MODEL_BLEND_ALPHA) ? state.modelAlphaPipeline
                                              : state.modelOpaquePipeline;
        /* WebGPU has no glDepthRange, so the view weapon gets its own
         * slice of the depth buffer through the viewport instead -- the
         * same trick, and the same reason: the gun must not poke into a
         * wall the player is standing against. */
        const wantViewmodel = (flags & Nitro.MODEL_VIEWMODEL) !== 0;
        if (wantViewmodel !== viewmodel) {
          pass.setViewport(0, 0, sceneWidth, sceneHeight, 0,
            wantViewmodel ? Nitro.VIEWMODEL_DEPTH : 1);
          viewmodel = wantViewmodel;
        }
        if (boundPipeline !== pipeline) {
          pass.setPipeline(pipeline);
          pass.setBindGroup(0, state.frameBindGroup);
          boundPipeline = pipeline;
          boundTexture = -1;
        }
        if (boundVertices !== modelBuffer) {
          pass.setVertexBuffer(0, modelBuffer);
          boundVertices = modelBuffer;
        }
        if (textureId !== boundTexture) {
          pass.setBindGroup(1, entry.bindGroup);
          boundTexture = textureId;
        }
        pass.draw(count, 1, first, 0);
      }
      if (viewmodel) pass.setViewport(0, 0, sceneWidth, sceneHeight, 0, 1);
    };

    /* Opaque first, both kinds, then everything that blends -- the order
     * the software renderer's own edge list produces. */
    drawWorldBatches(0, opaqueBatches, false, 'opaque');
    drawModelBatches(0, opaqueModelBatches, 'opaque');
    /* Turbulent world batches are gathered with the world for batching, but
     * alpha makes them translucent. Defer them until opaque models are down
     * so an entity behind water cannot draw over the water surface. */
    drawWorldBatches(0, opaqueBatches, true, 'blend');
    drawWorldBatches(opaqueBatches, batchCount, true);
    drawModelBatches(0, opaqueModelBatches, 'blend');
    drawModelBatches(opaqueModelBatches, modelBatchCount);

    if (particlePointer && particleCount > 0) {
      const bytes = particleCount * Nitro.PARTICLE_STRIDE;
      const buffer = Nitro.ensureUploadBuffer('particleBuffer', bytes,
        GPUBufferUsage.VERTEX);
      state.device.queue.writeBuffer(buffer, 0, HEAPU8, particlePointer, bytes);
      state.device.queue.writeBuffer(state.particleUniform, 0,
        floats.subarray(29, 37));
      pass.setPipeline(state.particlePipeline);
      pass.setBindGroup(0, state.frameBindGroup);
      pass.setBindGroup(1, state.particleBindGroup);
      pass.setVertexBuffer(0, buffer);
      pass.draw(6, particleCount);
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
