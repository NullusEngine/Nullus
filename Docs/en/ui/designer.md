# UI Designer and Editor Extensions

The editor is both a consumer and a stress test of the UI system. This page covers the visual editing entry point for UI documents and how project scripts extend the editor interface.

## UI Document Designer

### Create a screen interface

Prerequisites: an editor built with `NLS_ENABLE_GAME_UI=ON` and an open, saved, runnable scene.

1. Right-click in a project directory in the Asset Browser, choose `Create > UI Document`, and name it `HUD`. The editor creates a `.ui` file and opens UI Designer. Double-click the file to reopen it, or find the panel under `Window > UI Designer`.
2. Select the root in the designer Hierarchy and click `Panel` under `Controls` to add a child. Set `Name` to `StatusPanel`, `Layout > Size` to `(320, 100)`, and `Style > Background` to an opaque color.
3. Confirm that the panel appears in the preview. Click `Save` and check for `Saved`. `Undo` / `Redo` revert or restore design operations; `Reload` reads the document from disk.
4. Create an empty scene object named `HUD` and add `UICanvasComponent` through the Inspector's component selector. Drag the `.ui` file onto `Document` and set `Render Mode` to `Screen Overlay`.
5. Save the scene and enter Play. A colored panel should appear over the Game View. For text, add Text/Button, fill in `Text`, and configure a project font resource. For script control, give elements unique names and look them up through the Canvas API.

The designer exposes names, text, layout mode/size/spacing, and background color. Controls do not yet cover every schema field; some styling and resource configuration still require JSON edits. Successful parsing establishes format validity; fonts, input, and final output still need target-backend runtime checks.

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
