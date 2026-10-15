# Viewport Controls and Shortcuts

This page covers Scene View navigation, transform gizmos, picking, undo, and playback control. Every shortcut can be rebound through `Edit > Shortcuts...`.

## Scene View navigation

The Scene View uses an editor camera that is independent of any `CameraComponent` in the scene. Moving it never modifies the game camera, and entering Play uses the scene camera for the Game View.

- Drag with the right mouse button to orbit.
- Use the scroll wheel to zoom.
- Use the middle mouse button to pan.

When a shot must be reproduced exactly, the editor accepts startup arguments that force the Scene View camera pose and step it forward for a fixed number of frames, which is useful for batch validation and regression comparison. See the [Editor command line](../reference/layout-cli.md#editor-command-line).

## Transform gizmos

The toolbar exposes three transform modes, registered as `editor.icon.toolbar.move`, `editor.icon.toolbar.rotate`, and `editor.icon.toolbar.scale`:

| Mode | Effect |
| --- | --- |
| Move | Translate the selection |
| Rotate | Rotate the selection |
| Scale | Scale the selection |

Gizmo edits are written to the object's `TransformComponent` and recorded in the undo stack. Rotation is stored as a quaternion, so gimbal drift does not accumulate.

## Picking

- Click an object in the Scene View to select it.
- Picking uses hit proxies rather than reading back the rendered image, so selection is decoupled from render visibility.
- Clicking an entry in the Hierarchy updates the same selection state.

Picking and gizmos share one selection set, and the Inspector always shows the current selection.

## Undo and redo

| Command | Default binding |
| --- | --- |
| `edit.undo` | Ctrl+Z |
| `edit.redo` | Ctrl+Y, or Ctrl+Shift+Z |
| `edit.delete-selected-gameobject` | Delete the selection |
| `edit.delete-selected-gameobject-hierarchy` | Delete the selection and its hierarchy |

The undo service records operation names, which is why the `Edit` menu shows labels such as `Undo Move GameObject`. Undo covers scene object creation, deletion, and modification, Inspector field edits, Prefab overrides, and asset file operations.

## Playback control

| Command | Default binding |
| --- | --- |
| `editor.play` | F5 |
| `editor.stop` | Escape |

`editor.stop` is only available while running. Entering Play drives scripts through `Awake`, `OnEnable`, `Start`, and `Update`; stopping runs `OnDisable` and `OnDestroy`. Details are in [Lifecycle and Activation](../scene/lifecycle.md).

## Graphics debugging shortcuts

| Command | Default binding | Notes |
| --- | --- | --- |
| `debug.renderdoc.capture-next-frame` | F11 | Capture the next frame |
| `debug.renderdoc.open-latest-capture` | Ctrl+F11 | Open the most recent capture |

Both require RenderDoc to be available. Configuration and command line usage are in [Profiling and Graphics Debugging](../quality/profiling.md).

## Display toggles

`Debugging > Debug Draw` controls helper drawing: `Grid`, `Bounds`, `Cameras`, and `Lights`. `Wireframe Mode` switches wireframe display. These toggles affect the editor viewport only and do not change shipped rendering.

## Customizing shortcuts

`Edit > Shortcuts...` opens the shortcut panel, which is driven by the command registry:

- Every command has a stable ID, a display name, a category, and a context.
- You can set a binding, add alternate bindings, clear a binding, or mark a command as unassigned.
- Conflicts are validated on submit; duplicate global bindings are rejected.
- The profile is versioned and stored, with the default profile named `Default`.

## Next steps

- [Editor and Project Settings](settings.md) tunes viewport caching, rendering, and debug options.
- [Debugging and Testing](../quality/debugging.md) turns editor behavior into reproducible checks.
