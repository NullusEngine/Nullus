# Implementation phases

All modules A–K and all three demos are required. Phase boundaries are recovery checkpoints, not scope reductions.

| Phase | Deliverable | Acceptance |
|---|---|---|
| 0 | Source audit, architecture, requirements matrix, formats, update order and progress | Every requested capability has a phase and acceptance entry; proceed directly to code |
| 1 | Single Sprite assets/import, SpriteRenderer, Unlit, sorting, culling, batching, existing 2D view integration, templates | Create/import/drag/edit/save/reopen/Play in Editor; Cook and independent Player smoke |
| 2 | Sprite Editor, slicing, pivot/border/trim/tight mesh, sliced/tiled rendering, atlas, masks/groups, Pixel Perfect, camera helpers | Stable references after slicing/repacking/reimport; usable Undo and picking |
| 3 | Complete Physics2D shapes, bodies, materials, joints, queries, events, interpolation, scripts and tools | Platformer mechanics, safe callbacks/destruction and isolated worlds |
| 4 | Chunked Tilemap, palette tools, rules/random/animated tiles, incremental render/physics rebuild and streaming | Author/save/run/package a level; local edits stay local |
| 5 | Generic property/reference tracks, frame clips, library/resolver, Animator and workbench integration | Idle/Run/Jump/Attack and script/event flow; 3D animation regression |
| 6 | Sprite Lit, all light kinds, normal/emission/shadows; material graph, VFX, camera shake, UI/audio/postprocessing | Lighting Showcase and mixed transparent rendering |
| 7 | Bones/weights/IK and CPU deformation tools, SpriteShape geometry/collisions, iso/hex grids, Aseprite/WebP/PSD/PSB | Every advanced feature has an operational tool and example |
| 8 | Platformer, Pixel RPG, Lighting Showcase; docs/templates/benchmarks/desktop packaging | Full twenty-step workflow and 3D regressions on declared targets |

## Phase 1 sequence

1. Sprite descriptor, validated artifact codec, stable subresource metadata and runtime reference resolution.
2. Shared Editor/Cook import, preserving the primary Texture identity.
3. Reflected component, scene extraction and immutable resource ownership.
4. Real RHI geometry/instances, adjacent compatible batching, sorting, visibility and statistics.
5. Inspector, creation menus, browser/Scene drag and picking, Undo/Prefab/save, templates.
6. Build, tests, real visible editor operation and standalone package evidence.

Do not move a phase to VERIFIED while any required acceptance gate is unexecuted. Store the exact next command and blockers in Progress.md. No automatic Git commit/reset or termination of existing processes.
