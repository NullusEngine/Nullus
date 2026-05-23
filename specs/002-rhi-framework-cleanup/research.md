# Research: RHI Framework Migration

**Date**: 2026-04-03
**Feature**: RHI Framework Migration

---

## Decision 1: OpenGL Backend Formal RHI Coverage

**Decision**: OpenGL backend has **critical gaps** that would block a full Formal RHI migration.

### Findings

OpenGL backend (`OpenGLExplicitDeviceFactory.cpp`) uses `CreateCompatibilityExplicitDevice()` - no native Formal RHI implementation.

**Critical Gaps (Formal RHI NOT supported by OpenGL)**:

| Feature | OpenGL Status | Impact |
|---------|---------------|--------|
| Render Passes | Not supported | CompatibilityCommandBuffer simulates poorly |
| Pipeline Barriers | Empty stub (no sync) | Data races, visual artifacts |
| Compute Pipeline | Stub | Compute workloads fail |
| Fence/Semaphore | Stub | Multi-frame sync broken |
| Descriptor Set Model | Limited translation | Flexibility lost |

**What DOES work via Legacy IRenderDevice**:
- Texture/Buffer creation and binding
- Framebuffer operations
- Viewport, clear, draw operations
- Swapchain presentation

### Resolution

**Recommendation**: OpenGL backend should keep using compatibility layer internally but expose Formal RHI API. This contradicts the spec's "Option A" but is the only viable path.

**Rationale**: The spec's "Option A" (OpenGL fully migrated to Formal RHI) is not achievable without rewriting OpenGL backend from scratch. The practical approach is:
- OpenGL uses compatibility internally (Tier B)
- External API is unified Formal RHI
- DX12/Vulkan are Tier A with full Formal RHI

**Alternatives considered**:
- Option A (full OpenGL migration) - Not feasible, requires complete OpenGL backend rewrite
- Option B (keep compatibility) - Acceptable internally but external API should still be Formal RHI

---

## Decision 2: Editor Legacy IRenderDevice Usage

**Decision**: Editor panels do NOT directly call IRenderDevice. Migration is through UI framework chain.

### Findings

**Call Chain (every texture displayed in UI)**:
```
Editor Panel (Image/ButtonImage)
  -> RHITextureView (shared_ptr)
  -> UIManager::ResolveTextureView()
  -> RHIUIBridge::ResolveTextureView()
  -> ExtractCompatibilityLegacyTextureResource()
  -> IRenderDevice::GetUITextureHandle()
```

**UI components using Legacy RHI (ranked by frequency)**:

| Component | Legacy Call | Frequency |
|-----------|-------------|-----------|
| SceneView, GameView, AssetView (AView-derived) | GetUITextureHandle | Per frame, per FBO texture |
| Toolbar (5 buttons) | GetUITextureHandle | Per frame |
| AssetBrowser TexturePreview | GetUITextureHandle | Per hovered texture |
| MaterialEditor | GetUITextureHandle | Per sampler uniform |

**Key Insight**: The FBO color view is already a `RHITextureView` object. The problem is `ExtractCompatibilityLegacyTextureResource()` extracting the legacy texture from it.

### Resolution

**Recommendation**: Migrate by making `RHITextureView` self-sufficient - no extraction of underlying legacy texture needed.

**Migration Order**:
1. RHIUIBridge (User Story 2)
2. UIManager::ResolveTextureView
3. Image/ButtonImage widgets
4. Toolbar
5. AView-derived panels
6. Remove compatibility layer

---

## Decision 3: Compatibility Layer Call Graph

**Decision**: Compatibility layer is deeply embedded. DX12/Vulkan use explicit path; DX11/OpenGL/Metal use compatibility.

### Findings

**Two Draw Paths in Driver::SubmitMaterialDraw()**:
```
DX12/Vulkan: Explicit path (RHIDevice + recorded command buffers)
DX11/OpenGL/Metal/None: Compatibility path (IRenderDevice directly)
```

**Compatibility Files and Dependencies**:
```
CompatibilityWrapperInternal.cpp    <- CompatibilityRHIInternal.cpp, CompatibilityMaterialInternal.cpp
CompatibilityBufferInternal.cpp    <- UniformBuffer, IndexBuffer, ShaderStorageBuffer, VertexBuffer
CompatibilityTextureInternal.cpp <- Texture, Texture2D, TextureCube, Framebuffer
CompatibilityFramebufferInternal.cpp <- Framebuffer, MultiFramebuffer
CompatibilityLegacyBindingInternal.cpp <- CompatibilityRHIInternal.cpp, Driver.cpp
CompatibilityLegacyDrawInternal.cpp <- CompatibilityRHIInternal.cpp, Driver.cpp
CompatibilityMaterialInternal.cpp <- CompatibilityRHIInternal.cpp, Driver.cpp
CompatibilityRHIInternal.cpp      <- ExplicitDeviceFactory.cpp (fallback), RHIUIBridge.cpp, Driver.cpp
CompatibilityDeviceInternal.cpp   <- ExplicitDeviceFactory.cpp (Metal/None fallback)
```

**No circular dependencies** - purely an adapter layer.

**Key Finding**: DX12 and Vulkan can have compatibility removed first; DX11/OpenGL/Metal still need it.

### Resolution

**Recommended Migration Order**:
```
Phase 1: Descriptor Unification
  1.1. TextureDesc/BufferDesc audit
  1.2. Create unified structures
  1.3. Update all references

Phase 2: DX12/Vulkan Native Path First
  2.1. Verify DX12 explicit path works standalone
  2.2. Verify Vulkan explicit path works standalone
  2.3. Remove compatibility from DX12/Vulkan path

Phase 3: UI Framework Migration
  3.1. RHIUIBridge Formal RHI path
  3.2. UIManager ResolveTextureView
  3.3. Image/ButtonImage widgets

Phase 4: Type-Safe Handles
  4.1. Replace void* returns

Phase 5: DX11/OpenGL Compatibility Removal (may be deferred)
  5.1. Migrate or retain compatibility internally
  5.2. Remove IRenderDevice.h
```

---

## Decision 4: Scope Adjustment

**Decision**: Full OpenGL migration (spec Option A) is not achievable. Adjustment needed.

### Original Spec Assumption
- OpenGL backend must fully migrate to Formal RHI
- All backends use Formal RHI exclusively

### Revised Reality
- OpenGL has fundamental gaps (no render pass, broken barriers, stub compute)
- DX12/Vulkan are Tier A (full Formal RHI)
- OpenGL is Tier B (compatibility internally, Formal RHI externally)

### Resolution
The spec's intent (unified API) is achieved. The implementation reality is:
- DX12/Vulkan: Native Formal RHI, no compatibility
- OpenGL: Internal compatibility, external Formal RHI API
- Metal/DX11: Separate spec (out of scope)

This is consistent with the existing architecture where OpenGL is already marked as Tier B.

---

## Summary of Resolved Unknowns

| Unknown | Resolution |
|---------|------------|
| OpenGL Formal RHI coverage gaps | Critical gaps exist; OpenGL keeps internal compatibility but exposes Formal API |
| Editor Legacy heavy usage | Editor doesn't call Legacy directly; UI framework chain is the migration path |
| Compatibility layer call graph | DX12/Vulkan explicit path first; compatibility removal phased |

---

## Open Questions Still Outstanding

None - all major unknowns have been resolved through research.
