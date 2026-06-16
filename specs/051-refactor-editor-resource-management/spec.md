# Feature Specification: Editor Resource Management Refactor

**Feature Branch**: `051-refactor-editor-resource-management`  
**Created**: 2026-06-15  
**Status**: Draft  
**Input**: User description: "重构编辑器资源管理。需要与运行时资产和内置资产区分开，但也需要统一管理；程序图标编译进 EXE；菜单、工具栏、操作按钮使用 Font Awesome 图标字体并合并进 ImGui FontAtlas；Scene、Prefab、Mesh、Material 等彩色图标由 SVG 构建阶段转换成 PNG 图集，发布时放进 EditorResources 或 pak；资源预览使用独立纹理 + ThumbnailCache；资源路径基于 EXE 所在目录，不使用当前工作目录；从一开始抽象成同一套资源索引，开发态读文件、发布态读 pak/打包资源；只保留当前用到的所有图标，没用到的直接删掉，所有图标去除 Unity 相关命名。"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Stable Editor Resource Lookup (Priority: P1)

As an editor user, I need the editor to find its own icons, fonts, helper models, shaders, and default layouts no matter what current working directory launched the executable, so startup and UI rendering are stable from IDE, launcher, terminal, and packaged builds.

**Why this priority**: The current resource lookup depends on relative paths such as `../Assets/...`, which can break startup or make editor UI resources disappear when the process starts from a different directory.

**Independent Test**: Launch or instantiate editor resource path resolution from a non-repository working directory and verify every required editor resource resolves from the executable installation root.

**Acceptance Scenarios**:

1. **Given** the editor executable is located under an installation directory, **When** the process current working directory is changed before startup, **Then** editor resource paths still resolve under the executable-relative `Assets/Editor` and `Assets/Engine` roots.
2. **Given** a required editor resource exists in the development `App/Assets` layout, **When** the editor asks for that resource by logical ID, **Then** it receives the same resource without referencing the process current directory.
3. **Given** a required editor resource is missing, **When** the editor asks for it, **Then** the failure is logged with the logical resource ID and candidate location.

---

### User Story 2 - Unified Catalog Across Development and Release (Priority: P1)

As an engine developer, I need one logical resource index that can serve editor resources from loose development files or packaged release storage, so callers use stable IDs instead of file layout assumptions.

**Why this priority**: The editor needs to separate editor resources from runtime and project assets while still managing them consistently. A catalog abstraction prevents duplicate lookup logic and enables release packaging later.

**Independent Test**: Resolve the same set of logical editor resource IDs through a loose-file catalog backend and a packaged-resource-capable backend contract, verifying callers do not change IDs or paths.

**Acceptance Scenarios**:

1. **Given** a catalog entry exists for an editor icon, **When** development mode is active, **Then** the catalog resolves the entry to a loose file under `App/Assets/Editor`.
2. **Given** the same catalog entry exists in packaged metadata, **When** release mode is active, **Then** the catalog resolves through the packaged backend without changing the public resource ID.
3. **Given** runtime engine assets and editor assets both exist, **When** the editor catalog is enumerated, **Then** editor-only assets are distinguishable from runtime assets and project/user assets.

---

### User Story 3 - Clean Nullus Icon Set (Priority: P2)

As an editor user, I need the asset panel, toolbar, menus, and action buttons to display Nullus-owned icons with consistent names and only the icons that are actually used by the editor, so the resource tree is maintainable and no Unity-branded naming leaks into the product.

**Why this priority**: The existing editor icon directory contains many unused imported icons and file names such as `unity_project_*`. These make the resource system noisy and the UI inconsistent.

**Independent Test**: Inspect the editor resource catalog and `App/Assets/Editor` icon tree to verify every cataloged icon is referenced by the editor and no path or logical ID contains Unity-related names.

**Acceptance Scenarios**:

1. **Given** the asset browser needs a folder, scene, prefab, mesh, material, texture, shader, script, or default asset icon, **When** it requests the fallback icon ID, **Then** the ID uses a Nullus-style namespace and resolves to a non-Unity-named asset.
2. **Given** a toolbar or menu action uses an icon, **When** the editor loads the UI font atlas, **Then** icon-font glyphs are available from the same UI font atlas path used by text rendering.
3. **Given** the editor resource directory is scanned, **When** unused icon files are compared with the catalog, **Then** unused legacy icon files are absent from the tree.

---

### User Story 4 - Asset Previews Remain Separate From Editor Icons (Priority: P3)

As an editor user browsing project assets, I need thumbnails for previewable resources and type icons for non-previewable resources, so generated asset previews do not pollute editor icon ownership or package layout.

**Why this priority**: Material, prefab, and texture previews have different lifecycle and cache behavior from static editor UI icons. They should remain independent textures under ThumbnailCache.

**Independent Test**: Request thumbnails for previewable and non-previewable asset browser entries and verify preview textures are served from ThumbnailCache while fallback icons resolve through the editor catalog.

**Acceptance Scenarios**:

1. **Given** a texture asset has a generated thumbnail, **When** the asset browser displays it, **Then** the thumbnail preserves the source aspect ratio and uses ThumbnailCache instead of the static icon catalog.
2. **Given** a material or prefab can generate a preview, **When** the asset browser displays it, **Then** the preview is loaded as a thumbnail texture and transparent background is preserved where available.
3. **Given** an asset cannot generate a preview, **When** the asset browser displays it, **Then** a cataloged fallback icon is used.

### Edge Cases

- The executable path cannot be determined or canonicalized on a platform.
- Development loose files are unavailable, but packaged resources exist.
- Packaged resources are unavailable, but loose files exist in a development checkout.
- A catalog entry points to a file that was pruned or renamed.
- Two catalog entries accidentally point to the same logical ID.
- A resource path attempts to escape the allowed editor/runtime resource roots.
- A generated thumbnail is stale, corrupt, oversized, or missing.
- Current working directory changes after editor startup.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The editor MUST resolve editor resource roots from the executable location or an explicit installation root, not from the process current working directory.
- **FR-002**: The system MUST provide one logical editor resource catalog that maps stable Nullus-style resource IDs to resource metadata and backend locations.
- **FR-003**: The catalog MUST support loose-file development resources and packaged-release resources through the same public lookup interface.
- **FR-004**: The catalog MUST classify resources as editor-only, runtime built-in, project/user, or generated preview/cache resources.
- **FR-005**: Editor callers MUST request icons, fonts, helper models, shaders, layouts, and brand assets through catalog IDs or catalog-derived paths rather than hard-coded `../Assets/...` paths.
- **FR-006**: The editor MUST keep program window/application icons as compiled application resources where the platform supports it, with a catalog entry for runtime UI use of the same brand mark.
- **FR-007**: Menu, toolbar, and operation-button glyphs MUST be loaded through the UI icon font path and merged into the ImGui font atlas.
- **FR-008**: Asset type icons for folder, scene, prefab, mesh, material, texture, shader, script, font, audio, and default assets MUST use Nullus-owned logical IDs and filenames.
- **FR-009**: No logical resource ID, active catalog entry, or retained editor icon filename MAY contain Unity-related naming.
- **FR-010**: Unused editor icon files MUST be removed from the retained editor resource tree unless explicitly listed as required by a current UI call site or catalog entry.
- **FR-011**: Static editor icons MUST be distinct from generated asset thumbnails; generated thumbnails MUST remain independent textures managed through ThumbnailCache.
- **FR-012**: Material and prefab preview generation MUST continue to use preview/thumbnail paths and fall back to cataloged type icons only when a preview is not available.
- **FR-013**: Resource lookup failures MUST include the logical ID, resource class, backend mode, and attempted location in diagnostics.
- **FR-014**: The resource catalog MUST be testable without starting the full editor window or graphics device.
- **FR-015**: The editor resource layout MUST be organized under `App/Assets/Editor` with clear groups for brand, fonts, icon sources, generated icon outputs, packaged metadata, helper models, shaders, settings, and preview cache boundaries.

### Key Entities

- **Editor Resource Catalog**: The logical index of editor resources. It stores stable IDs, resource category, scope, backend hints, loose-file path, packaged path, and expected use.
- **Resource ID**: A stable Nullus namespace string such as `editor.icon.asset.scene`, `editor.font.ui.default`, or `editor.brand.logo.mark`.
- **Resource Backend**: The lookup provider that returns bytes or paths from either loose development files or packaged release storage.
- **Resource Root Resolver**: The component that derives install, editor asset, runtime asset, and project/user asset roots without relying on current working directory.
- **Icon Manifest**: The subset of static icon resources currently used by editor UI and asset browser fallbacks.
- **Thumbnail Cache**: Generated preview storage for project assets, separate from static editor icons and packaged editor resources.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of editor startup resource paths for fonts, layouts, helper shaders, helper models, toolbar icons, asset type icons, and brand icons resolve correctly after changing current working directory away from the repository.
- **SC-002**: 100% of retained static editor icon files are referenced by catalog entries or current editor call sites.
- **SC-003**: 0 retained editor icon filenames and 0 active catalog IDs contain Unity-related naming.
- **SC-004**: The asset browser fallback icon test suite verifies all asset types resolve to Nullus-style IDs.
- **SC-005**: Catalog lookup tests cover both loose-file and packaged-backend modes for the same logical IDs.
- **SC-006**: Missing-resource diagnostics identify the failed logical ID and backend mode in one log message.
- **SC-007**: Material, prefab, and texture preview behavior remains separate from static icon fallback behavior in focused tests.

## Assumptions

- The first implementation can provide the packaged backend as an interface or manifest-backed stub if full pak serialization is not yet present, as long as callers already use the same catalog interface.
- The development layout remains rooted at `App/Assets` in source checkouts, while installed builds place `Assets` next to the executable or inside the configured package backend.
- Runtime built-in assets under `App/Assets/Engine` remain available to runtime systems, but editor-only UI resources move or remain under `App/Assets/Editor`.
- The resource refactor should update existing tests and add focused tests before broad visual polish.
- The implementation should preserve existing generated thumbnail cache behavior unless a change is required to keep previews separated from editor icons.
