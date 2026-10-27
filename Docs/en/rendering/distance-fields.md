# Distance Fields

The first distance-field implementation supports static triangle meshes, rigid transforms, Mesh Distance Fields, Global Distance Fields, and Scene View diagnostics on Windows DX12. Particle collision, distance-field shadows/AO, skeletal deformation, Morph Targets, material vertex displacement, and terrain are not integrated.

## Enable and import

Generation is disabled by default. Enable **Generate Mesh Distance Fields** in Scene View's **Distance Fields** menu, or set this project `.nullus` option:

```ini
generate_mesh_distance_fields=true
```

Source-adjacent `.meta` settings control model import:

| Setting | Default | Meaning |
| --- | --- | --- |
| `DISTANCE_FIELD_GENERATION` | `inherit` | Inherit the project, force `enabled`, or force `disabled` |
| `DISTANCE_FIELD_RESOLUTION_SCALE` | `1` | Resolution scale from `0.125` to `4` |
| `DISTANCE_FIELD_TWO_SIDED` | `false` | Surface distance for open thin geometry without a negative interior |

Reimport after changing settings. Editor and command-line Cook share a CPU builder using import-scaled LOD0; fields do not switch with rendering LOD. Opaque and Masked triangles participate fully; Transparent triangles are excluded. Open/nonmanifold geometry reports quality diagnostics. A failed explicitly enabled build stops publication and retains the previous valid result.

MeshRenderer's **Affect Distance Field** controls participation. **Mostly Static Distance Field** selects the static cache; movement, mesh changes, and disabling still invalidate pages covering old bounds.

## Storage and runtime

Model fields use software indirection, physical `8³` bricks, and seven valid intervals per axis. Three mips are built separately with R8 UNORM and per-mip decode ranges; positive and negative saturated regions have distinct constant markers. The main artifact contains metadata and the lowest mip, while two higher mips load independently. Publication commits mesh and field payloads together, and runtime packages include those dependencies.

Material sections of one logical mesh share field generation. Complete, identically transformed section instances can share the union field; individually moving, disabling, or deleting sections makes remaining instances use their own fields.

Devices share a model atlas and a global physical-page pool, each budgeted at 128 MiB. Model upload is limited to 8 MiB per frame using DX12-aligned byte accounting. Lowest mips take priority; budget pressure degrades residency or reports allocation failure. Each view owns its page table and clipmaps, separating Scene, Game, and previews.

The global field uses four clipmaps with radii 16, 64, 256, and 1024 scene units, each with 20³ pages and 140³ valid intervals. It prefers R16F when the required UAV operations exist, otherwise uses R32F with fewer pages. Paging, binning, allocation, and composition execute on the graphics queue. Stationary views without resource changes do not recompose clean pages; invalid fine pages may fall back to valid coarser levels.

Nonuniform scale and mirroring use a conservative distance bound based on the minimum singular value. Singular/nonfinite transforms are excluded. Vulkan and Metal currently report distance fields as unsupported.

## Debugging and sampling

The **Distance Fields** menu exposes Mesh/Global modes and Surface, Trace Steps, Signed Slice, Normals, Clipmaps, Pages, and Residency views. Signed Slice uses world-space Y. Mesh Residency colors identify the sampled mip; Global Residency distinguishes fine pages, coarse fallback, and invalid pages. Counters report objects, bricks, pages, uploads, failed allocations, CPU preparation, and the latest completed GPU update. **Rebuild Distance Fields** invalidates pages and retries failed loads.

`DistanceFieldGpuResources` exposes GPU resources. `DistanceFieldSampling.hlsli` returns world-space distance, gradient, voxel scale, and validity; consumers must check validity.

## Validation

`NullusDistanceFieldTests` covers builder accuracy, sparse data, volume uploads, reflection, scene updates, and DX12 composition. `NullusDistanceFieldIntegrationTests` covers Editor/Cook agreement, cache invalidation, failed publication retention, and `.npak` reads without loose artifacts.

Interactive checks include settings/reimport, view switching, object movement/deletion, camera scrolling, view close/reopen, and correct mouse ownership. Test targets do not establish a completed interactive or cross-platform acceptance run.
