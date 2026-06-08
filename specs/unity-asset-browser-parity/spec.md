# Feature Specification: Unity Asset Browser Parity

**Feature Branch**: `unity-asset-browser-parity`  
**Created**: 2026-06-08  
**Status**: Draft  
**Input**: User asks to open an isolated worktree and optimize/refactor the Nullus asset panel so the visible Project/Asset Browser experience aligns with Unity's Project panel. Engine built-in assets do not need to be displayed.

## User Scenarios & Testing

### User Story 1 - Navigate Project Assets Like Unity (Priority: P1)

Artists and developers browsing a Nullus project should see a Unity-style Project panel: a left `Assets/` directory tree, a right current-folder content grid, a breadcrumb path, and project-only content without Engine built-in assets mixed into the view.

**Why this priority**: This is the minimum visible parity slice. Without a split tree/grid layout, later thumbnail and filtering work still feels like the old tree browser.

**Independent Test**: Open the Asset Browser on a project containing folders and files under `Assets/`, select nested folders from the left tree, and verify the right grid shows only the selected folder's direct contents with matching breadcrumb state.

**Acceptance Scenarios**:

1. **Given** a project with `Assets/Models/main_sponza`, **When** the user selects `main_sponza` in the left tree, **Then** the right content grid shows direct items from that folder and the breadcrumb reads `Assets > Models > main_sponza`.
2. **Given** Engine assets exist outside the project `Assets/`, **When** the Asset Browser is opened, **Then** Engine built-in assets are not shown in the tree or content grid.
3. **Given** the selected folder is deleted or moved externally, **When** the asset watcher refreshes the panel, **Then** selection falls back to the nearest valid project folder and the grid remains usable.

---

### User Story 2 - Inspect Imported Assets In A Thumbnail Grid (Priority: P1)

Artists comparing imported FBX/glTF scenes should see source files and generated sub-assets directly in the current-folder grid, including generated prefabs, meshes, materials, textures, shaders, scenes, scripts, and folders.

**Why this priority**: The current tree hides imported scene outputs behind text-only entries. Unity-style parity requires imported outputs to be visible, selectable, draggable, and previewable from the grid.

**Independent Test**: Import a model scene with generated prefab, mesh, material, and texture artifacts, select the containing folder, and verify each generated sub-asset appears as a separate grid item with stable selection and drag payload behavior.

**Acceptance Scenarios**:

1. **Given** an imported `Assets/main_sponza/NewSponza_Main_glTF_003.fbx`, **When** import artifacts are current, **Then** the grid shows the source file plus generated prefab, mesh, material, and texture sub-assets as individual items.
2. **Given** a generated prefab grid item, **When** the user drags it into the scene, **Then** the existing generated prefab drag/drop workflow is used instead of reparsing the source file on the foreground path.
3. **Given** a material, texture, model, or prefab grid item, **When** the user selects it, **Then** Asset Properties targets the selected asset or sub-asset.
4. **Given** a scene grid item, **When** the user double-clicks it, **Then** the existing scene load behavior runs.

---

### User Story 3 - See Real Persistent Thumbnails (Priority: P1)

Artists should see real thumbnail previews comparable to Unity's Project grid. Textures use their image content, materials use material preview spheres, models and prefabs use rendered model previews, and unsupported types use project icons.

**Why this priority**: The requested target explicitly chooses the high-fidelity thumbnail path. Without persistent thumbnails, the grid cannot provide Unity-like visual scanning for large imported scenes.

**Independent Test**: Open a folder containing textures, materials, models, and prefabs; wait for thumbnail generation; restart the editor; verify the grid reuses cached thumbnails from the project Library until source or artifact stamps change.

**Acceptance Scenarios**:

1. **Given** a project texture asset, **When** the grid displays it, **Then** its thumbnail shows the texture image rather than a generic icon.
2. **Given** a generated or authored material asset, **When** the grid displays it, **Then** its thumbnail shows a lit material sphere preview.
3. **Given** a model or generated prefab asset, **When** the grid displays it, **Then** its thumbnail shows a rendered asset preview when generation succeeds, otherwise a stable type icon with an error diagnostic.
4. **Given** thumbnail cache entries exist under `Library/AssetThumbnails/`, **When** the editor restarts and the source/import stamps are unchanged, **Then** the thumbnails are reused without regenerating all visible assets.
5. **Given** a source asset, meta file, generated artifact, or thumbnail setting changes, **When** the grid next requests the thumbnail, **Then** the stale thumbnail is invalidated and rebuilt.

---

### User Story 4 - Search And Filter Current Folder (Priority: P2)

Users working in large asset folders should be able to filter the right grid by text and by asset type while keeping the left tree focused on navigation.

**Why this priority**: Unity's Project panel includes search/filter affordances, and large imported folders need quick narrowing. The first version is intentionally scoped to the selected folder.

**Independent Test**: Select a folder containing mixed asset types, enter a query, choose a type filter, and verify only direct current-folder grid items matching both filters remain visible.

**Acceptance Scenarios**:

1. **Given** the current folder contains many materials and meshes, **When** the user types `lamp` in the search box, **Then** only current-folder items whose display name contains `lamp` remain visible.
2. **Given** the current folder contains materials, textures, and prefabs, **When** the user selects the `Material` type filter, **Then** only material items remain visible.
3. **Given** a search query and type filter are active, **When** the user clears the query or returns the filter to `All`, **Then** the grid returns to the broader result set without changing the selected folder.

---

### User Story 5 - Preserve Existing Asset Workflows (Priority: P1)

Users who already rely on the Asset Browser should keep existing project asset operations while gaining the Unity-style layout.

**Why this priority**: Replacing the current panel must not regress essential editor workflows such as import, reimport, create, rename, delete, duplicate, preview, drag/drop, and prefab-stage opening.

**Independent Test**: Exercise each existing file/folder operation from the new grid or tree and verify it calls the same underlying editor workflow or produces equivalent project state.

**Acceptance Scenarios**:

1. **Given** the user right-clicks a project folder, **When** they choose Create Folder/Scene/Shader/Material, **Then** the asset is created in that folder and appears in the right grid.
2. **Given** the user right-clicks a model source asset, **When** they choose Reimport, **Then** the existing project reimport pipeline runs and the grid refreshes after completion.
3. **Given** the user renames, duplicates, or deletes a file from the grid, **When** the operation succeeds, **Then** existing propagation behavior updates dependent references and panel state.
4. **Given** the user drags a hierarchy object into a project folder, **When** the drop commits, **Then** the existing Save As Prefab workflow runs and the resulting prefab appears in the grid.

### Edge Cases

- Empty project `Assets/` folders must show a useful empty state without showing Engine assets.
- Unknown file types should not crash the panel; they may appear with a generic file icon only when they are valid project files that the editor can manage safely.
- Meta files and Library artifacts must not appear as user-facing grid items.
- Thumbnail generation failures must not block browsing, selection, drag/drop, or other asset operations.
- The thumbnail cache must not escape the project root or write into source `Assets/`.
- Large folders must remain scrollable and responsive while thumbnails are pending.
- Search/filter should apply only to the current folder in v1, not to the entire project.
- Generated sub-assets must remain associated with their source asset for invalidation, drag payloads, and properties.
- Project file watcher updates may arrive while a thumbnail job is running; stale thumbnail results must not replace newer cache entries.
- Existing `Asset View`, `Asset Properties`, `Scene View`, `Hierarchy`, `Material Editor`, and object-reference picker integrations must remain compatible with generated sub-asset payloads.

## Requirements

### Functional Requirements

- **FR-001**: The Asset Browser MUST keep the existing panel/menu identity while replacing the old single tree view with a split left-tree/right-grid Project panel.
- **FR-002**: The Asset Browser MUST display project `Assets/` content only and MUST NOT show Engine built-in assets in the v1 tree or grid.
- **FR-003**: Users MUST be able to select folders from the left tree and see only the selected folder's direct user-facing contents in the right grid.
- **FR-004**: The right grid MUST show a breadcrumb path for the selected folder.
- **FR-005**: The right grid MUST show folders, supported source assets, and generated model-scene sub-assets as selectable grid items.
- **FR-006**: Generated prefab, mesh, material, texture, and shader sub-assets MUST appear as separate grid items when the asset database reports current imported records for the selected folder.
- **FR-007**: Grid items MUST preserve existing drag payload behavior for files, generated prefabs, sub-assets, and hierarchy-object-to-folder prefab creation.
- **FR-008**: Grid item selection MUST update `Asset Properties` with the selected asset or sub-asset target where supported.
- **FR-009**: Double-click behavior MUST preserve existing scene loading, prefab-stage opening, and preview behavior for supported asset types.
- **FR-010**: Existing folder and file context menu operations MUST remain available from the new tree/grid experience.
- **FR-011**: The panel MUST support manual refresh and import actions from the Project panel toolbar.
- **FR-012**: The panel MUST support current-folder text search.
- **FR-013**: The panel MUST support current-folder type filtering for at least All, Folder, Model, Prefab, Mesh, Material, Texture, Shader, Scene, and Script.
- **FR-014**: The panel MUST provide a thumbnail-size control that changes grid density without losing selected folder state.
- **FR-015**: Texture grid items MUST display real texture thumbnails when the texture can be loaded.
- **FR-016**: Material grid items MUST display generated material-sphere thumbnails when thumbnail generation succeeds.
- **FR-017**: Model and prefab grid items MUST display generated rendered thumbnails when thumbnail generation succeeds.
- **FR-018**: Unsupported or failed thumbnail types MUST display stable type icons and record diagnostics without blocking browsing.
- **FR-019**: Thumbnail files MUST be stored under the active project `Library/AssetThumbnails/` directory.
- **FR-020**: Thumbnail cache identity MUST include asset identity, sub-asset key when present, source/meta/artifact freshness inputs, thumbnail kind, and thumbnail settings that affect pixels.
- **FR-021**: Stale thumbnail cache entries MUST be invalidated when source assets, meta files, generated artifacts, or relevant thumbnail settings change.
- **FR-022**: Thumbnail generation MUST be asynchronous or otherwise non-blocking from the user's perspective for large visible folders.
- **FR-023**: The panel MUST keep the asset watcher/preimport refresh path functional after the layout refactor.
- **FR-024**: The panel MUST hide `.meta` files, project `Library` artifacts, and implementation-only cache files from user-facing grid results.
- **FR-025**: The presentation layer MUST be testable without requiring a live editor window.
- **FR-026**: The thumbnail cache key and stale/fresh decisions MUST be testable without requiring a live renderer.

### Key Entities

- **Project Folder Selection**: The current folder under project `Assets/` that drives the right grid and breadcrumb.
- **Asset Browser Item**: A user-facing grid entry representing a folder, source asset, or generated sub-asset.
- **Asset Type Filter**: The current grid type filter such as All, Folder, Model, Prefab, Mesh, Material, Texture, Shader, Scene, or Script.
- **Thumbnail Request**: A request for a visual preview for one asset browser item at a requested size/settings combination.
- **Thumbnail Cache Entry**: A persisted preview image and metadata stored under `Library/AssetThumbnails/`.
- **Generated Sub-Asset Entry**: A database-backed item such as generated prefab, mesh, material, texture, or shader that belongs to a source asset.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Users can navigate from `Assets/` to a nested folder and see correct direct-folder grid contents in under 3 clicks.
- **SC-002**: Engine built-in assets do not appear in the Asset Browser tree or grid during v1 verification.
- **SC-003**: A folder containing an imported model scene shows the source file plus generated prefab, mesh, material, and texture sub-assets as independent grid items.
- **SC-004**: At least texture, material, model, and prefab assets display real or generated thumbnails after pending work completes, with stable type-icon fallback on failure.
- **SC-005**: Restarting the editor with unchanged assets reuses existing `Library/AssetThumbnails/` entries for visible assets instead of regenerating every thumbnail.
- **SC-006**: Search and type filtering reduce current-folder grid results deterministically without changing the selected folder.
- **SC-007**: Existing import, reimport, create, rename, delete, duplicate, preview, scene load, prefab stage open, and drag/drop workflows remain functional from the new panel.
- **SC-008**: Focused automated tests cover presentation item construction, project-only filtering, current-folder search/type filtering, generated sub-asset expansion, thumbnail cache identity, and thumbnail freshness decisions.

## Assumptions

- "Unity parity" for this feature means the visible Project panel behavior from the provided screenshot plus core project asset workflows, not every Unity Project Browser feature.
- Favorites, Packages, label/tag search, global project search, one-column/two-column Unity layout switching, and custom asset labels are out of scope for v1.
- Engine built-in assets can remain loadable by managers and object pickers; they are only hidden from this Asset Browser view.
- The editor can add small purpose-built presentation/cache classes under `Project/Editor/Assets` or `Project/Editor/Panels` without introducing a new asset database.
- Real rendered thumbnails for material/model/prefab previews may use existing editor rendering resources and can fall back to icons when a renderer/backend capability is unavailable.
- Unit tests should cover deterministic non-rendering behavior; renderer thumbnail visual quality may require focused runtime/manual verification.
