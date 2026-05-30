# Data Model: Scene View Camera State Machine

## Overview

This feature introduces an explicit interaction model for Scene View camera control. The model is transient editor state only; it is not serialized and does not affect runtime game data.

## Entities

### 1. Camera Interaction State

Represents the active Scene View camera mode.

**Fields**

- `kind`: one of `Neutral`, `Blocked`, `Fly`, `Pan`, `Orbit`
- `ownsCursor`: whether the state currently owns camera-side cursor shape/capture
- `ownsInfiniteWrap`: whether the state requires infinite cursor wrapping
- `suppressesMouseDelta`: whether the state should ignore early mouse deltas after capture
- `enteredFromActiveButtons`: whether the state was entered because a button press established a live navigation gesture

**Rules**

- `Neutral` never owns cursor or infinite wrap.
- `Blocked` never owns cursor or infinite wrap and forces cleanup when entered.
- `Fly` owns cursor and infinite wrap.
- `Pan` owns cursor and infinite wrap.
- `Orbit` owns cursor and infinite wrap.

### 2. Interaction Input Snapshot

Represents the per-frame inputs used to evaluate state transitions.

**Fields**

- `sceneInputAllowed`: whether Scene View camera input is currently allowed by panel-level rules
- `cameraInputBlocked`: whether a hard editor block condition is active
- `windowFocused`: whether the editor window still has focus
- `mouseOverView`: whether the pointer is inside Scene View interaction bounds
- `leftPressed`, `middlePressed`, `rightPressed`: edge-triggered mouse button press events
- `leftDown`, `middleDown`, `rightDown`: current mouse button held state
- `altDown`: whether the orbit modifier is active
- `wantTextInput`: whether editor/UI text entry is active

**Rules**

- `wantTextInput` or `cameraInputBlocked` force `Blocked`.
- `middleDown + altDown` transitions toward `Orbit` when scene input is allowed.
- `middleDown + !altDown` transitions toward `Pan` when scene input is allowed.
- `rightDown` transitions toward `Fly` when scene input is allowed.
- Loss of focus or button ownership mismatch transitions back to `Neutral` or `Blocked`.

### 3. Transition Result

Represents the state-machine output for a frame or event boundary.

**Fields**

- `previousState`
- `nextState`
- `enterActions`
- `exitActions`
- `cursorShape`: desired cursor, if the next state owns cursor
- `shouldResetMouseInteraction`: whether transient button/mouse capture state must be cleared

**Rules**

- Entering `Blocked` always sets `shouldResetMouseInteraction = true`.
- Leaving any cursor-owning state emits cleanup actions exactly once.
- Re-evaluating the same state with unchanged ownership emits no cursor-change action.

### 4. Camera Motion Execution Mode

Represents which motion algorithm `CameraController` should execute after state transition.

**Variants**

- `None`
- `FlyLookAndMove`
- `PanCamera`
- `OrbitTarget`

**Rules**

- `Neutral` and `Blocked` map to `None`.
- `Fly` maps to `FlyLookAndMove`.
- `Pan` maps to `PanCamera`.
- `Orbit` maps to `OrbitTarget`.

## Relationships

- `SceneView` produces a high-level `sceneInputAllowed` / blocking context.
- `CameraController` gathers mouse/button/input data and feeds an `Interaction Input Snapshot` into the state machine.
- The `Camera Interaction State` determines cursor ownership and movement execution mode.
- The `Transition Result` drives one-time side effects on `UIManager` and `Window`.

## State Transition Summary

| From | Trigger | To | Required Side Effects |
|------|---------|----|-----------------------|
| `Neutral` | text input or modal block | `Blocked` | ensure camera cursor/capture are cleared |
| `Neutral` | right mouse gesture | `Fly` | claim cursor control, enable infinite wrap, suppress initial delta |
| `Neutral` | middle mouse without orbit modifier | `Pan` | claim cursor control, enable infinite wrap, suppress initial delta |
| `Neutral` | middle mouse with orbit modifier | `Orbit` | claim cursor control, enable infinite wrap, suppress initial delta |
| `Fly` / `Pan` / `Orbit` | button released or focus lost | `Neutral` | release cursor control, disable wrap, clear transient flags |
| `Fly` / `Pan` / `Orbit` | text input or modal block begins | `Blocked` | release cursor control, disable wrap, clear transient flags |
| `Pan` | orbit modifier becomes active while gesture remains valid | `Orbit` | switch state without duplicate cleanup/acquire churn |
| `Orbit` | orbit modifier becomes inactive while gesture remains valid | `Pan` | switch state without duplicate cleanup/acquire churn |
| `Blocked` | block removed and no gesture active | `Neutral` | no cursor ownership restored automatically |

## Validation Implications

- Pure unit tests should verify state transitions and side-effect flags without requiring real window or UI backends.
- Integration tests should verify Scene View produces the expected block conditions for text entry and modal UI.
- Manual validation should confirm the visible cursor changes only on state entry/exit and not continuously while a state remains active.
