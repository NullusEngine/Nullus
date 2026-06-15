# Research: Unity Asset Browser Parity

## Decision: Replace The Existing Asset Browser In Place

**Rationale**: The user chose direct replacement. Keeping the panel name and menu identity preserves existing editor entry points while avoiding two competing asset browsers.

**Alternatives considered**: Adding a parallel `Project Browser` would reduce migration risk but creates duplicate workflows and cleanup debt. Keeping the old tree and bolting a grid next to it would preserve too much old state and undercut Unity-style parity.

## Decision: Show Project Assets Only

**Rationale**: The user said Engine built-in assets do not need to be displayed. Unity's Project panel centers user project content; Nullus managers can still load built-ins through resource paths when needed.

**Alternatives considered**: Showing Engine and Project roots as peers matches the old browser but conflicts with the requested screenshot. Hiding Engine assets only by default but exposing a toggle is deferred.

## Decision: Use Current-Folder Search And Type Filtering

**Rationale**: The user selected scoped current-folder search with type filtering. This matches the first visible Project panel slice and keeps navigation state easy to reason about.

**Alternatives considered**: Full-project search is useful but requires a separate result mode and ranking semantics. Search-only without filters would leave large imported folders harder to scan.

## Decision: Expand Generated Sub-Assets Into The Grid

**Rationale**: The user selected full Unity-style visibility. Supported imported outputs such as generated prefabs, meshes, materials, textures, and shaders should be visible and draggable as first-class grid items.

**Alternatives considered**: Folding sub-assets beneath the source file is tidier but less like Unity and weaker for FBX/glTF parity inspection. Showing only scene-droppable assets hides useful material/mesh diagnostics.

## Decision: Persist Thumbnails Under Project Library

**Rationale**: The user selected persistent cache semantics. `Library/AssetThumbnails/` follows the existing project Library pattern and avoids regenerating every thumbnail after editor restart.

**Alternatives considered**: In-memory cache is simpler but makes large projects feel slow every session. Deferring persistence would risk shipping a visually aligned but operationally weak Project panel.

## Decision: Separate Deterministic Presentation From Rendering Work

**Rationale**: Current `AssetBrowser.cpp` mixes filesystem traversal, menus, drag/drop, and preview behavior. Presentation item construction, filtering, and cache-key logic can be tested without a live editor window or renderer.

**Alternatives considered**: Rewriting everything directly in `AssetBrowser.cpp` is faster initially but would make this already-large panel harder to verify and review.

## Decision: Use Persistent Thumbnails With Stable Fallback Icons

**Rationale**: The user selected the visually strong path. Texture thumbnails can use decoded texture content; material, model, and prefab items can use deterministic generated previews or stable type icons until renderer-backed preview generation has runtime evidence. Browsing must still work when generation fails.

**Alternatives considered**: Type icons only would be faster and enough for basic workflows but not close enough to Unity's visual grid for this request. Renderer-backed AssetPreview parity is deferred because it needs focused runtime/rendering validation.
