# Data Model: UE5 LightGrid Alignment

## UE5AlignedLightGridSettings

Represents runtime LightGrid culling configuration.

- `cellPixelSize`: default 64; minimum 1.
- `gridSizeZ`: default 32; minimum 1.
- `maxCulledLightsPerCell`: default 32; minimum 1.
- `linkedListCullingEnabled`: default true; fixed-capacity fallback remains available.
- `maxLightCullingDistanceKilometers`: optional override; disabled when <= 0.

## LightGridFrameConstants

Per-frame constants shared by compute and graphics passes.

- View matrix.
- Projection matrix.
- Inverse view-projection matrix.
- Camera world position and near plane.
- Render width, render height, inverse render width, far plane.
- Grid size X, grid size Y, grid size Z, max culled lights per cell.
- Scene light count, ambient/degraded-lighting parameters, skybox flag, linked-list mode flag.

## LightGridPackedLight

Packed GPU representation of one runtime light.

- World position and influence range.
- World direction and light type.
- Color and intensity.
- Attenuation coefficients and spot outer cutoff.

## LightGridCellRecord

Graphics-readable record for one frustum-space grid cell.

- `offset`: offset into compact light index buffer, or linked-list start offset after compaction.
- `count`: number of lights affecting the cell after clamp/compaction.

## LightGridCullingBuffers

GPU buffers used by culling and lighting.

- Packed lights.
- Per-cell count or head buffer.
- Fixed-capacity scratch indices or linked-list node buffer.
- Compact counter.
- Compact cell records.
- Compact light indices.

## State Transitions

1. `Unprepared`: no valid per-frame LightGrid data.
2. `PreparedFrameData`: CPU-side constants and initial buffers are created.
3. `CullingDispatched`: compute pass assigns lights to grid cells.
4. `Compacted`: compact records and graphics-readable indices are ready.
5. `BoundForGraphics`: forward/deferred lighting shaders consume the same graphics binding set.
6. `Disabled`: no LightGrid compute or graphics binding is produced; renderers use the no-LightGrid fallback context.
