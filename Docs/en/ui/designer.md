# UI Designer and Editor Extensions

The editor is both a consumer and a stress test of the UI system. This page covers the visual editing entry point for UI documents and how project scripts extend the editor interface.

## UI Document Designer

### Create and edit a screen interface

Prerequisites: an editor built with `NLS_ENABLE_GAME_UI=ON` and an open, saved, runnable scene.

1. Right-click in a project directory in the Asset Browser, choose `Create > UI Document`, and name it `HUD`. The editor creates a `.ui` file and opens UI Designer. Double-click the file to reopen it, or find the panel under `Window > UI Designer`.
2. Search or click a control in the Palette to create it as a child of the selected element. You can also drag a control onto the canvas to create and position it at the drop point.
3. Drag an element on the canvas to change its `offsetMin`, and drag any of the eight handles to resize. Grid snapping and alignment guides are toggled in the toolbar. When the parent uses `Stack`, `Flex` or `Grid`, position is owned by the layout and the handles report that direct manipulation is unavailable.
4. The Inspector edits the full layout record (anchors, pivot, offsets, size, min/max size, margin, padding, spacing, aspect ratio, flexGrow, grid columns, clipping), style (background/text color, opacity, visible/enabled/interactive) and content (text, password, font size). Fields are driven by an editor-side reflected view model, so they share the scene Inspector's property widgets and metadata; mixed fields across a multi-selection show `—` and edits apply to every selected element. The 3x3 anchor presets re-anchor without moving the current rect.
5. The Hierarchy supports inline rename with `F2` / double-click, drag reorder and reparent (dropping into the dragged subtree is rejected), a row context menu (add, duplicate, delete, show/hide, ordering) and per-row visibility toggles.
6. Shortcuts: `Del` delete, `Ctrl+D` duplicate, `Ctrl+C/V` copy/paste, arrow keys nudge 1px (`Shift` 10px), `F` frame all, `Shift+F` frame selection, `Ctrl+±` zoom, `Ctrl+0` 100%, `Ctrl+S` save, `Esc` cancel the active gesture.
7. Saving writes the `.ui` document. Documents with warnings can be saved; validation errors disable saving. Clicking a row in Problems selects and frames the element. Closing, switching or reloading a dirty document asks Save / Discard / Cancel, and an external file change shows a Reload / Keep Local banner.
8. Create an empty scene object named `HUD` and add `UICanvasComponent` through the Inspector's component selector. Drag the `.ui` file onto `Document` and set `Render Mode` to `Screen Overlay`.
9. Save the scene and enter Play. The Game View shows the same layout as the designer canvas. For script control, give elements unique names and look them up through the Canvas API.

The canvas preview uses the runtime `UIDocument` / `UIElement` layout engine: the instance is rebuilt when the document revision changes, and once the font atlas is uploaded it draws the same glyph quads as the Game View. Texture and font asset pickers and nine-slice borders remain follow-up work.

Designer state (grid, snapping, guides, wireframe, hidden elements) lives under the `Editor/UI Designer` settings category.

The editor provides a `UIDocumentDesigner` panel for opening and editing UI documents:

- Opening a document builds the element tree following the `UIDocument` definition.
- Selecting a node exposes layout, style, text, and control properties for editing.
- The design model and the runtime instance are separate, so design-time changes never mutate a running `UIDocumentInstance`.
- Saving writes the document JSON; refreshing re-instantiates and increments the generation.

Design-model validation shares the same parsing code as the runtime, so a document that parses in the designer parses at runtime. A parse failure reports a path-qualified diagnostic instead of silently dropping nodes.

The editor's own interface overlay publishes through the same UI runtime: the editor draws into a dedicated overlay that the render side composites, rather than manipulating render resources from business code.

## Extending the editor from scripts

Project editor scripts can register custom Inspectors, tool windows, and menu items.

### Directory convention

```text
<Project>/Assets/**/Editor/**/*.cs   ->   GameScriptsEditor.dll (editor process only)
<Project>/Assets/**/*.cs             ->   GameScripts.dll (editor and player builds)
```

Editor scripts may reference `Nullus.Editor` and derive from `ComponentEditor` or `EditorWindow`.

### Custom Inspector

```csharp
using Nullus;
using Nullus.Editor;

namespace Nullus.GameScriptsEditor;

[CustomEditor(typeof(UICanvasComponent))]
[CanEditMultipleObjects]
public sealed class UiCanvasInspector : ComponentEditor
{
    public override void OnInspectorGUI()
    {
        EditorGUILayout.HelpBox("Drawn from script", MessageType.Info);
        EditorGUILayout.PropertyField(serializedObject.FindProperty("document"));

        if (EditorGUILayout.Button("Reset pixels per unit"))
        {
            var pixelsPerUnit = serializedObject.FindProperty("pixelsPerUnit");
            if (pixelsPerUnit is not null)
            {
                pixelsPerUnit.floatValue = 100.0f;
                ApplyModifiedProperties();
            }
        }
    }
}
```

### Custom tool window

```csharp
using Nullus.Editor;

namespace Nullus.GameScriptsEditor;

public sealed class AssetAuditWindow : EditorWindow
{
    [EditorMenuItem("Window/Asset Audit")]
    public static void ShowWindow()
    {
        var window = GetWindow<AssetAuditWindow>() ?? new AssetAuditWindow();
        window.Open();
    }

    public override void OnGUI()
    {
        EditorGUILayout.Label("Hello from a project editor script");
    }
}
```

## Target type matching

Custom editors target one of two kinds:

| Target | How it is matched |
| --- | --- |
| Reflected engine component | Use the generated type carrying `NativeTypeName`, for example `typeof(UICanvasComponent)` |
| Project script component | Use a `Nullus.Behaviour` subclass, matched by the script's own type name |

All script components share the same native `ScriptComponent` reflected type, so script component editors match on the script's own full C# type name. One script's editor never affects another script component. Base-class matching across script components is not supported yet.

## Data access constraints

Scripts cannot write component objects directly. All reads and writes go through `serializedObject` / `SerializedProperty` and commit through `ApplyModifiedProperties()`, which is what makes them share undo, Prefab override, and multi-object editing semantics with the built-in editors.

## Available controls

`EditorGUILayout` currently provides:

- `Label`, `Space`
- `Toggle`, `IntField`, `FloatField`, `Slider`
- `TextField`, `Button`, `HelpBox`
- `PropertyField`
- `BeginHorizontal` / `EndHorizontal`, `BeginVertical` / `EndVertical`

## Registration and priority

- A script editor for the same target defaults to a higher priority than the built-in editor, which is how it can override one.
- A duplicate combination of target type, inheritance rule, and priority fails registration with an explicit error.
- Menu items may only be attached under the `Window` and `Tools` top-level menus.

## Error isolation

Exceptions thrown by a script or editor are caught: that Inspector falls back to generic drawing with an error message, and after enough consecutive failures the script editor is disabled without affecting other panels. Recompiling scripts rescans the registry and rebuilds open editors and windows.

## Current boundaries

- v1 does not expose script `CustomPropertyDrawer` registration.
- There is no arbitrary ImGui passthrough; the control set is fixed to the controlled API above.

Extending what scripts can do means extending the controlled control set and the registration model first, then updating this page.

## Next steps

- [Editor Script Extensions](../architecture/editor-extensions.md) covers the module-level view.
- [UI Overview](overview.md) returns to the runtime semantics.
