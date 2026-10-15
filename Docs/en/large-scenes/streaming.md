# Residency and Budgets

The residency system decides which resources should currently be in memory. It must satisfy three constraints at once: data the frame needs must be present, memory usage has a ceiling, and loading must not block the frame.

## Principle

The system keeps two things separate:

| Concept | Meaning |
| --- | --- |
| Interest set | Resources the current view and logic consider necessary |
| Resident set | Resources already loaded and in memory |

The difference between them is the pending load queue. Separating them means duplicate requests can be merged, load order can follow priority, and loads can be deferred when the budget is tight without breaking correctness.

## Requests and deduplication

- Multiple requests for the same resource merge into one entry.
- Requests carry priority; scene mesh artifacts are submitted at high priority ahead of material and texture decode.
- Commits and evictions are accounted in one queue, exposing request, commit, and evict counts.

## Budgets

Budgets constrain three resource kinds:

- CPU memory
- GPU memory
- Per-frame upload volume

When a budget is exceeded, eviction follows priority and recency rather than failing outright. Eviction respects frame-retirement pins: a resource referenced by the current frame cannot be reclaimed.

## Frame retirement pins

Rendering is multi-threaded, so the CPU no longer referencing a resource does not mean the GPU has finished with it. Residency keeps retirement pins that cooperate with the frame resource ring and GPU fences so reclamation happens at a genuinely safe point.

## Relationship to the resource queue

Editor startup uses the asynchronous scene resource pipeline. Windows defaults to `pipelined`, other platforms to `async`:

- Scene mesh artifacts are submitted at high priority.
- Material and texture decode stay at normal priority to keep the queue fair, so heavy geometry never starves textures.

To compare against the previous behavior, switch back to the synchronous prewarm path:

```powershell
Editor.exe --editor-startup-resource-pipeline legacy <project>
```

## Observability

Residency exposes request, commit, and evict counts together with reasons. Reading them follows the same approach as other telemetry; see [Profiling and Graphics Debugging](../quality/profiling.md).

The editor currently exposes a telemetry summary switch for thumbnails (`--editor-thumbnail-telemetry-summary <path>`). Residency counters are published with renderer statistics and have no separate summary switch.

## Current boundaries

- Terrain and world partitioning are not implemented, so region-based streaming of a world is not available yet.
- Cubemap runtime loading and streaming mips remain extension points; see [Models, Meshes, and Textures](../assets/models-textures.md).
- Default budgets are conservative and should be tuned per project.

These boundaries are recorded in the [Feature Status Matrix](../roadmap/index.md), and the implementation order follows the dependencies listed in the related chapters.

## Next steps

- [Visibility and Occlusion](visibility-occlusion.md) removes invisible objects first.
- [LOD and HLOD](lod-hlod.md) reduces distant load with proxies.
