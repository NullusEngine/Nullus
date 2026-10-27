# Physics Status and Plans

Physics is **partial**: a PhysX 5.6.1 scene query world and static colliders serve native consumers such as particle collision. Dynamic rigid bodies, complete collision events, and character controllers remain unimplemented.

## Static colliders

1. Add **Physics / Collider** to a scene object.
2. Select Box, Sphere, or Capsule and configure Half Extents, Radius, Half Height, Layers, and Trigger.
3. Run the scene. Query geometry synchronizes before component updates; moving, disabling, or deleting objects should update or release their query bodies.

Trigger is a query-filter attribute here, not an Enter/Stay/Exit event API. Half Extents is half the box size. This component publishes static query geometry and does not move under gravity.

## Native query API

`PhysicsWorld` registers boxes, spheres, capsules, and triangle meshes, and updates or removes bodies. Acquire `Read(QueryFilter)` to use `Raycast`, `RaycastAll`, `SweepSphere`, and `OverlapSphere`. Read views retain synchronized access; release them before mutating the world on the same thread.

Scenes own independent query worlds. [Particle Systems](../audio-vfx/particles.md) connect to their owning scene by default. Collider shape, dimensions, layers, and Trigger settings are reflected/scriptable; general queries remain native APIs rather than a complete C# / Lua physics query library.

## Dependencies and build

CMake pins PhysX 5.6.1 source. Set `NLS_PHYSX_SOURCE_DIR` for an offline checkout containing `physx/`. SDK sources and build products live in the build directory. The CPU SDK is separate from RHI-driven GPU particle simulation.

`Tests/Unit/PhysicsWorldTests.cpp` and `PhysicsSceneTests.cpp` cover world queries and scene synchronization. Test definitions or a successful build on one platform do not establish all-platform acceptance.

## Missing capabilities

- Dynamic rigid-body stepping, contact solving, joints, and a complete friction/restitution authoring workflow.
- Trigger events, collision callbacks, character controllers, and physics debug visualization.
- A complete scripting query library, substepping, and presentation interpolation.

Project `gravity` and fixed-step callbacks do not imply integrated dynamics. [Animation](../animation/roadmap.md) supplies poses and presentation, not a kinematic or dynamic controller.

## Related pages

- [Lifecycle and Activation](../scene/lifecycle.md) explains fixed-step callbacks.
- [Feature Status Matrix](../roadmap/index.md) separates capabilities from validation.
