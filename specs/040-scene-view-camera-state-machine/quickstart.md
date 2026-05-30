# Quickstart: Scene View Camera State Machine

## Goal

Validate that Scene View camera interaction has moved to explicit state transitions and no longer reassigns cursors every frame.

## Prerequisites

- Windows editor build environment configured for Nullus
- Existing repository dependencies installed
- Feature branch `040-scene-view-camera-state-machine` checked out

## Build

```powershell
& 'F:/Microsoft Visual Studio/2022/MSBuild/Current/Bin/MSBuild.exe' 'Build/Tests/Unit/NullusUnitTests.vcxproj' /p:Configuration=Debug /p:Platform=x64 /m:1 /nologo /v:minimal
```

## Targeted Automated Validation

Run the new and related interaction regression tests:

```powershell
& 'D:/VSProject/Nullus/Build/bin/Debug/NullusUnitTests.exe' --gtest_filter=SceneViewCameraInteractionStateMachineTests.*:PanelWindowHookTests.SceneViewBlocksCameraInputDuringTextEntryEvenInsideView:ReflectedPropertyDrawerTests.DragWidgetsEnableClickToInputBeforeDrag
```

Run broader Scene View and reflected input regression coverage:

```powershell
& 'D:/VSProject/Nullus/Build/bin/Debug/NullusUnitTests.exe' --gtest_filter=PanelWindowHookTests.SceneViewInteractionHintsDoNotForceThreadedRenderingDrain:PanelWindowHookTests.SceneViewRetainsExplicitReadbackAndResizeDrains:ReflectedPropertyDrawerTests.*
```

## Manual Validation

### 1. Text Entry Stability

1. Launch the editor in the Debug validation environment.
2. Open Scene View and focus a transform or numeric input that can coexist with Scene View hover/focus.
3. Click into the input so it enters text-edit mode.
4. Move the pointer across Scene View bounds while the input remains active.
5. Confirm the text-edit cursor remains stable and no Scene View navigation cursor appears.

### 2. Fly Mode

1. Activate the standard right-mouse fly gesture in Scene View.
2. Confirm the fly cursor appears once when the gesture starts.
3. Keep flying for several seconds and confirm the cursor remains stable without visible churn.
4. Release the gesture and confirm the cursor returns to the shape that was active before the gesture exactly once.

### 3. Pan And Orbit

1. Start middle-mouse pan and confirm the pan cursor appears once.
2. Release middle mouse and confirm the pre-gesture cursor shape is restored.
3. Start middle-mouse with the orbit modifier and confirm the orbit cursor appears.
4. Toggle modifier changes during the same gesture if supported by the implementation and confirm transitions remain stable and deterministic.

### 4. Forced Reset Cases

1. Begin a camera interaction.
2. While the interaction is active, trigger a blocking state such as text entry or shortcut settings.
3. Confirm camera movement stops, camera cursor ownership is released, and the next visible cursor belongs to the blocking UI.

## Completion Criteria

- Targeted automated tests pass.
- Existing related Scene View/input regression tests still pass.
- Manual validation confirms:
  - no text-cursor flicker during Scene View-adjacent text entry
  - fly/pan/orbit cursors change only on state transitions
  - mouse release restores the cursor shape that was active before camera or drag cursor ownership
  - blocked states cancel camera ownership cleanly
