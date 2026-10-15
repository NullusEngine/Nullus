# Cameras, Lights, and Output

Cameras decide what is seen; lights decide how it is lit. Both are components, adjustable in the editor and drivable from scripts.

## Camera

Scripting surface of `CameraComponent`:

| Member | Notes |
| --- | --- |
| `GetFov()` / `SetFov(float)` | Perspective field of view |
| `GetNear()` / `SetNear(float)` | Near clip plane |
| `GetFar()` / `SetFar(float)` | Far clip plane |
| `GetSize()` / `SetSize(float)` | Orthographic size |
| `GetProjectionMode()` / `SetProjectionMode(...)` | Projection mode |
| `GetClearColor()` / `SetClearColor(Vector3&)` | Clear color |

Switching between perspective and orthographic keeps the same clipping and visibility logic; `Size` only applies in orthographic mode.

The Scene View uses its own editor camera and never reads a `CameraComponent` from the scene. Entering Play makes the Game View use the scene camera, so "the angle I set up in the editor" and "the runtime image" are two different things.

## Lights

Scripting surface of `LightComponent`:

| Member | Notes |
| --- | --- |
| `GetLightType()` / `SetLightType(...)` | Light type |
| `GetColor()` / `SetColor(Vector3&)` | Color |
| `GetIntensity()` / `SetIntensity(float)` | Intensity |
| `GetRange()` / `SetRange(float)` | Influence range |
| `GetOuterCutoff()` / `SetOuterCutoff(float)` | Outer cone angle |

Lights enter clustered lighting according to their type. Directional lights relate to the main light shadow, while point and spot lights use `Range` and `OuterCutoff` to bound their influence volume.

## Output

Rendering output falls into three categories:

| Output | Notes |
| --- | --- |
| Swapchain | The final image presented to the window |
| Offscreen scene target | The scene renders to a texture and then goes through an output batch |
| Editor view target | Separate targets for the Scene View and Game View |

The forward path renders to an offscreen scene target before going through a separate swapchain output batch. That gives readbacks, screenshots, and later passes a stable input instead of depending on the current swapchain image.

Swapchain image readback starts before Present, which avoids the synchronization hazard of reading the same image after it was presented.

## Clear color and skybox

The camera clear color provides the background when no skybox is present. With a skybox, the skybox pass writes the background and the clear color becomes a fallback.

The skybox is its own pass drawn before transparent objects, so transparency blends correctly over the background.

## Cameras and UI

`UICanvasComponent` interacts with cameras through its render mode:

| Mode | Notes |
| --- | --- |
| `ScreenOverlay` | Drawn over the screen without scene depth |
| `ScreenCamera` | Bound to a specific camera and able to participate in depth testing |
| `World` | Rendered in world space, using either the direct or render-target path |

World-space path selection, depth test, and depth write are documented in [UI Overview](../ui/overview.md).

## Debug helpers

`Debugging > Debug Draw` overlays helper shapes for cameras and lights so you can check orientation, range, and cone angles. These drawings exist only in the editor viewport.

## Next steps

- [Materials and ShaderLab](materials.md) gives meshes their look.
- [Graphics Backends and Large-Scene Features](backends.md) covers how backend capability gates features.
