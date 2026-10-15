# Rendering Overview

Nullus renders in three layers: the Game Thread produces commands, the Render Thread consumes them and builds the frame, and the RHI lane translates commands into native command buffers and submits them. Those boundaries are fixed, and rendering features must land in the right layer.

## The path of a frame

```text
Game Thread
  SceneRenderStore incremental updates + RenderFramePacket (camera, view, output targets, frame parameters)
        |
        v
Render Thread
  FrameGraph decides pass order and resource transitions
  visibility collection -> draw commands -> pipeline state -> command recording
        |
        v
RHI lane (Dispatch / Translate / Submit)
  native command buffers -> queue submission -> semaphores -> Present
```

The Game Thread no longer copies the whole scene every frame. `SceneRenderStore` is persistent data owned by the Render Thread and updated incrementally through commands; only a lightweight payload crosses per frame. That removes the per-frame snapshot cost in large scenes.

## FrameGraph

The FrameGraph describes each pass's inputs and outputs together with resource states, and derives execution order and barriers. Rendering code declares dependencies while the framework handles:

- When a resource is created, used, and released
- Texture and buffer state transitions
- Aliasing and reuse of transient resources
- Dependency edges between passes

This model lets the forward and deferred paths share resource management, and keeps new passes from hand-writing barriers.

## Render paths

| Path | Content |
| --- | --- |
| Forward | Depth prepass, forward shading, transparent objects, skybox, editor overlays |
| Deferred | GBuffer, clustered lighting, decals, deferred lighting, transparency, editor output |

Both paths are built through the FrameGraph. The editor and the game share the same pass implementations and only differ in output targets.

## Scene features

- **Clustered lighting** buckets lights into frustum clusters instead of iterating every light per pixel.
- **Decals** project after the GBuffer stage in the deferred path.
- **Skybox** is its own pass, drawn before transparent objects.
- **Transparent objects** sort back to front and disable depth writes.
- **Editor view output** gives the Scene View and Game View their own targets, so focusing one never rebuilds the whole pipeline.

## Large-scene optimizations

- Visibility collection driven by the spatial index instead of a per-object linear scan
- Draw command caching and instancing batching
- Static mesh LOD construction and runtime selection
- HLOD proxies and residency budgets
- HZB occlusion on DX12, conservatively gated by device capability

Details are in [Graphics Backends and Large-Scene Features](backends.md).

## Shader workflow

HLSL is the only maintained shader source. DXIL, SPIR-V, and reflection data are build artifacts, and maintaining one HLSL source alongside a hand-written GLSL copy is not allowed.

The engine ships a shader library plus a set of ShaderLab shaders: `StandardPBR`, `Transparent`, `UnlitColor`, `UnlitTexture`, `AlphaTest`, and `ForwardDepthOnly`, along with the deferred-path shaders `DeferredGBuffer`, `DeferredLighting`, `DeferredDecal`, `Skybox`, `HZBBuild`, and `HZBOcclusion`.

Binding spaces are fixed and do not vary by backend:

| Space | Content |
| --- | --- |
| `set0` | Frame / Scene |
| `set1` | Material |
| `set2` | Object |
| `set3` | Pass / Rare |

Material data belongs to the `Material` space. Pass constants and pass textures are renderer-controlled and do not live on material instances.

## Render thread modes

The project file's `rhi_thread_mode` accepts three values:

| Mode | Notes |
| --- | --- |
| `none` | No dedicated RHI thread; useful for comparison and debugging |
| `dedicated_thread` | RHI work runs on a fixed thread |
| `tasks` | The RHI lane is an execution context on TaskGraph workers |

DX12 and Vulkan default to `tasks`, while Metal stays on `dedicated_thread` until thread ownership and concurrent submission are validated. Backends declare support through a capability bit; forcing an unsupported combination fails startup with an explicit reason instead of silently falling back.

## Frame resources and fences

- A `FrameResourceRing` slot only answers "is the GPU still using this batch"; reuse timing is decided by GPU fences.
- CPU frame latency is governed together by `FrameLatencyController` and the Render / Rhi / Swapchain sync depths of `RenderCommandFence`.
- Resource release goes through a deferred queue so nothing is reclaimed while the GPU may still reference it.

## Next steps

- [Cameras, Lights, and Output](cameras-lights.md) renders the scene.
- [Materials and ShaderLab](materials.md) authors shaders and materials.
- [Graphics Backends and Large-Scene Features](backends.md) covers the backend matrix and large-scene details.
