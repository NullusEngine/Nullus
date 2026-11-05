# Distance Fields

The first distance-field implementation supports static triangle meshes, rigid transforms, Mesh Distance Fields, Global Distance Fields, and Scene View diagnostics on Windows DX12. Particle collision, distance-field shadows/AO, skeletal deformation, Morph Targets, material vertex displacement, and terrain are not integrated.

## Enable and import

Generation is disabled by default. Enable **Generate Mesh Distance Fields** in Scene View's **Draw Mode > Distance Field Settings** menu, then click **Build Missing / Outdated**, or set this project `.nullus` option:

```ini
generate_mesh_distance_fields=true
```

Source-adjacent `.meta` settings control model import:

| Setting | Default | Meaning |
| --- | --- | --- |
| `DISTANCE_FIELD_GENERATION` | `inherit` | Inherit the project, force `enabled`, or force `disabled` |
| `DISTANCE_FIELD_RESOLUTION_SCALE` | `1` | Resolution scale from `0.125` to `4` |
| `DISTANCE_FIELD_TWO_SIDED` | `false` | Surface distance for open thin geometry without a negative interior |

Distance field settings Apply independently and queue background work. **Build Missing / Outdated** reads published LOD0 geometry without reimporting materials, textures, rendering LODs or Prefabs. Legacy assets migrate their build metadata once. The build uses the last published geometry and preserves the state of unapplied model import settings. Enabling the project option or changing the display mode does not generate missing fields; the viewport displays a message when data is missing.

Editor and command-line Cook share the CPU build service. Opaque and Masked triangles participate fully; Transparent triangles are excluded. Open/nonmanifold geometry reports diagnostics. Failed, cancelled or superseded work preserves valid published artifacts.

`MeshDistanceFieldSource` records geometry digests, logical groups and participation rules. Logical coarse fields become usable before detail work; single-section joint fields share their computation. Interactive import commits geometry and its mandatory mip together, then queues detail completion. `Library/DistanceFieldCache` stores each mip by builder version, geometry, effective settings and backend. A bounded process cache reuses acceleration structures; unchanged builds perform no distance queries.

The offline kernel uses an available Embree 4 SDK, otherwise its built-in SAH BVH. Configure `NLS_EMBREE_ROOT` with `include`, `lib`, and `bin`; the standard Windows SDK also needs oneTBB's `tbb12.dll` and `tbb12.lib`. Runtime manifests deploy build-tool dependencies and licenses. Set `NLS_EMBREE_NEEDS_TBB=OFF` for an internal-tasking SDK. Persistent workers, bounded point queries, exact sign early termination and shared brick-boundary samples preserve the distance quality contract. All three mips remain independently computed.

MeshRenderer's **Affect Distance Field** controls participation. **Mostly Static Distance Field** selects the static cache; movement, mesh changes, and disabling still invalidate pages covering old bounds.

## Storage and runtime

Model fields use software indirection, physical `8³` bricks, and seven valid intervals per axis. Three mips are built separately with R8 UNORM and per-mip decode ranges; positive and negative saturated regions have distinct constant markers. The main artifact contains metadata and the lowest mip, while two higher mips load independently. Publication commits mesh and field payloads together, and runtime packages include those dependencies.

Material sections of one logical mesh share field generation. Complete, identically transformed section instances can share the union field; individually moving, disabling, or deleting sections makes remaining instances use their own fields.

Devices share a model atlas and a global physical-page pool, each budgeted at 128 MiB. Model upload is limited to 8 MiB per frame using DX12-aligned byte accounting. Lowest mips take priority; budget pressure degrades residency or reports allocation failure. Each view owns its page table and clipmaps, separating Scene, Game, and previews.

The global field uses four clipmaps with radii 16, 64, 256, and 1024 scene units, each with 20³ pages and 140³ valid intervals. It prefers R16F when the required UAV operations exist, otherwise uses R32F with fewer pages. Paging, binning, allocation, and composition execute on the graphics queue. Stationary views without resource changes do not recompose clean pages; invalid fine pages may fall back to valid coarser levels.

Nonuniform scale and mirroring use a conservative distance bound based on the minimum singular value. Singular/nonfinite transforms are excluded. Vulkan and Metal currently report distance fields as unsupported.

## Debugging and sampling

The **Draw Mode** menu exposes Mesh/Global modes. **Distance Field Settings** offers Surface, Trace Steps, Signed Slice, Normals, Clipmaps, Pages, and Residency views. Trace Steps keeps misses dark and maps hit counts continuously from blue to red on a logarithmic scale. Surface-normal lighting preserves the shape; hue represents cost and brightness represents surface lighting. The viewport shows a legend. **Red at steps** adjusts the upper display range from 8 to 512 steps (64 by default). Signed Slice uses world-space Y. Mesh Residency colors identify the sampled mip; Global Residency distinguishes fine pages, coarse fallback, and invalid pages. Counters report objects, bricks, pages, uploads, failed allocations, CPU preparation, and the latest completed GPU update. **Rebuild Global Pages** invalidates pages and retries failed loads; it does not generate model fields.

**Build Coarse Preview** builds only the mandatory mip. **Force Rebuild Model Fields** recomputes requested fields. Background Tasks provides progress and cancellation; the menu reports the backend, query timings and built/cached/current mip counts. Incremental publication batches completed results and preserves unrelated artifact entries.

Mesh DF visualization clips rays to the scene volumes and uses conservative distance lower bounds to skip models that cannot supply the closest distance. Once loading, uploads and page updates settle, unchanged views reuse the rendered image. Camera, scene, debug-setting or field-resource revisions trigger rendering again. **View image** reports `cached` or `updating`; GPU timing refers to the latest actual update.

`DistanceFieldGpuResources` exposes GPU resources. `DistanceFieldSampling.hlsli` returns world-space distance, gradient, voxel scale, and validity; consumers must check validity.

## Validation

With the same 112 Sponza field objects at 1280×720 on an RTX 5070 Laptop GPU with the DX12 debug layer, the mean GPU time over 20 settled frames fell from 33.3 ms to 2.56 ms for mesh-field visualization. These timings cover distance-field GPU passes, not the full Editor frame. Stationary image reuse separately avoids repeated rendering. Surface silhouette intersection-over-union was 99.94% before versus after optimization.

A local Sponza measurement on 2026-11-04 (Ryzen 9 8945HX, Release, Embree 4, 430 unique geometries and 1290 mips) completed a full forced rebuild in 136.2 seconds and published the first coarse field in 0.42 seconds. An unchanged build took 0.71 seconds with zero mesh reads, BVH builds, distance queries or publications. These figures describe this test asset; the first coarse publication does not mean the entire scene is ready.

Build `NullusUnitTests` and run it with `--gtest_filter="DistanceField*.*"`.
The suites cover builder accuracy, sparse data, volume uploads, reflection, scene updates, DX12 composition, Editor/Cook agreement, cache invalidation, failed publication retention, and `.npak` reads without loose artifacts.

Interactive checks include settings/reimport, view switching, object movement/deletion, camera scrolling, view close/reopen, and correct mouse ownership. Test targets do not establish a completed interactive or cross-platform acceptance run.
