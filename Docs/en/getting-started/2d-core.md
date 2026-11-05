# 2D Core Authoring Entry Point

This page covers the implemented entry points of Phase 1. The actual acceptance status of building, automated tests, visible editor operations, and standalone publishing is recorded in `Docs/Plans/2D/Progress.md`; the later authoring tools still follow the feature matrix.

## Creating a Project and a Scene

Choose the **2D Core** template in the Launcher. The template contains `Assets/Scenes/Main.scene`, an orthographic Main Camera, and a Hero Sprite, along with the import defaults for new images. The Scene View reuses the existing **2D** toggle and the planar navigation.

Ordinary assets default to 100 pixels per world unit: a 64x64 image covers 0.64x0.64 world units. Changing the importer's Pixels Per Unit changes the geometry size, while Transform Scale changes the size of that instance in the scene.

The template uses a fixed step of 1/60 second with at most eight fixed updates per frame. Older projects without these project settings keep using their previous clock defaults.

## Importing Images and Creating Sprites

1. Put the image into the project's Assets folder. The 2D Core `Assets/ImportDefaults.ini` makes new images use the Single Sprite setting, while an existing `.meta` keeps its settings.
2. Choose the Sprite type in the image import settings, set PPU and Pivot, and run the reimport. An invalid PPU produces a diagnostic and keeps the last successfully published asset.
3. Expand the image in the Asset Browser and select its Sprite sub-asset. Drag the sub-asset into the Scene View or the Hierarchy to create a SpriteRenderer object.
4. An image with a single Sprite can also be dragged into the scene directly. When an image contains several Sprites, the specific sub-asset has to be selected.

Texture remains the primary asset of an image, and a Sprite uses its own persistent sub-asset identity. Renaming or reimporting an image must not change the identity of an existing Sprite. Deleting and recreating an asset produces a new identity.

Objects can also be created with **2D Objects > Sprite**, and then a Sprite is assigned to the Sprite field of the SpriteRenderer.

## Instance Properties and Saving

SpriteRenderer provides Sprite, Color (RGBA), Flip X, Flip Y, Sorting Layer, and Order in Layer. Flip mirrors the geometry around the pivot of the Sprite, Color multiplies with the image color, and Alpha affects transparent coverage and picking.

Project sorting layers are edited through the Sorting Layer dropdown. A sorting layer is stored with a persistent ID, and the name and the position of a layer can be adjusted separately. A higher Order in Layer draws later inside the same layer, and identical sorting settings keep falling back to depth sorting. The default transparent Sprite test scene tests depth and disables depth writes.

Dragging into the scene uses the existing subtree Undo operation, and property edits follow the Inspector's Undo/Redo. After the scene is saved and reopened, the asset references still point at the same logical Sprite. Prefabs and scenes use the same component and persistent reference serialization.

The Camera/Advanced controls of the Scene View show the submission, visibility, culling, batching, draw call, and split reasons of a Sprite, and they can switch the CPU batching path. The camera's geometric culling switches also apply to Sprites.

## Running and Verifying a Release

Use the Play/Stop controls of the editor to inspect the scene. A shipping Player uses the existing Build Profiles and NullusBuild with the target platform artifacts; runtime assets are never created from source images or from Sprite shader authoring files.

During Play you can switch to the Scene View and drag objects, and you can edit Inspector properties. Stopping restores the scene state from before Play, including edits that had not been saved. A failing script host restore records a diagnostic, and the scene can still be stopped and edited, and the editor can still be closed.

Local development verification can use:

```powershell
NullusBuild.exe build --project "D:\Projects\SpriteDemo\SpriteDemo.nullus" --output "D:\Projects\SpriteDemo\Dist" --player "<the Player directory of the current build>" --no-sign
```

`--no-sign` is for unsigned local verification. Publishing configuration and signing keep using the existing build pipeline.

For an unmodified 2D Core template build, run:

```powershell
Tools\2D\ValidateTemplatePlayer.ps1 -ProductDirectory "<product directory>" -EvidenceDirectory "<a new evidence directory>" -HideSpriteAuthoringSources
```

This Windows/DX12 check stores the actually rendered PNGs, the logs, and an `evidence.json`. It checks one visible Sprite draw, the script host initialization, the native module paths, and that source images are not published with the content package, and it hides the Sprite shader authoring files during the check. It does not replace the real mouse acceptance of the editor or the verification on other platforms.

Existing scenes can pass `-ExpectedSubmittedSprites`, `-ExpectedVisibleSprites`, and `-ExpectedStartScene` to avoid rewriting the scene for acceptance. Visible Sprites still have to share one batch.

## Sprite Prefabs

Dragging a Sprite from the Hierarchy into an Asset Browser folder creates a connected Prefab. After changing instance properties, right-clicking a property offers `Apply Override` or `Revert Override`, and the revert operation supports Undo/Redo. Saving the scene records the instances and the overrides, and reopening it keeps the connection.

The packaging stage expands the runtime scene from the target platform Prefab artifacts and keeps instance properties, the hierarchy, and the references inside the scene. The authoring scene keeps the Prefab connection. Missing artifacts or a dependency cycle stops the packaging with a diagnostic.

## Fixed-Seed Scenes

```powershell
python Tools\2D\GenerateSpriteBenchmark.py --project "D:\Projects\SpriteDemo\SpriteDemo.nullus" --count 10000 --seed 12345
```

The generator creates `Assets/Scenes/SpriteBenchmark.scene` and a configuration record, keeps Main.scene, and refuses to overwrite an existing benchmark by default. It replaces all base Sprites of the source scene so that the output count matches `--count`, and it records the hash of the source scene. A source Sprite must be a root object that only has a Transform and a SpriteRenderer; a scene with Prefab connections, extra components, or references to external objects produces a diagnostic. The configuration record does not contain measured performance. Comparing the two batching paths requires the same output resolution, camera, backend, thread mode, warm-up, and sampling interval.

An existing product can be verified with the batching feature by running:

```powershell
Tools\2D\ValidateSpriteBenchmark.ps1 -ProductDirectory "<product directory>" -BenchmarkScene "<existing SpriteBenchmark.scene>" -EvidenceDirectory "<a new evidence directory>"
```

This script copies the product into a temporary directory, replaces the start scene of the copy, and checks 10,000 Sprites, one batch, one draw, and the actual GPU readback. It keeps the original product and the project. The recorded total time covers startup, loading, drawing, readback, and exit, and it does not represent a stable frame time or a performance percentile.

The entry points and the acceptance of the Sprite Editor, Atlas, Pixel Perfect, Physics2D, Tilemap, frame-by-frame animation, 2D lighting, and the advanced authoring capabilities are completed in the later phases; see the full feature matrix in `Docs/Plans/2D/FeatureMatrix.md`.
