# Sprite slice resource foundation

Status: shared metadata/compiler, Grid/alpha slicing, Sprite Editor document/UI and Border/Sliced/Tiled IMPLEMENTED, with focused Core, Editor/Cook, live Editor and DX12/Player evidence on the frozen Windows RelWithDebInfo build. Physical desktop mouse operations and other platforms are tracked separately; the rest of Phase 2 remains open.

`Runtime/Core/Assets/SpriteSheet.h/.cpp` defines versioned slice metadata and the shared Single/Multiple compiler. Texture remains the primary `texture:main` artifact. Each slice publishes a Sprite v1 sibling with its own persistent UUID, display name, pixel rectangle, normalized pivot and PPU. Scene, Prefab and runtime references continue to use the existing source GUID plus Sprite sub-resource key.

## Metadata

Existing `.meta` settings gain two optional keys:

- `SPRITE_MODE`: `single` or `multiple`, case insensitive; absent means Single.
- `SPRITE_SLICES`: compact JSON with `version: 1` and a `sprites` array.

Each version 1 array entry contains `id`, `name`, integer `x/y/width/height`, two-element `pivot` and positive `pixelsPerUnit`. Version 2 also includes `border`; unbordered writes retain version 1. Pixel coordinates start at the source image's top left. IDs are canonical UUIDs; order and names never determine identity. Duplicate IDs, invalid dimensions, unsupported versions, malformed data and out-of-source rectangles fail before any new Sprite payload is appended. Limits are 65,536 slices and 16 MiB of metadata. Multiple mode can intentionally contain no slices; the primary Texture remains present.

Single mode retains the original builder and binary output, including the legacy `SPRITE_ID` fallback. When Sprite Editor converts Single to Grid or automatic slices, its first new slice retains the original Single UUID. Existing exact-rectangle matches retain their own UUIDs. Deleting an existing logical slice intentionally removes that reference target.

## Grid generation

`GenerateGridSpriteSlices` accepts cell dimensions, offsets, padding, a partial-cell flag and defaults for new cells. It reuses existing definitions with exactly matching rectangles, preserving UUID, name and edits. New cells receive new UUIDs. Duplicate existing rectangles cause an explicit identity-matching error. Serialization stores generated identities before Editor/Cook processing; the compiler never invents identity from array position.

The API also accepts manually authored definitions. `GenerateAlphaSpriteSlices` finds four-connected opaque regions in top-left RGBA data with configurable alpha threshold and minimum opaque-pixel count, preserves exact-rectangle identities and rejects oversized input. It uses packed visited bits and a bounded work queue. Border is covered below; Trim/Tight Mesh and physical outlines remain separate work.

## Sprite Editor

`Project/Editor/Sprites/SpriteDocument` decodes project source images and edits the same importer draft as Inspector. `Inspector/AssetImporterEditing.h` exposes durable draft edits through the global Undo service and delegates Apply/Revert to the existing importer actions. There is no second metadata writer or private Undo timeline.

Open a texture's Asset Browser context menu and choose **Open Sprite Editor**, or open **Window > Sprite Editor** and enter its project texture path. The window provides a GPU image preview, logical slice list, Select/Move and Create tools, bottom-right resize handles, name/rectangle/Pivot/PPU properties, Grid and automatic slicing dialogs, Apply & Reimport, Revert, Reload Source and Undo/Redo. Coordinates use source pixels; Pivot uses normalized bottom-left coordinates. Rectangle drags preview locally and create one Undo command only on release. A click without a change does not convert Single to Multiple. Escape cancels an uncommitted drag.

The `sprite-editor` input context owns save, revert, undo, redo, delete, cancel and frame-all commands while this panel or its children have keyboard focus. Source changes block edits/Apply until reload; metadata changes block Apply; edits from another panel cancel an unreleased drag. Apply uses the existing asynchronous import reservation, rollback and Undo completion boundaries.

Initial Core alpha and document tests are `tmp/2d-sprite-editor-core-tests.log` (23/23) and `tmp/2d-sprite-editor-all-tests.log` (121/121). The first real DX12 screenshot, `tmp/2d-sprite-workbench-visible.png`, proves the image and four slice outlines render, but exposes an oversized off-screen window and clipped tools. It is not full UI acceptance. Window placement, scrolling/property layout and frame-driven interaction regressions are being corrected and verified separately.

Follow-up Editor build: `tmp/2d-sprite-editor-reload-build.log`. Sprite Editor initialization/document/UI checks pass 11/11; complete native import/Prefab/workbench regression passes 127/127 in 95.884 seconds (`tmp/2d-sprite-editor-ui-final.log/XML`, `tmp/2d-sprite-editor-all-native.log/XML`). Five tests feed actual ImGui frame/input events without moving the desktop pointer. They cover window recovery, clean selection, create/move/resize, commit-on-release, cancel, Undo/Redo, child focus and conflicting draft edits. Creation snaps both endpoints to source pixels; a stationary click at a fractional source coordinate creates no rectangle. Earlier failed boundary-pixel and restricted script-build runs remain in Progress.md.

The subsequent watcher correction passes 128/128 (`tmp/2d-sprite-editor-watcher-tests.log/XML`). `tmp/2d-sprite-editor-watcher-live-evidence` supplies stable live Editor Apply/Undo/Redo checks against the shared draft, history and four published Sprite descriptors, matching original/final metadata hashes, a DPI-correct GPU screenshot and normal exit 0. Watcher notifications during active imports are deferred and compared after completion, preserving genuine external changes. Earlier live runners checked too soon and did not pass full acceptance; their misleading early workflow results and close timeouts remain documented.

[Sprite Editor user guide](../../zh-CN/editor/sprite-editor.md) describes the current controls. Trim/Tight Mesh/outline and Atlas tools are still required. These focused Windows results do not imply complete checkout, platform or Phase 2 acceptance.

## Border, Sliced and Tiled extension

Sprite descriptors add integer pixel borders in **Left, Bottom, Right, Top** order. Their horizontal/vertical sums must fit the source rectangle. Single import settings use `SPRITE_BORDER_LEFT/BOTTOM/RIGHT/TOP`. Multiple slices use a version 2 `border` array. Unbordered payloads and slice metadata retain their version 1 byte representation; readers accept both versions. The native Sprite envelope remains schema 1 and the logical bordered payload uses version 2. An explicit `nullus-sprite-compiler` dependency/version participates in Editor freshness and Cook cache decisions so a pre-extension artifact cannot hide the newly supported fields.

`SpriteGeometry` generates at most nine local rectangles and matching source UV rectangles. Corners use `border / PPU` world units. The center and edge spans stretch to the reflected SpriteRenderer **Size**. If the requested size is smaller than both borders, they shrink proportionally; zero-area patches are omitted. **Simple** retains source dimensions, while **Sliced** uses the explicit target size. Sprite Editor displays border fields and guides through the shared Undo draft.

Both CPU batching and GPU instancing consume the same patches, with one object sort key and one immutable resource owner. Culling uses target-size bounds and published counts remain logical Sprite counts. Tests check legacy/v2 data, limits, compressed sizes, coverage, source UV orientation, shared Undo, component serialization, Editor/Cook agreement and a nine-color DX12 texture. Completed evidence is listed below; previous workbench results describe their earlier snapshot. Trim/Tight Mesh, outlines, Atlas and Mask are still required.

The Tiled path repeats the center and border strips using their source size / PPU, clips the last tile instead of stretching it, and limits each Sprite to 65,536 generated quads. The same CPU and instanced submission paths consume these patches. A repeated checker texture with a transparent texel supplies explicit pixel/picking expectations. Script draw modes use GetDrawModeValue/SetDrawModeValue (0/1/2), while native/Inspector use the reflected enum. Trim/Tight Mesh, outlines, Atlas and Mask remain subsequent Phase 2 work.

Windows validation passed on 2026-11-02: 29 Core, 130 import/Prefab/Undo, 7 actual DX12 Sprite tests and 340 broad rendering tests. Live Editor Border Apply/Undo/Redo restored the exact metadata bytes and exited 0. Existing-project Sliced and Tiled archives and independent DX12 Players passed, each with one visible Sprite and one batch/draw. Logs, retained failed invocations, configuration and scope are in Progress.md. Desktop mouse editing of all fields and other backends/platforms remain open.

## Integration and evidence

Both `AssetDatabaseFacade.cpp` and `AssetCooker.cpp` call `AppendSpriteArtifacts`. The old Single helper remains available for existing callers. Every sibling records a dependency on the associated Texture sub-resource. Invalid definitions leave the caller's request unchanged, preserving the existing artifact writer's transactional publication.

- `tmp/2d-sheet-core-tests.log`: 22/22 Core Sprite/sheet tests passed.
- `tmp/2d-sheet-all-import-tests.log`: 118/118 Sprite/import/SDK/Prefab tests passed.
- `MultipleSpritesShareEditorCookAndRetainKeysAcrossReorderRenameAndRemoval`: checks warm Single-to-Multiple cooking, four slice descriptors, metadata reordering/name edits, source rename, removal, retained keys, primary Texture and equal Editor/Cook Sprite payloads.
- `tmp/2d-sheet-example-provenance.json`: native-serialized asset-only fixture copied into the existing SpriteValidation project. No `.nullus` file was created.
- Existing-project scene: `Assets/Scenes/SpriteSheetValidation.scene`; profile: `sprite-sheet-validation`. Four Sprite components use four logical children of one texture. `tmp/2d-sheet-package.log` records a validated archive with four Sprite artifacts, one Texture and the shared Shader.

Independent Player readback passed: `tmp/2d-sheet-player-evidence/evidence.json` and `presented.png`. Four distinct cropped regions are visible, all four submitted Sprites survive culling, and one batch/draw uses two leases. Both scripting hosts and product/Windows module isolation passed. The source sheet image is excluded from delivered content and authoring Sprite shader files were hidden/restored during this run. Other RHI/platform and authoring UI checks remain open.
