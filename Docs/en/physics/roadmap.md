# Physics Status and Plans

Physics is **partial**: PhysX 5.6.1 now supports independent body/shape ownership, compounds, basic dynamics, queries and aggregated collision events. 2D uses an independent Box2D 3.2.0 world with basic bodies, colliders and native queries. Complete assets, controllers, constraints, scripting coverage and editor acceptance remain unfinished.

## Static colliders

1. Add **Physics / Collider** to a scene object.
2. Select Box, Sphere, Capsule or Cylinder and configure Half Extents, Radius, Half Height, Center, Layers, Collision Mask, Friction, Restitution and Trigger.
3. Run the scene. Query geometry synchronizes before component updates; moving, disabling, or deleting objects should update or release their query bodies.

Half Extents is half the box size. A 3D Collider attaches to its own or nearest ancestor Rigidbody, stopping at a nested body. Without a Rigidbody it creates a static body. Each shape retains its Collider identity. Rigidbody supports mass, gravity, linear/angular velocity, damping, CCD, motion locks and sleep settings; updating these properties preserves the runtime body identity. A 2D Trigger affects simulation and queries; full 2D Enter/Stay/Exit script events remain pending.

3D Rigidbody Constraints is a mask: position XYZ uses 1/2/4, rotation XYZ uses 8/16/32, and 63 freezes all axes. Auto Mass Properties defaults on and derives center/inertia from actual shapes. Disable it to use Center Of Mass, Inertia Tensor and Mass Frame Rotation. Manual values use meters and kg*m² in the body frame; Transform scale changes geometry without scaling these values. Added fields support serialization, cloning and missing-field defaults; visible Inspector interaction remains unverified.

Start Awake controls initial state and Sleep Threshold controls automatic sleeping. C++ WakeUp, Sleep, ApplyTorque and AddForceAtPosition join existing forces at the safe fixed-step boundary, in order and once. IsSleeping reads the last submitted state. Force, torque and positioned force support Force, Acceleration, Impulse and VelocityChange. Shape rebuilding preserves current sleep state and does not erase an awakened body's velocity by reapplying its initial configuration. Added reflected metadata does not imply complete C# / Lua facade coverage.

## 2D scene components

After rebuilding Editor, the registered component entries are **Physics 2D / Rigidbody** and **Physics 2D / Collider**. Colliders support Box, Circle, Capsule, Polygon, Edge and Chain. Size is the full size in meters, Center is local, and Local Rotation is authored in degrees. Capsule Segment Half Height excludes the rounded ends.

Polygon uses a local meter-space Vertices array for one simple contour. Either
winding is accepted, with 3-256 vertices and no holes or repeated closing vertex.
Concave contours split into convex pieces while preserving empty regions; they
share one Body and Collider identity. Edge uses exactly two vertices and collides
on both sides. Chain uses 2-257 open vertices or 3-256 when Loop is enabled. Its
front points right along the contour, with neighboring ghost vertices suppressing
internal-edge contacts. Chain requires a static, non-trigger body. Mirroring a
chain component preserves its authored front side.

Invalid self-intersections, repeated/too-close vertices, unsafe numeric values or
unsupported changes report LastError and retain the complete previous group.
Contour/ghost spacing starts at 0.01 meters. Queries return each actual piece and
its `QueryHit2D.subShapeId`; the builders number parts from one per geometry
revision. `GetShapeHandles()` exposes all members and `IsQueryReady()` validates
them. Vertices and Loop save and clone as configuration. Automated tests cover
real mass/impulse, concave queries, one-sided collision, seam sliding, transformed
children and full-group lifetime. Polygon drawing tools, multi-path Composite,
holes, Tilemap integration and visible Inspector acceptance remain pending.

A Rigidbody collects colliders from its object and descendants up to another Rigidbody. Standalone colliders create static bodies. One object cannot be driven by both 2D and 3D rigidbodies. The 2D solver writes XY position and Z rotation while retaining Z position. The 2D content module owns PPU conversion from sprite pixels to world units.

Edit mode synchronizes query geometry without simulating. Play uses independent scene worlds; physics owns dynamic pose and moving a parent does not teleport its dynamic child. Tilt, degenerate transforms and shear produce component diagnostics. Native and scene lifecycle tests passed on Windows Debug; visible editor interaction remains unverified.

Scene physics settings now save and clone gravity in both dimensions, Box2D solver subdivisions, worker counts and optional render interpolation. Existing scenes without settings retain the default values. The native `Scene::SetPhysicsSettings` API applies gravity and subdivisions to current worlds; worker changes take effect when a world is recreated. A dedicated editor settings control remains pending.

With `interpolate=true` (default off), rendering samples the last two fixed poses without changing authoritative Transforms or queries. Mesh/light/skybox proxies, Sprites, particle view transforms and game cameras use this presentation path. Visual children inherit their nearest active body's presentation pose; nested bodies remain independent. Teleport, restore, shape rebuild, pause and single Step clear history. Visible smooth-motion and GPU acceptance are tracked separately from automated proxy/camera tests.

## Native query API

`PhysicsWorld` retains the old registration APIs and adds CreateBody plus separate Attach APIs for boxes, spheres, capsules, SDK custom cylinders, convex meshes, static triangle meshes and sampled heightfields. Dynamic concave meshes are rejected. Heightfield input is row-major meter data along X/Z, quantized with an automatic 16-bit height scale; this is not a full Terrain authoring tool. Acquire `Read(QueryFilter)` for `Raycast`, `RaycastAll`, `SweepSphere` and `OverlapSphere`. Release read views before mutating the same world on that thread.

Scenes own independent query worlds. [Particle Systems](../audio-vfx/particles.md) connect to their owning scene by default. Collider shape, dimensions, layers, and Trigger settings are reflected/scriptable; general queries remain native APIs rather than a complete C# / Lua physics query library.

3D read views also provide SweepShape, SweepShapeAll, SweepShapeNonAlloc, OverlapShape and OverlapShapeNonAlloc. QueryGeometry3D describes a sphere, box, Y-axis capsule/cylinder or an existing convex shape in the same world. Borrowing a shape avoids cooking and automatically ignores its owning body. GetShapeWorldPose supplies the full pose including native local alignment. Static triangle meshes and heightfields remain targets and explicitly return UnsupportedGeometry as source volumes. Status distinguishes a valid miss, invalid input and invalid handles. Buffers report count, total and truncated; an empty buffer counts capacity. Primitive sweeps default to the SDK precise path, while convex meshes and cylinders retain SDK GJK tolerance. initialOverlap marks a sweep starting inside geometry; check hasPointAndNormal before using contact position/normal.

`PhysicsWorld2D` provides safe body/shape handles, compound bodies, mass/density, force modes, CCD, motion locks, sleeping, Raycast, RaycastAll, CircleCast and OverlapCircle. Native angles and angular velocities use radians. Overlaps return identity; check `QueryHit2D.hasPointAndNormal` before reading contact geometry.

Both dimensions offer `BeginMutationBatch()` so queries cannot observe a partially rebuilt collider set. Release ReadAccess on the same thread before mutation.
Native `PhysicsCommandBuffer` records body creation, shape attachment/replacement, dynamics settings and release for a later FIFO commit. The queue owns geometry input, and result tickets validate buffer identity and generation. Component creation/removal and motion commands issued during contact callbacks wait for a safe boundary; `Scene::TrySyncTransforms()` returns false within that callback scope. Callbacks may keep their query views open; direct native mutation still requires releasing those views. Stop clears unconsumed motion commands and releases objects in retained old worlds.
Native world gravity and Box2D subdivisions follow the same commit boundary. Calling `Scene::Stop()` in a callback queues a stop; safe synchronization then stops the scene and cancels remaining catch-up ticks. SceneManager refuses immediate loading, unloading or sandbox discard in that callback. Use `LoadAndPlayDelayed` and run `Update()` after the callback returns. Automated checks cover Prefab object-graph instances in both dimensions, collider reparenting, independent nested dynamic poses and repeated replacement of saved runtime scenes. Complete Prefab asset workflows and mouse-driven editor acceptance remain separate.
Both also offer RaycastNonAlloc and overlap NonAlloc methods with caller-owned arrays. Results report written count, total hits and truncated. Ray buffers keep the nearest hits; an empty array counts required capacity. Both native worlds can capture/restore state for unchanged world, shape topology and dynamics configuration. This excludes solver caches and strict cross-platform deterministic replay.

3D events aggregate compound subshapes by Collider owner pair, with at most one Stay per step, including sleeping contacts. Events carry step index, identities and up to four real contact samples; check hasContactData before using samples. Complete script collision-data bindings remain pending.

## Physics resources and offline cooking

`.physmat` materials and `.collisionmesh` resources use existing `.meta` GUIDs, PPtr references, ArtifactDB, native artifact containers and runtime manifests. Shared Cook and editor import/reimport call the same compiler. Both collider dimensions expose `physicsMaterial`; 3D additionally exposes `collisionMesh` and the `COLLISION_MESH` shape value. Colliders without resources retain their inline properties.

Sources are JSON version 1. Materials configure `staticFriction`, `dynamicFriction`, `restitution`, `friction2D` and `restitution2D` independently. The average/minimum/multiply/maximum `frictionCombine` and `restitutionCombine` modes apply to 3D; 2D retains Box2D mixing. The minimal material is `{"version":1}`.

Collision sources support `kind: "convex"` point clouds, `"triangle"` vertices/indices and `"heightfield"` sampled meter grids. Optional `bakeScale`, welding parameters and convex vertex limit affect cooking. Mirrored meshes have their triangle winding corrected during Cook; heightfields require positive scale. Runtime loads cooked streams and shares native geometry across worlds using the same resource. Triangle meshes/heightfields remain static and non-trigger, while convex meshes support dynamic simulation. Mesh Transform mirrors require baking. Invalid data and unsupported transforms report diagnostics while retaining the previous valid shape.

Cache inputs include source GUID/content, baking parameters, metadata, backend version and target. Cancellation or stale source data retains the last valid artifact. Native collision Cook requires a matching host OS/CPU and explicitly rejects cross-platform/architecture requests; platform acceptance remains separate. Loaders verify SHA-256, type, GUID/fileID, version and runtime target. Scenes save references only.

Automated gates cover actual heightfield support, both-dimension material rebound, cache invalidation, resource reload and scene cloning. A runtime-manifest test deletes source/meta files and removes ArtifactDB before loading and simulating cooked collision. Dedicated authoring UI, drag/drop, Undo/Redo, visible interaction and complete Player Cook acceptance remain pending.

## Reusable compound Shapes

`.physshape` groups 1-256 primitive or cooked-mesh parts under one Collider.
Select Shape Asset (value 5) and assign its typed Shape Resource reference.
The JSON source uses version 1, with positive, unique part IDs:

```json
{"version":1,"shapes":[
  {"id":7,"type":"box","halfExtents":[1,0.25,0.5],"position":[-2,0,0]},
  {"id":19,"type":"sphere","radius":0.5,"position":[2,0,0]}
]}
```

Types are box, sphere, capsule, cylinder and mesh. Optional rotation is an
X/Y/Z/W quaternion; scale and position use the Collider's local meter frame.
Mesh parts specify `"mesh":{"guid":"..."}` for a `.collisionmesh` resource.
Any part can override its material with `"material":{"guid":"..."}` for
`.physmat`. All parts use the Collider's layer, mask and Trigger. Spheres require
uniform scale and capsules/cylinders equal radial X/Z scale; shear and unbaked
mesh reflection produce diagnostics. Triangle/heightfield parts remain static
and non-trigger.

Import, Cook and runtime manifests follow the existing asset pipeline, including
transitive mesh/material dependencies. Reload replaces the entire group at safe
synchronization while preserving its Body. Invalid geometry or unresolved children
retain the previous group. Native queries return `subShapeId`; contact events
identify a representative pair with `firstSubShapeId` and `secondSubShapeId`.
Part IDs survive source reordering; successful replacement invalidates old native
shape handles. Ordinary body rotation preserves the group. Resource authoring UI,
drag/drop, Undo/Redo and visible editor acceptance remain pending.

## Dependencies and build

CMake pins PhysX 5.6.1 and Box2D 3.2.0 source archives and checksums and requires CMake 3.22 or newer. Offline overrides are `NLS_PHYSX_SOURCE_DIR` (containing `physx/`) and `NLS_BOX2D_SOURCE_DIR` (the Box2D source root). SDK sources and binaries stay in the build directory.

Physics suites now run in `NullusUnitTests`; use `--gtest_filter="Physics*.*"` for a focused run.
They cover both backends, the fixed-step clock, shared TaskGraph, query atomicity, scenes, serialization, and cleanup. Passing these tests does not establish other-platform or interactive acceptance.

## Missing capabilities

- Complete 3D resource authoring UI, automatic collision generation from models, constraints and controllers.
- Multi-path 2D Composite and holes, tilemap collision, complete shape-query scripting and one-way platforms.
- Complete script contact-data APIs, 2D event scripting, Physics Debugger, Gizmos and visible interpolation acceptance.
- Ragdoll, vehicles, fracture, buoyancy and optional CUDA soft bodies/cloth.

The default interval is 1/60 second, with at most 8 ticks per frame and a 0.25-second debt cap. Box2D solver subdivisions are independent and default to 4. Playback single stepping executes exactly one fixed tick, including at TimeScale zero, and preserves the ordinary fractional remainder. Headless tests validate this behavior; actual editor clicking remains unverified. [Animation](../animation/roadmap.md) supplies poses and presentation rather than a dedicated character controller.

## Related pages

- [Lifecycle and Activation](../scene/lifecycle.md) explains fixed-step callbacks.
- [Feature Status Matrix](../roadmap/index.md) separates capabilities from validation.
