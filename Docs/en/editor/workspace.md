# Editor Workspace

The editor is the main working surface. It shares the engine code with the game runtime, so the Scene View renders through the same pipeline rather than a separate preview implementation.

![Nullus Editor: Sponza scene, hierarchy, inspector, transform gizmo, and Asset Browser](../../Screenshots/editor.png)

## Work area layout

| Area | Purpose |
| --- | --- |
| Menu bar | File, Edit, Build, GameObjects, Resources, Window, Layout, Tools, Debugging, Help |
| Toolbar | Transform tool switching, playback controls, and common actions |
| Status bar | Active project, run state, and messages |
| Scene View | Editing viewport with gizmos, grid, and debug drawing |
| Game View | Runtime output |
| Hierarchy | Object tree of the current scene |
| Inspector | Property editing for the selection |
| Asset Browser | Browse, search, drag, and preview project assets |
| Asset View | Preview panel for material spheres, models, and Prefabs |
| Console | Logs and script diagnostics |
| Profiler / Frame Info | Performance and frame statistics |
| Project Settings | Project-level configuration |
| UI Document Designer | Visual editing of UI documents |

Panels dock, float, and reorder. The `Window` menu toggles panels individually, and `Layout` provides five presets: `Default`, `Tall`, `Wide`, `2 by 3`, and `4 Split`.

## Menus

### File

- `New Scene` (Ctrl+N), `Save Scene` (Ctrl+S), `Save Scene As...` (Ctrl+Shift+S)
- `Exit` (Alt+F4)

### Edit

- `Undo` / `Redo`, where the label includes the current action such as `Undo Move GameObject`
- `Settings...` opens project settings
- `Shortcuts...` opens the shortcut panel

### Build

- `Build game`
- `Build game and run`
- `Temporary build`

### GameObjects and Resources

`GameObjects` provides object creation entry points. `Resources` provides `Compile shaders` and `Save materials`.

### Tools

The `Tracy Profiler` submenu offers `Open Tracy Profiler` and `Open Capture Folder`. When Tracy is not compiled in, the items still exist and report that the target is unavailable.

### Debugging

- `Debug Draw`: `Enabled`, `Grid`, `Bounds`, `Cameras`, `Lights`
- `Wireframe Mode`
- `RenderDoc`: `Enabled`, `Capture Next Frame` (F11), `Open Latest Capture` (Ctrl+F11), `Open Capture Folder`, `Auto Open Replay UI`

### Help

`GitHub`, `Bug Report`, and `Feature Request` open the corresponding pages.

## Panel details

### Hierarchy

Shows the object tree with parent/child and selection state, Prefab connection coloring, and deletion of the selection. Prefab instances, overrides, missing references, and generated read-only objects are colored differently; see [Prefab Workflow](../prefab/workflow.md).

### Inspector

The Inspector is driven by reflection metadata: a field's type selects the control, whether a property has a setter decides whether it is editable, and whether a component allows multiple instances decides whether it can be added twice. Script components match custom editors by the script's own type name.

Changes enter the shared undo stack. Saving and reloading are explicit resource actions, so dragging a slider never writes to disk implicitly.

### Asset Browser

Organizes content by folder, source asset, and generated sub-asset, with search, categories, icons, drag and drop, object reference pickers, and thumbnail previews. Thumbnails are generated asynchronously under per-frame budgets and caching.

### Console

Collects build diagnostics, asset import errors, script logs, and runtime errors. The logging surface is documented in the [Scripting API Reference](../scripting/api.md).

### Profiler and Frame Info

Live data requires the matching build switches. See [Profiling and Graphics Debugging](../quality/profiling.md) for enabling them and reading the numbers.

### Icon Browser

`Window > Icon Browser` is a development window: search by id/tag/alias, preview size tiers, interaction states and Dark/Light icon themes, filter Missing/Unused, inspect texture generation and load diagnostics, and hot reload with `Reload editor icons`. The manifest under `Tools/Icons` is build-time only.

## Panels and script extensions

Project editor scripts can register custom Inspectors, tool windows, and menu items; the results appear under the `Window` and `Tools` menus. See [Editor Script Extensions](../architecture/editor-extensions.md).

## Next steps

- [Viewport Controls and Shortcuts](interaction.md) turns the common actions into muscle memory.
- [Editor and Project Settings](settings.md) configures rendering, caches, and user preferences.
