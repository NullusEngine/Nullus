# Research: UE4.27 Deferred Lighting Alignment

## Decision: Phase one uses fullscreen light-list accumulation

**Rationale**: Current Nullus deferred lighting already renders a fullscreen quad over the GBuffer. The failure evidence points to that pass depending on clustered per-pixel culling, which can leave valid scene lights absent and the result at the ambient floor. A fullscreen loop over the packed scene light list restores visible lighting with the smallest blast radius.

**Alternatives considered**: Add immediate per-light draw volumes; port UE tiled deferred thresholding; fix only cluster injection. Per-light/tiled paths are larger and require more RHI state work. Fixing only injection keeps deferred correctness dependent on the clustered optimization path.

## Decision: Ambient Sphere is global for deferred phase one

**Rationale**: Nullus default scenes create an Ambient Light component as `AMBIENT_SPHERE`. UE-style deferred ambient/indirect contribution is not dependent on a local cluster volume. Treating Ambient Box and Ambient Sphere as global contributors in the deferred light-list loop matches user expectations and fixes the observed default scene darkness.

**Alternatives considered**: Keep Ambient Sphere clustered/local; convert default scenes to Ambient Box. Both hide the renderer issue and leave existing Ambient Sphere scenes dim.

## Decision: Keep clustered light grid bound but not required for deferred visibility

**Rationale**: Forward shaders and existing tests rely on `ForwardLightData`, `u_ForwardLocalLightBuffer`, `u_NumCulledLightsGrid`, and `u_CulledLightDataGrid`. Deferred phase one can reuse the same pass binding set while choosing a different accumulation function.

**Alternatives considered**: Add a separate deferred light buffer and binding layout immediately. That is cleaner long-term but creates broader C++ binding and shader metadata churn before visible lighting is restored.

## Decision: RenderDoc labels stay stable and descriptive

**Rationale**: The user explicitly needs capture stages to be distinguishable. Existing labels `DeferredGBuffer`, `DeferredLighting`, and light-grid compute names are retained; future substage labels can be added without renaming the main events.

**Alternatives considered**: Rename all passes to UE names. That would break existing tests and make the engine-specific capture less stable.
