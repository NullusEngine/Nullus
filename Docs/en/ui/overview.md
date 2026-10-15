# UI Overview

Nullus UI is retained-mode: a document describes an element tree, and the runtime builds instances from it, lays them out, and produces a draw snapshot. UI can be drawn on screen or attached to objects in world space.

## Building blocks

| Component | Purpose |
| --- | --- |
| `UIDocument` | JSON-loaded document definition with the element tree and styles |
| `UIDocumentInstance` | Runtime instance holding the element tree, generation, and signal bus |
| `UIElement` | Element node with type, layout, style, text, and interaction state |
| `UIStyleSheet` | State-aware style sheet applying per-state patches |
| `UIInputRouter` | Hit testing, pointer capture, focus, and keyboard dispatch |
| `UIAnimationPlayer` | Track-based animation (currently opacity tracks) |
| `UIDataContext` | Path-addressed data binding with change subscriptions |
| `UISignalBus` | Named signals and subscriptions |
| `UILocalizationTable` | Localization table with a fallback locale |
| `UICanvasComponent` | Connects a document instance to the scene, render mode, and sorting |

## Document format

`UIDocument` is currently at schema version `1`. Each node carries:

| Field | Notes |
| --- | --- |
| `nodeGuid` | Node identity, preserved across reloads |
| `type` | Element type |
| `name` / `className` | Name and style class |
| `layout` | Layout parameters |
| `style` | Inline style |
| `text` / `passwordMode` | Text and password mode |
| `requiresRenderTarget` | Whether a dedicated render target is needed |
| `textureId` / `textureGeneration` | Texture reference and generation |
| `fontAssetId` / `fontPixelSize` | Font and size |
| `children` | Child nodes |

## Element types

`Panel`, `Image`, `NineSlice`, `Text`, `RichText`, `Button`, `Toggle`, `Radio`, `Slider`, `ProgressBar`, `TextField`, `Dropdown`, `ScrollView`, `Scrollbar`, and `ListView`.

## Layout modes

| Mode | Notes |
| --- | --- |
| `Absolute` | Absolute coordinates |
| `Stack` | Single-direction stacking |
| `Flex` | Flexible layout |
| `Grid` | Grid layout |
| `Overlay` | Overlapping children |
| `Scroll` | Scrollable container |

Layout runs in `UpdateLayout(viewportSize, dpiScale)`. The instance caches the last layout and skips recomputation when neither the viewport nor the DPI changed, while still producing a frame-scoped snapshot.

## Styling

`UIStyleSheet` supports style patches that apply per state. Available state bits are `Hovered`, `Pressed`, `Focused`, `Disabled`, `Checked`, `Selected`, and `Invalid`.

A patch can override background color, foreground color, opacity, and similar properties. The input router maintains states during interaction, and elements also store `hovered`, `checked`, `selected`, and `invalid` flags.

## Input and focus

`UIInputRouter` handles:

- `HitTest` for position-based hit testing
- `DispatchPointer` / `DispatchScroll` for pointer and wheel events
- `DispatchKey` / `DispatchTextInput` for keyboard and text input
- `Navigate` for directional focus navigation
- `CapturePointer` / `ReleasePointer` for pointer capture
- `Focus` / `ClearFocus` for focus management

Events build a parent path before dispatch, and elements manage subscriptions through `UIEventListenerHandle`.

## Animation

`UIAnimationPlayer` uses a track model: `AddOpacityTrack` adds an opacity track for one element and returns a track handle; `Play`, `Stop`, and `Tick` drive time; `RemoveTrack` and `Clear` release tracks. It uses unscaled time by default.

Only opacity tracks exist today. A fuller property and curve animation system is planned; see the [Roadmap](../roadmap/index.md).

## Data binding and signals

- `UIDataContext` stores path-to-value mappings where the value is `bool`, `int64`, `double`, `string`, a color, or a 2D vector. `Set` raises change notifications and `Subscribe` subscribes by path.
- `UISignalBus` provides named signals: `Connect` subscribes and `Emit` broadcasts and returns the notification count.

Together they decouple UI from gameplay logic: the UI never holds script objects directly, and scripts never have to walk the element tree.

## Localization

`UILocalizationTable` is at schema version `1` and carries a locale plus a fallback locale. Lookup prefers the current locale and falls back when a key is missing.

## Render modes

`UICanvasComponent` provides three render modes:

| Mode | Notes |
| --- | --- |
| `ScreenOverlay` | Screen overlay without scene depth |
| `ScreenCamera` | Bound to a camera and able to participate in depth testing |
| `World` | World-space rendering |

World-space rendering has three path values: `Auto` (choose automatically), `Direct` (draw straight to the target), and `RenderTarget` (render to a texture first). Use `RenderTarget` when other materials need to sample the result.

Other rendering-related properties:

| Property | Notes |
| --- | --- |
| `sortOrder` / `sortLayer` | Sort order and layer |
| `cameraId` | Associated camera |
| `referenceResolution` | Reference resolution |
| `dpiScaleMode` | `ConstantPixelSize` / `ScaleWithScreenSize` / `ConstantPhysicalSize` |
| `dpiMatch` | Blend factor for the scaling mode |
| `pixelsPerUnit` | World-space pixel density |
| `depthTest` / `depthWrite` | Depth test and write |
| `alwaysOnTop` | Draw on top |
| `worldSize` | World-space size |
| `updatePolicy` | `EveryFrame` / `OnDemand` / `Manual` |

`updatePolicy` decides how often the document instance updates: every frame, on demand, or manually triggered. A static interface set to `OnDemand` avoids rebuilding layout and geometry every frame.

## Document reload

`UIDocumentInstance::Reload` rebuilds the element tree and preserves compatible per-node state keyed by `nodeGuid`. Reload increments the generation, which invalidates element handles held by scripts, matching the native object hot reload semantics. Never cache element handles across a reload.

## Script access

Scripts reach the UI through `UICanvasComponent`: `GetElementHandle(name)` resolves a handle from a name, after which text, values, checked state, and focus can be read and written. Lua never holds 64-bit handles directly; name-based access resolves them internally.

## Preview in the editor

The editor provides a `UIDocumentDesigner` panel and a document design model for opening and editing UI documents inside the editor. See [UI Designer and Editor Extensions](designer.md).

## Current boundaries

- Animation covers opacity tracks only
- Rich text layout and full text wrapping are not implemented
- There is no cross-project theming or style reuse mechanism
- UI documents share the object graph with scenes and Prefabs, but the document itself is a separate JSON format

## Next steps

- [UI Designer and Editor Extensions](designer.md) edits and extends the UI inside the editor.
- [Scripting API Reference](../scripting/api.md) lists the `UICanvasComponent` members.
