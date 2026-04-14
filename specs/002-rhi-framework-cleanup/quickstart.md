# Quickstart: RHI Framework Migration

**Date**: 2026-04-03
**Feature**: RHI Framework Migration

## What This Migration Does

Removes Legacy `IRenderDevice` and compatibility wrappers from Nullus engine, migrating all rendering to use the Formal RHI abstraction.

## Before vs After

### Before
```
Editor/Game
  -> IRenderDevice (Legacy)
  -> Compatibility Layer
  -> Backend (DX12/Vulkan/OpenGL)

Two parallel code paths:
- Formal RHI (RHIDevice) for some operations
- Legacy RHI (IRenderDevice) for others
```

### After
```
Editor/Game
  -> RHIDevice (Formal RHI)
  -> Backend (DX12/Vulkan/OpenGL)

Single unified code path:
- All operations use Formal RHI
- Compatibility layer removed
```

## Affected Code Areas

| Area | Changes |
|------|---------|
| `Runtime/Rendering/RHI/` | Remove Compatibility*.*, IRenderDevice.h |
| `Runtime/Rendering/Buffers/` | Update to use RHIDevice directly |
| `Runtime/Rendering/Resources/` | Update Texture, Material to use Formal RHI |
| `Runtime/UI/` | Update UIManager, Image widgets |
| `Project/Editor/` | Update all panels to use Formal RHI |
| `Project/Game/` | Update game rendering to use Formal RHI |
| `Runtime/Rendering/Context/` | Update Driver factory methods |

## Migration Phases

### Phase 1: Descriptor Unification
1. Audit TextureDesc/BufferDesc in RHITypes.h and RHIEnums.h
2. Create unified structures
3. Update all references

### Phase 2: UI Framework Migration
1. Migrate RHIUIBridge to Formal RHI
2. Update UIManager::ResolveTextureView
3. Update Image/ButtonImage widgets
4. Test all backends (DX12, Vulkan, OpenGL)

### Phase 3: Type-Safe Handles
1. Replace void* returns with typed NativeHandle
2. Update all consumers

### Phase 4: Remove Compatibility Layer
1. Remove Compatibility*.cpp/h files
2. Remove IRenderDevice.h
3. Update all Editor/Game code

## Validation Commands

### Build
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

### Run Tests
```powershell
ctest --test-dir build -C Debug --output-on-failure
```

### Manual UI Test
1. Launch Editor.exe with DX12 backend: `Editor.exe --backend dx12 MyProject.nullus`
2. Launch Editor.exe with Vulkan backend: `Editor.exe --backend vulkan MyProject.nullus`
3. Launch Editor.exe with OpenGL backend: `Editor.exe --backend opengl MyProject.nullus`
4. Verify Scene View, Game View, Asset Browser all render correctly

### RenderDoc Verification
```powershell
set NLS_RENDERDOC_CAPTURE=1
set NLS_RENDERDOC_CAPTURE_AFTER_FRAMES=60
Editor.exe
# Check captures for correct render pass structure
```

## Common Issues

| Issue | Cause | Fix |
|-------|-------|-----|
| Link errors for IRenderDevice symbols | Code still references old interface | Update to RHIDevice |
| void* handle returns | Not yet migrated | Use typed NativeHandle |
| OpenGL rendering broken | Compatibility layer removed | OpenGL uses internal compatibility (Tier B) |
| UI textures not displaying | Texture view not resolved | Check ResolveTextureView path |

## Backends Status After Migration

| Backend | Formal RHI Support | Notes |
|---------|-------------------|-------|
| DX12 | Full (Tier A) | Native implementation |
| Vulkan | Full (Tier A) | Native implementation |
| OpenGL | Tier B | Internal compatibility, Formal API externally |

Metal and DX11 are out of scope (separate spec).
