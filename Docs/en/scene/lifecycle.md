# Lifecycle and Activation

Components expose a set of virtual functions that the scene calls at defined moments. Getting the order and the gating conditions right is the prerequisite for understanding why a callback did or did not fire.

## Callback order

Entering Play, for each active object:

```text
OnCreate  (once, when the component is created and bound to its host)
OnAwake   (once, when the object first enters the running state)
OnEnable  (each time the object or component becomes enabled)
OnStart   (once, on first entering the running state after Awake)
```

Every frame:

```text
OnUpdate(deltaTime)      regular update
OnFixedUpdate(deltaTime) fixed-step update
OnLateUpdate(deltaTime)  after the regular update
```

Leaving Play:

```text
OnDisable  (when the object or component becomes disabled)
OnDestroy  (when the object or component is destroyed)
```

## Gating conditions

- `OnAwake` and `OnStart` only run when the object is active in the hierarchy.
- Once an object has awoken or started, re-activating it does not repeat those callbacks.
- `OnEnable` runs on every transition from inactive to active and can therefore run several times per session.
- A component that was previously active runs `OnDisable` before `OnDestroy`.
- A component is destroyed once; repeated destruction requests do not repeat the callbacks.

## Activation state

Activation has two levels:

| Level | Query | Notes |
| --- | --- | --- |
| Self | `IsSelfActive()` / `GetActive()` | Looks only at this object's own toggle |
| Hierarchy | `IsActive()` | True only when this object and every parent are active |

A component takes effect when it is enabled and its host GameObject is active in the hierarchy, that is `IsActiveAndEnabled()`. Disabling a parent disables the components of the whole subtree, and re-enabling the parent re-triggers `OnEnable` following the same rules.

`SetActive` only changes the object's own toggle and never modifies its parents.

## Enabling and disabling components

`SetEnabled(bool)` only fires callbacks on a real state change:

- From active to inactive: `OnDisable`
- From inactive to active: `OnEnable`
- Once the host has been destroyed: the call is ignored

This guarantees that toggling a flag never produces duplicate `OnEnable` calls.

## Scene switching

`SceneManager` owns loading the active scene and switching between scenes. When a scene is loaded at runtime, the editor first deactivates the callbacks of the current objects and then activates the new scene objects, so two scenes never drive the same frame.

Editor-transient objects such as gizmos and helper drawing are not saved and must not be relied on by scripts.

## Common questions

**A callback never fires**

Check whether the host object is active in the hierarchy. A disabled parent deactivates the whole subtree.

**`OnEnable` fires more than once**

That is intended. Logic that must run exactly once belongs in `OnAwake` or `OnStart`.

**`OnAwake` ran but `OnStart` did not**

The object was deactivated after `OnAwake`. `OnStart` runs when the object is activated again.

## Callbacks from scripts

C# and Lua script components use the same callback names. On the C# side they are methods on `Nullus.Behaviour`; on the Lua side they are members of the returned module table. See [Scripting Overview](../scripting/overview.md).

## Next steps

- [Scenes, GameObjects, and Components](objects.md) returns to the object model.
- [Scripting Overview](../scripting/overview.md) uses these callbacks from script.
