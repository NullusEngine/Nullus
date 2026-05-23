# Research: ImGuizmo Transform Gizmo

## Decision: Vendor ImGuizmo under `ThirdParty/ImGuizmo`

**Rationale**: Nullus already vendors Dear ImGui under `ThirdParty/ImGui` and builds it through `ThirdParty/imgui.cmake`. ImGuizmo is a small Dear ImGui extension distributed as source with `ImGuizmo.h` and `ImGuizmo.cpp`, so vendoring it beside Dear ImGui keeps dependency ownership consistent and avoids adding a new package-manager requirement to local builds.

**Alternatives considered**:

- **vcpkg package**: Rejected for this repository because the current dependency model is source-vendored third-party libraries and no `vcpkg.json` manifest exists.
- **Git submodule**: Rejected for first integration because it adds update workflow overhead for a two-file library and the repo already vendors similar UI code directly.
- **Copy into Project/Editor**: Rejected because license/provenance and third-party update boundaries are clearer under `ThirdParty`.

## Decision: Add ImGuizmo to the existing `ImGui` CMake target or a small `ImGuizmo` target linked by Editor

**Rationale**: ImGuizmo depends directly on Dear ImGui symbols. A dedicated `ImGuizmo` static target linked to `ImGui` keeps ownership explicit and lets Editor include `ImGuizmo/ImGuizmo.h` without changing runtime UI sources. If the existing build layout makes a separate target awkward, adding `ImGuizmo.cpp` to the local ImGui-related third-party build is acceptable as long as provenance remains under `ThirdParty/ImGuizmo`.

**Alternatives considered**:

- **Header-only wrapper in Editor**: Rejected because ImGuizmo has a `.cpp` translation unit.
- **Runtime UI target dependency**: Rejected unless needed because the feature is editor-only and should not expand runtime UI responsibilities.

## Decision: Draw ImGuizmo as a Scene View ImGui overlay

**Rationale**: ImGuizmo is designed to render through Dear ImGui draw lists. Scene View already presents its render target as an ImGui image widget and tracks the image draw bounds through `AView::IsMouseWithinView()` and `AView::GetLocalViewPosition()`. Drawing the gizmo after the image widget allows the manipulator rectangle to match the viewport image without adding renderer pass complexity.

**Alternatives considered**:

- **Draw through `DebugSceneRenderer` models**: Rejected because the goal is to replace the custom coordinate-axis renderer with ImGuizmo.
- **Draw before the image widget**: Rejected because the overlay needs the final image bounds and should appear above scene content.
- **Fullscreen main viewport overlay without clipping to Scene View**: Rejected because docked/resized Scene View bounds must align with the render target.

## Decision: Use ImGuizmo interaction state to coordinate picking and camera input

**Rationale**: Existing Scene View picking relies on a color picking pass and `GizmoBehaviour` axis state. ImGuizmo provides hover/active state for its manipulator. Scene View can let ImGuizmo consume drag interactions and suppress actor selection changes while the gizmo is active, while preserving normal actor picking when the cursor is not over/using the gizmo.

**Alternatives considered**:

- **Keep old gizmo picking for ImGuizmo handles**: Rejected because ImGuizmo already handles hit testing and manipulation, and keeping two hit-test systems for the same handles risks disagreement.
- **Disable all actor picking whenever an actor is selected**: Rejected because users still need to select other actors in Scene View.

## Decision: Apply transforms through existing actor transform APIs

**Rationale**: The inspector, serialization, and scene rendering already observe actor transform components. The ImGuizmo result should be decomposed back into position, rotation, and scale and applied through the existing transform component, preserving the same data path as current editing behavior.

**Alternatives considered**:

- **Store an ImGuizmo-specific transform state**: Rejected because it would duplicate authoritative transform data.
- **Write directly to matrices only**: Rejected because existing components expose position, quaternion rotation, and scale as the authoritative editable state.

## Decision: Preserve current snapping semantics

**Rationale**: The spec requires existing snap intent and snap units. The current behavior activates snapping with Control and reads `EditorSettings::TranslationSnapUnit`, `RotationSnapUnit`, and `ScalingSnapUnit`. ImGuizmo supports optional snap values during manipulation, so the integration should map the existing modifier and units to ImGuizmo when active.

**Alternatives considered**:

- **Always snap**: Rejected because it changes established editing behavior.
- **No snapping in v1**: Rejected because snap preservation is a functional requirement.
