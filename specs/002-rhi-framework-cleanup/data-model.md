# Data Model: RHI Framework Migration

**Date**: 2026-04-03
**Feature**: RHI Framework Migration

## Overview

This is an API migration (not a data modeling task). The entities below represent the key interfaces being modified.

## Key Entities

### 1. TextureDesc (Unified - replaces duplicate)

**Location after migration**: `Runtime/Rendering/RHI/RHITypes.h`

**Purpose**: Descriptor for texture creation

**Fields**:
| Field | Type | Description |
|-------|------|-------------|
| width | uint32_t | Texture width in pixels |
| height | uint32_t | Texture height in pixels |
| depth | uint32_t | Texture depth (for 3D) |
| format | TextureFormat | Pixel format (single enum) |
| dimension | TextureDimension | 1D, 2D, 3D, Cube |
| mipLevels | uint32_t | Number of mipmap levels |
| samples | uint32_t | MSAA sample count |
| usage | TextureUsage | Usage flags (render target, shader resource, etc.) |
| initialState | ResourceState | Initial resource state |
| debugName | string | Debug label |

**Constraints**:
- Width/height must be > 0
- Format must be a valid TextureFormat enum value
- MipLevels must be >= 1

### 2. BufferDesc (Unified - replaces duplicate)

**Location after migration**: `Runtime/Rendering/RHI/RHITypes.h`

**Purpose**: Descriptor for buffer creation

**Fields**:
| Field | Type | Description |
|-------|------|-------------|
| size | size_t | Buffer size in bytes |
| type | BufferType | Vertex, Index, Uniform, Storage |
| usage | BufferUsage | Usage flags (dynamic, etc.) |
| memoryUsage | MemoryUsage | GPU memory location |
| initialData | void* | Optional initial data pointer |
| debugName | string | Debug label |

### 3. RHIDevice (Formal RHI - primary device interface)

**Location**: `Runtime/Rendering/RHI/Core/RHIDevice.h`

**Purpose**: Platform-agnostic device abstraction

**Key Methods**:
| Method | Returns | Description |
|--------|---------|-------------|
| CreateBuffer(const BufferDesc&) | shared_ptr<RHIBuffer> | Create buffer |
| CreateTexture(const TextureDesc&) | shared_ptr<RHITexture> | Create texture |
| CreateSwapchain(const SwapchainDesc&) | shared_ptr<RHISwapchain> | Create swapchain |
| GetQueue(QueueType) | RHIQueue& | Get graphics/compute/copy queue |
| CreateGraphicsPipeline(...) | shared_ptr<RHIGraphicsPipeline> | Create graphics pipeline |
| CreateBindingLayout(...) | shared_ptr<RHIBindingLayout> | Create binding layout |

### 4. RHITextureView (Formal RHI - texture view)

**Location**: `Runtime/Rendering/RHI/Core/RHITexture.h`

**Purpose**: A view into a texture (for sampling, render target, etc.)

**Key Methods**:
| Method | Returns | Description |
|--------|---------|-------------|
| GetTexture() | shared_ptr<RHITexture> | Get underlying texture |
| GetDesc() | TextureViewDesc | View configuration |
| GetNativeImageHandle() | TypedHandle | Native image handle (replaces void*) |

### 5. NativeHandle (Type-safe - replaces void*)

**Purpose**: Type-safe native resource handle

**Typed variants**:
| Backend | Handle Type |
|---------|-------------|
| DX12 | ID3D12Resource* |
| Vulkan | VkImage |
| OpenGL | GLuint |
| Metal | id<MTLTexture> |

**Constraints**:
- Each backend provides appropriately typed handles
- No raw void* casting in consumer code
- Type checking at compile time where possible

## Relationships

```
RHIDevice
  |-creates-> RHIBuffer
  |-creates-> RHITexture
  |-creates-> RHISwapchain
  |-creates-> RHIGraphicsPipeline
  |-creates-> RHIBindingLayout
  |-creates-> RHIBindingSet

RHITexture
  |-viewed_by-> RHITextureView
  |-has_native-> NativeHandle (typed)

RHIBuffer
  |-has_native-> NativeHandle (typed)
```

## State Transitions

### Resource State Model

Resources have a state tracked via `ResourceState` enum:
- Undefined
- RenderTarget
- ShaderResource
- CopySource
- CopyDest
- DepthStencil
- Present

Transitions are done via pipeline barriers.

## Removed Entities

The following will be removed from the codebase:

| Entity | Reason |
|--------|--------|
| IRenderDevice | Legacy interface, replaced by RHIDevice |
| CompatibilityWrapperInternal.* | Bridge layer, no longer needed |
| CompatibilityRHIInternal.* | Bridge layer, no longer needed |
| CompatibilityBufferInternal.* | Buffer creation via RHIDevice |
| CompatibilityTextureInternal.* | Texture creation via RHIDevice |
| CompatibilityFramebufferInternal.* | Framebuffer via RHITextureView |
| CompatibilityLegacyBindingInternal.* | Binding via RHIBindingSet |
| CompatibilityLegacyDrawInternal.* | Drawing via RHICommandBuffer |
| CompatibilityMaterialInternal.* | Material via RHIGraphicsPipeline |
| CompatibilityDeviceInternal.* | Device creation via RHIDevice |

## Constraints

1. All texture/buffer creation must go through RHIDevice
2. No direct IRenderDevice calls from Editor/Game code
3. UI rendering must use typed NativeHandle (not void*)
4. Resource state must be explicitly tracked
5. OpenGL backend may use internal compatibility (Tier B) but external API is Formal RHI
