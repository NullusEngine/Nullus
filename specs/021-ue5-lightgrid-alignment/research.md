# Research: UE5 LightGrid Alignment

## Decision: Treat UE5 Forward Local Light Grid As The Alignment Target

**Decision**: Align Nullus LightGrid to UE5's forward renderer local light grid model first: lights are culled into a frustum-space grid and scene lighting iterates only the lights affecting the current cell.

**Rationale**: Epic's Forward Shading Renderer documentation describes the forward renderer as culling lights and reflection captures to a frustum-space grid, then having each pixel iterate the affecting entries in the forward pass. This is the closest published UE5 contract to Nullus's current LightGrid path.

**Sources**:
- Epic Forward Shading Renderer documentation: https://dev.epicgames.com/documentation/ru-ru/unreal-engine/forward-shading-renderer-in-unreal-engine
- Unreal Art Optimization pass notes for `ComputeLightGrid`: https://unrealartoptimization.github.io/book/profiling/passes/

**Alternatives considered**:
- Align to deferred tiled lighting only: rejected because the user specifically called the existing LightGrid path, and UE's named `ComputeLightGrid` is primarily described around forward light culling.
- Preserve the current fixed-grid Nullus model: rejected because it does not match UE's pixel-size-driven grid settings.

## Decision: Use Local UE 4.27 Source As File-Level Reference

**Decision**: Treat `F:\Epic Games\UE_4.27\Engine` as the licensed local reference for LightGrid internals. The key files are:
- `Source/Runtime/Renderer/Private/LightGridInjection.cpp`
- `Shaders/Private/LightGridInjection.usf`
- `Shaders/Private/LightGridCommon.ush`
- `Shaders/Private/ForwardLightingCommon.ush`
- `Source/Runtime/Renderer/Private/SceneRendering.h`

**Rationale**: The user supplied a local Engine source path. UE 4.27 contains the named LightGrid compute path and exposes exact constants, shader flow, buffer names, and Z-slice formula needed for Nullus alignment.

**Findings**:
- Defaults are `GLightGridPixelSize = 64`, `GLightGridSizeZ = 32`, `GMaxCulledLightsPerCell = 32`, and `GLightLinkedListCulling = 1`.
- `LightGridInjectionGroupSize = 4`, and injection/compaction use 3D threadgroups over `CulledGridSize`.
- `NumCulledLightsGridStride = 2` and `NumCulledGridPrimitiveTypes = 2`, reserving grid storage for lights and reflection captures.
- Injection owns one grid cell per compute thread, computes the cell view-space AABB, then loops local lights and tests sphere/AABB plus refined spot cone/AABB.
- The linked-list path writes reverse links into `StartOffsetGrid`/`CulledLightLinks`, then `LightGridCompactCS` compacts records into `NumCulledLightsGrid` and `CulledLightDataGrid`.
- Z slicing is logarithmic via `LightGridZParams`, not linear near/far interpolation.

**Alternatives considered**:
- Continue relying only on public docs: rejected because a licensed local reference is now available.
- Treat UE 4.27 as irrelevant because the user said UE5: rejected because the supplied source is the only local licensed source and contains the named LightGrid implementation.

## Decision: Adopt UE5 Console Variable Defaults For Public Contract Parity

**Decision**: Default Nullus LightGrid settings to the supplied UE reference values:
- light grid pixel size: 64
- Z slice count: 32
- max culled lights per cell: 32
- linked-list culling: enabled by default as the target behavior, with fixed-capacity fallback retained during staged implementation

**Rationale**: Epic's console variable reference lists these defaults and describes their semantics. These values provide an objective, testable parity baseline without requiring access to proprietary source text.

**Sources**:
- Epic Console Variables Reference: https://dev.epicgames.com/documentation/es-es/unreal-engine/unreal-engine-console-variables-reference%3Fapplication_version%3D5.6
- Unreal Engine 4.13 release notes documenting the original forward renderer light-grid controls and reverse linked-list culling: https://www.unrealengine.com/fr/blog/unreal-engine-4-13-released

**Alternatives considered**:
- Keep Nullus defaults of 16x9x24 and 128 lights per cluster: rejected because these values differ materially from UE defaults.
- Make all settings editor-only: rejected because the renderer needs runtime settings before editor UI is available.

## Decision: Stage Linked-List Culling Behind A Testable Flag

**Decision**: Add the UE-style linked-list culling setting as part of the public LightGrid settings contract, but implement it in a controlled phase after fixed-capacity parity tests lock the new defaults and grid sizing.

**Rationale**: UE's linked-list path changes buffer shape and overflow semantics. Implementing it without first locking grid defaults would mix two high-risk changes and make regressions harder to isolate.

**Alternatives considered**:
- Implement linked-list culling immediately as the only path: rejected because it risks breaking existing shaders and bindings in one step.
- Ignore linked-list culling: rejected because UE defaults to it and the user asked for full UE5 alignment.

## Decision: Replace Nullus Light-Oriented Injection With UE Cell-Oriented Injection

**Decision**: Rewrite Nullus `LightGridInjection.hlsl` around UE's cell-owned compute model: dispatch over `gridSizeX x gridSizeY x gridSizeZ`, compute a view-space cell AABB, loop lights inside the shader, and write culled references for the current cell.

**Rationale**: Nullus currently dispatches one thread per light and iterates that light's approximate screen/depth range. UE dispatches one thread per grid cell and tests all lights against the cell AABB. This affects dispatch dimensions, culling accuracy, linked-list storage, and overflow behavior.

**Alternatives considered**:
- Keep Nullus light-owned injection and only adjust constants: rejected because it would not match the supplied UE implementation flow.
- Build the grid entirely on CPU: rejected because UE's reference path is GPU compute culling.
