# Physics Status and Plans

This page states the current state of physics and interaction, the gaps, and the dependencies.

## Current status: planned

### What exists

- The project file has a `gravity` key for global gravity acceleration.
- The component update loop includes the fixed-step phase `OnFixedUpdate`, so scripts can run logic on a fixed step.
- `Time` exposes `fixedDeltaTime` (default `0.02`) and `maximumDeltaTime` (default `0.1`) plus `fixedFrameCount` for deterministic logic.

### What is missing

- No rigid bodies, colliders, triggers, raycasts, or shape queries
- No collision detection or solver, no friction or restitution
- No character controller
- No physics debug visualization or physics materials

`gravity` is currently a configuration value, not an input consumed by a solver.

## Goals

| Capability | Goal |
| --- | --- |
| Collision shapes | Box, sphere, capsule, convex hull, and triangle mesh |
| Rigid bodies | Mass, damping, kinematic and dynamic modes |
| Detection and solving | Contact generation, constraint solving, friction, restitution |
| Queries | Raycasts, sweeps, and overlap queries |
| Triggers | Enter, stay, and exit events without a physical response |
| Scene query interface | Cooperation with the scene hierarchy and spatial index |
| Character controller | Capsule-driven movement, ground detection, steps, and slopes |
| Debug visualization | Show colliders and contacts in the editor viewport |

## Dependencies and order

1. Physics needs a **third-party solver** or a self-written kernel with an explicit boundary. Choosing the solver is an architecture decision and must not be left to whoever implements it.
2. Fixed-step updates already exist, so stepping can hook into that phase; substeps and interpolation are what still need defining.
3. Physics objects must cooperate with the spatial index so queries never degrade into full-scene scans; see [Visibility and Occlusion](../large-scenes/visibility-occlusion.md).
4. Physics components are new reflected types that must be wired into serialization and the Inspector per [Reflection and Serialization](../architecture/reflection-serialization.md).
5. Once physics API enters the scripting manifest, both C# and Lua bindings are generated from that one manifest; see the [Scripting API Reference](../scripting/api.md).
6. A kinematic character controller can land before a full solver exists, but it shares shapes and queries with physics, and duplicating that infrastructure creates two competing sets of semantics.

## Relationship to animation

Dynamic character control depends on a physics solver; kinematic control first needs shapes, sweeps, and collision queries. Animation state machines coordinate presentation and are not prerequisites for kinematic controllers. The two implementation paths can interleave; see [Animation Status and Plans](../animation/roadmap.md).

## Not a commitment

This page describes goals and dependencies, not a schedule. When implementation status changes, the [Feature Status Matrix](../roadmap/index.md) and the source are authoritative.

## Next steps

- [Lifecycle and Activation](../scene/lifecycle.md) for fixed-step callback timing.
- [Feature Status Matrix](../roadmap/index.md) for the cross-domain overview.
