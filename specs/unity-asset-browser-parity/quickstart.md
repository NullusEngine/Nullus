# Quickstart: Unity Asset Browser Parity

## Manual Verification Scenario

1. Open the editor with a project containing `Assets/Models/main_sponza` and imported FBX/glTF scene assets.
2. Open `Window > Asset Browser`.
3. Verify the panel opens as a Unity-style Project view:
   - left side shows an `Assets/` folder tree,
   - right side shows a thumbnail grid,
   - Engine built-in assets are not visible.
4. Select `Assets/Models/main_sponza` in the tree.
5. Verify the breadcrumb reads `Assets > Models > main_sponza`.
6. Verify the grid shows direct folder content plus generated sub-assets for imported scenes.
7. Change thumbnail size with the bottom slider and verify grid density changes without changing the selected folder.
8. Enter a search query such as `lamp` and verify only current-folder matching items remain visible.
9. Select a type filter such as `Material` and verify only material items remain visible.
10. Wait for thumbnails to finish and verify:
    - textures show image previews,
    - materials show generated previews or stable fallback icons,
    - models/prefabs show generated previews or stable fallback icons.
11. Restart the editor and verify visible thumbnails are reused from `Library/AssetThumbnails/` when assets are unchanged.
12. Exercise core workflows from the new panel:
    - import asset,
    - reimport model,
    - create folder/scene/shader/material,
    - rename/delete/duplicate,
    - double-click scene,
    - double-click prefab,
    - drag generated prefab into Scene View,
    - select material/texture/model and open Asset Properties/Asset View.

## Automated Verification Focus

- Presentation item construction for project-only folders.
- Generated sub-asset expansion for imported model scenes.
- Current-folder search and type filtering.
- Breadcrumb and folder-selection fallback.
- Thumbnail cache key and freshness decisions.
- Drag payload preservation for source and generated asset items.
