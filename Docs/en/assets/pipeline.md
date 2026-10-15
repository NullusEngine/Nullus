# Asset Browser and Import

The content pipeline turns source files on disk into runtime data that is both usable and traceable back to its origin. The Asset Browser is the interface to that pipeline, and the asset database is its implementation.

## Asset layers

| Layer | Content | Location |
| --- | --- | --- |
| Source asset | Files and scripts provided by the user: models, textures, materials, scenes, Prefabs, C#/Lua scripts | `Assets/` |
| Import metadata | Identity, version, and import settings per source asset | Asset database |
| Artifact | Runtime data produced by import, carrying platform and encoder information | Content storage |
| Manifest | Describes which artifacts and dependencies one asset produced | Stored with the artifacts |

Source assets are identified by GUID rather than path, so moving or renaming a file keeps references intact. The path is only a hint and a display string.

## Asset Browser

The Asset Browser provides:

- Hierarchical browsing of folders, source assets, and generated sub-assets
- Name search and type categories
- Type icons and asynchronous thumbnails
- Drag and drop into the scene, onto Inspector fields, or onto objects
- Object reference pickers
- Preview in the Asset View panel

Thumbnails are generated per type: textures, material spheres, model previews, Prefab previews, and a generic icon. A generation failure records a reason and falls back to the type icon instead of leaving a blank entry.

Thumbnail and preview generation is subject to per-frame budgets and caching, so importing a large asset is not a synchronous operation.

## Import flow

### Import a model or texture

Prerequisites: an open project and a source format listed in [Models, Meshes, and Textures](models-textures.md).

1. Select the destination in the Asset Browser and use `Show in explorer` to open its disk location, or open the project's `Assets/` directly.
2. Copy the model and its texture dependencies there, preserving relative directories used by the model. When copying from another project, copy source assets rather than its `Library/`.
3. Return to the editor and wait for watcher refresh and import. Read failure diagnostics in the Console; a large model's thumbnail can appear later than its asset entry.
4. Expand the model's generated Prefab/mesh/material sub-assets and drag the Prefab into the Hierarchy. Textures can be dragged onto compatible material reference fields.
5. Saving changes to the external source triggers incremental import. To force a rebuild, right-click the source asset and choose `Reimport`. Do not overwrite generated sub-assets as independent source files.

Expected result: the browser shows the source and applicable sub-assets, the model instance is selectable in the scene, and the Console has no import errors.

`Import Here...`, `Create > Scene`, and `Create > Material` are currently disabled. Import by copying into `Assets/`; create and save scenes through `File`. Rename or move existing assets through editor operations and preserve their `.meta` identity data; moving only the source file can change reference identity.

```text
source file changes
  -> asset watcher marks it dirty
  -> importer runs with the asset's import settings
  -> artifacts are generated and a manifest is written
  -> the asset database publishes a new version
  -> the editor refreshes references and previews
```

Import settings are stored per asset. Changing them regenerates artifacts rather than modifying them in place.

The editor exposes a headless prewarm entry point to finish importing before a timing run or a release:

```powershell
Tools/Performance/Warm-NullusProjectAssets.ps1 -ProjectPath .\TestProject\TestProject.nullus
```

The script fully imports startup source assets and verifies available disk space. Its one-time migration cost is excluded from startup acceptance timing.

## Import setting storage

Texture and static mesh import settings are stored as explicit, serializable snapshots:

- Textures: type, color space, alpha, mip, maximum size, resize policy, compression intent, and per-platform overrides.
- Static meshes: LOD group, minimum LOD, automatic LOD screen size, plus per-level source kind, screen size, and mesh data.

Defaults and their effects are in [Models, Meshes, and Textures](models-textures.md).

## Scripts and scene assets

C# and Lua scripts are assets in the same pipeline:

- Saving a C# script triggers an incremental build and, on success, a hot reload.
- The script manifest exposes serializable fields to the editor and to serialization.
- Scenes and Prefabs are assets that other assets can reference.

See [Scripting Overview](../scripting/overview.md).

## Editor script extensions

Project files under `Assets/**/Editor/**/*.cs` compile into a separate editor assembly that is only loaded in the editor process, where they can register custom Inspectors and tool windows. See [Editor Script Extensions](../architecture/editor-extensions.md).

## Next steps

- [Models, Meshes, and Textures](models-textures.md) covers concrete import settings and build behavior.
- [References, Artifacts, and Caches](references.md) covers dependency tracking and runtime data storage.
