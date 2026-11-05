# Nullus Physics API

This document lists implemented APIs only; planned capability is tracked in
PhysicsImplementationStatus.md.

## Fixed-step clock

`NLS::Physics::FixedStepClock::Configure(FixedStepSettings, std::string&)`
validates settings atomically. `Advance(double scaledDeltaTime)` returns step
count, interval, interpolation alpha and discarded time. The input is already
scaled; the clock never applies TimeScale. `Reset()` clears accumulated and
dropped time. Separate clocks are independent. Manual stepping bypasses this
clock and leaves its remainder unchanged.

## Existing 3D compatibility API

`PhysicsWorld::Create`, AddBox/AddSphere/AddCapsule/AddTriangleMesh, Remove,
SetTransform/GetTransform, SetVelocity/GetVelocity, AddForce, Step,
TakeContactEvents and Read remain the native compatibility API. BodyHandle
includes world/index/generation. Release ReadAccess before mutating the world
on the same thread. ReadAccess offers Raycast, RaycastAll, SweepSphere and
OverlapSphere. Primitive sizes are world units and box extents are half sizes.

`CreateBody` and `AttachBox/AttachSphere/AttachCapsule/AttachConvexMesh/AttachTriangleMesh`
separate body ownership from shape ownership. ShapeHandle also validates the
world and generation. Shapes supply independent local pose, layer/mask,
friction, restitution and stable owner identity. Query hits return the actual
shape and owner; legacy OverlapSphere still returns unique body identities.
Triangle meshes remain static-only. The raw Attach APIs cook synchronously;
offline cooked resources use the separate pipeline below.

AttachCylinder uses the SDK's Y-axis custom convex geometry and keeps its
callback object alive with the native shape. AttachHeightField accepts real
row-major float heights in meters; rows advance along X and columns along Z.
Samples are quantized to signed 16-bit with an automatically chosen height
scale. Heightfields and triangle meshes are static-only and cannot be triggers.

BodySettings adds damping, angular velocity, six motion locks and CCD. ApplyForce,
AddForceAtPosition and AddTorque accept Force, Acceleration, Impulse and
VelocityChange. PhysX consumes these accumulators during the next step.
With autoMassProperties=true (default), attached shapes determine the center of
mass and inertia. With false, centerOfMass, massFrameRotation and inertiaTensor
specify the local mass frame and its principal moments. These use physical body
units (meters and kg*m^2), independent of Transform scale. Values must be finite,
mass and moments must have finite positive reciprocals, and mass-frame rotation
must be valid. Removing the last auto-mass shape resets its center and mass frame
to identity and gives each principal moment the mass value.
startAwake sets the initial dynamic state; sleepThreshold defaults to 0.005.
Shape changes preserve the body's current sleeping state rather than reapplying
startAwake and erasing motion after a previous wake.
SetKinematicTarget applies target motion. CaptureState/RestoreState preserve
pose, velocity, sleep and step index for unchanged world topology. Restore
rejects foreign, recycled, duplicate or changed-topology snapshots before
changing any body. It resets event history; it does not restore solver caches
or promise deterministic replay. WorldSettings can share a started TaskGraph;
the default dispatcher runs inline. Step waits for accepted backend tasks.

UpdateDynamics updates mass, damping and motion flags without replacing body
or shape handles. It preserves backend force accumulators. Snapshot restoration
requires unchanged topology and dynamics configuration revisions.

## 3D shape queries

ReadAccess adds SweepShape, SweepShapeAll, SweepShapeNonAlloc, OverlapShape and
OverlapShapeNonAlloc. QueryGeometry3D supports Sphere, Box, Y-axis Capsule,
Y-axis Cylinder and Shape. Box dimensions are half-extents; capsule halfHeight
excludes the round ends. Cylinder queries use the SDK's exact convex-core cylinder;
body cylinders retain the SDK custom-geometry callbacks.

Shape borrows a valid native shape in the same world, including its cooked convex
mesh, without cooking or allocating a new native shape during the read. Its entire
owning body is excluded automatically. GetShapeWorldPose returns the composed body
and native shape-local pose, including capsule axis alignment. Supply that full
geometry pose to the query; local offsets are not applied a second time. ReadAccess
retains the world and cooked/custom geometry until released, even after its world
wrapper is destroyed. Triangle meshes and heightfields remain query targets but
return UnsupportedGeometry when used as moving or overlap source geometry.

ShapeQueryStatus distinguishes Success (including a valid miss), InvalidInput,
InvalidShape and UnsupportedGeometry. Buffer variants return both status and
QueryBufferResult (count, total, truncated). They retain the nearest hits in the
caller-owned span, count beyond its capacity, and support an empty span. All-hit
and overlap queries return individual shapes; legacy OverlapSphere keeps its
body-deduplicated behavior. Invalid or missed nearest queries leave the supplied
hit unchanged. Legacy layers, trigger and static/dynamic filters remain in force.

preferPreciseSweep defaults to true for the SDK's supported sphere/box/capsule
path. Convex meshes and cylinders use the SDK GJK path and its geometric tolerance.
Sweeps reject distances above the SDK limit of 1e8 meters rather than silently
clamping, as well as invalid or unrepresentable bounds. QueryHit.initialOverlap
marks a sweep starting inside geometry; its contact position is unavailable.
Check hasPointAndNormal before using geometry data. Overlap results provide
identity only, and unavailable SDK position/normal fields are never read.

3D ContactEvent identifies both shapes, both owner tokens and the step index.
Compound subshapes with the same owner are aggregated into Enter/Stay/Exit once
per owner pair per step, including sleeping contacts. Up to four actual native
contact samples are included when hasContactData is true. Sleeping steps and
Exit events do not fabricate current contact geometry.

`BeginMutationBatch()` groups changes into one query-visible commit in either
dimension. It is nestable on the same thread. Release all ReadAccess views
before entering a mutation on that thread; do not call a world from its solver
callbacks. Do not hold read views across mutations of other worlds.

## Physics resources and offline cooking

PhysicsMaterial and CollisionMeshAsset are Engine Object resources referenced by
the existing PPtr system. `.physmat` and `.collisionmesh` source files use JSON
version 1 and the normal `.meta` GUID/importer settings. ArtifactType/AssetType
values are appended, preserving existing numeric identities. Shared AssetCooker
and the editor AssetDatabaseFacade use CompilePhysicsAsset; ArtifactWriter wraps
the result in NativeArtifactContainer and publishes through the existing ArtifactDB.
RuntimeManifestBuilder includes these resources in its normal dependency closure.
There is no parallel physics resource database.

A minimal material source is `{"version":1}`. Optional fields are staticFriction,
dynamicFriction, restitution, friction2D, restitution2D, frictionCombine and
restitutionCombine. Defaults retain 3D friction/restitution 0.5 and 2D friction
0.5/restitution 0. Friction ranges from 0 to 10000 and restitution from 0 to 1.
Combine strings average/minimum/multiply/maximum apply to the PhysX material;
Box2D uses its existing coefficient mixing. Unknown/duplicate fields, unsupported
versions, invalid numbers and excessive nesting fail without replacing valid data.

For example, a convex collision source is:

```json
{"version":1,"kind":"convex","vertices":[[0,0,0],[2,0,0],[0,2,0],[0,0,2]]}
```

Kinds are convex (a noncoplanar point cloud), triangle (vertices plus complete
indices), and heightfield (rows, columns, row-major heights, rowSpacing and
columnSpacing). Data uses meters. bakeScale defaults to [1,1,1]; meshes accept
mirrors and reverse triangle winding when needed. Heightfields require positive
bake scale. weldVertices, weldTolerance and convexVertexLimit configure native
cooking; the convex limit is 8-255. Source coordinates and scaled bounds are
limited to 1e7, vertex/height arrays to 1,048,576 entries and cooked data to 64 MiB.
The source snapshot is limited to 64 MiB. Unsupported inputs fail before cooking.

CookedCollisionMesh::Cook produces immutable backend data. Serialize/Deserialize
validate format, byte limits, checksum and compatibility with the exact PhysX
version, cooking schema, OS, CPU and endianness. AttachCollisionMesh loads a
native mesh lazily and reuses it across worlds sharing that immutable resource.
Runtime attachment and queries never cook. Optional positive mesh scale applies
without rebuilding cooked bytes; unsupported heightfield scales reject before
SDK geometry operations. Triangle/heightfield attachment is static and non-trigger.

Cache inputs include source GUID, content hash, bake parameters, meta settings,
backend/cooking version and target. Matching warm cooks reuse the existing artifact.
SDK/platform changes invalidate it. Native collision cooking requires the matching
host OS/CPU; cross-platform or cross-architecture requests fail explicitly. Material
JSON is portable. Source/meta revision and cancellation checks guard publication,
including final write callbacks; failed/stale results retain previous manifests.

LoadPhysicsMaterial and LoadCollisionMeshAsset resolve stable GUID/fileID references
through the active runtime manifest, or the authoring ArtifactDB. They verify full
artifact SHA-256, metadata identity/schema and runtime target before publishing
new snapshots. Cache context changes trigger revalidation. A failed reload retains
the last data snapshot. Cache identity includes persistent file type as well as
GUID/fileID, so distinct reference types cannot alias one object. PhysicsAssetReadScope holds one lazy authoring database
snapshot for scene synchronization, then makes new committed revisions visible on
the next synchronization. Runtime lookup uses its manifest index.

Both collider dimensions expose a physicsMaterial PPtr; 3D additionally exposes
collisionMesh and the appended COLLISION_MESH shape value (4). Bare colliders keep
their existing inline coefficients. Resource edits rebuild shapes at safe commits
without replacing bodies. Mesh Transform mirrors must be baked into source data;
unsupported shear/mirrors and missing resources report diagnostics while retaining
the last valid shape. Scene serialization/cloning persists references, never native
meshes, pending commands or runtime handles. Dedicated asset editors and full script
facade coverage remain separate work.

## Native 2D runtime

`PhysicsWorld2D::Create(WorldSettings2D, error)` owns a real Box2D world with
independent gravity and solver subdivisions. Settings can supply a shared,
started engine TaskGraph, or request an owned worker pool. The default runs
inline. Destroy worlds before shutting down a supplied task graph.

`CreateBody` creates Static, Dynamic or Kinematic bodies. `AddBox`, `AddCircle`,
`AddCapsule`, and `AddConvexPolygon` attach independent, generation-checked
shapes to one body. Native Box sizes are half-extents. Capsule half-height is
the segment half-length and excludes its rounded ends. The convex API rejects
concave input rather than silently filling its missing regions.

Shape settings apply offset, local rotation, density, friction, restitution,
filter bits, sensors and owner tokens. `autoMass=false` maintains explicit mass
while updating the inertia for attached shapes. Force, Acceleration, Impulse
and VelocityChange have real mass-dependent or mass-independent semantics.
World rotations and angular velocities use radians; distances use meters.

`GetState`, `SetTransform`, `SetVelocity`, `AddForce`, `AddForceAtPosition`,
`AddTorque`, `SetAwake`, `SetGravity` and `GetStatistics` operate on actual
backend state. Teleport preserves velocity by default and can clear it.
CaptureState/RestoreState also preserve pose, velocities, sleep and step index
for the same world and unchanged topology/configuration. This is not solver
cache rollback. UpdateDynamics preserves the body identity when properties change.

2D ReadAccess offers Raycast, RaycastAll, CircleCast and OverlapCircle with
trigger, layer and ignored-body filters. Hits return body/shape identity and
the supplied owner token. `hasPointAndNormal` distinguishes cast geometry
from overlap-only identity results; overlap results do not fabricate contact
positions. All-hit results are ordered by distance.

Both read APIs offer RaycastNonAlloc and an overlap NonAlloc method using
caller-owned spans. QueryBufferResult reports count, total and truncated.
Ray buffers retain the nearest hits in distance order; an empty span counts
the required capacity. Bounded overlaps return shape identities, whereas the
legacy 3D OverlapSphere vector returns unique body identities.

## Structural command buffer

Construct `PhysicsCommandBuffer` with a shared PhysicsWorld or PhysicsWorld2D.
The buffer holds a weak world reference; it does not extend world lifetime.
`CreateBody`, `AttachShape`, `ReplaceShape`, `UpdateDynamics`, `RemoveBody` and
`RemoveShape` record values without calling either SDK. CollisionGeometry3D/2D
own their mesh/polygon/height arrays. Pass an existing typed handle or a ticket
from an earlier command as PhysicsBodyReference/PhysicsShapeReference.
`SetGravity(Vector3)` / `SetGravity(Vector2)` and `SetSolverSubsteps(uint32_t)`
record world configuration. Subdivision commands apply only to 2D. Wrong dimensions,
nonfinite gravity and invalid subdivision counts return explicit failed results.

`Commit()` processes one FIFO snapshot in one mutation batch and returns
processed/applied/skipped/failed/pending counts plus busy. Concurrent producers
can record while it executes; those records wait for a subsequent commit.
Commit executes on the owning thread outside solving/callbacks, after releasing
all retained read views. An overlapping consumer returns busy rather than
executing the same snapshot twice. No scene objects or scripts are invoked.

`GetResult(ticket)` returns status, error and resulting typed handles. Tickets
validate buffer identity, reset generation and slot. Dependencies must refer to
earlier Applied results. Wrong dimensions, expired worlds and backend rejection
fail explicitly; stale/foreign removals are skipped. Shape replacement attaches
the new geometry before removing the previous shape, retaining valid geometry
when cooking/validation fails. `GetPendingCount`, `GetResultCount` and
`IsCommitting` expose queue state. Applied results remain available until Reset.
`Reset()` cancels pending work, frees result slots and invalidates old tickets;
it refuses while committing and does not destroy previously applied objects.

## Scene components

RigidbodyComponent exposes Constraints (position XYZ bits 1/2/4, rotation XYZ bits
8/16/32), Auto Mass Properties, Center Of Mass, Inertia Tensor, Mass Frame Rotation,
Start Awake and Sleep Threshold through reflection and serialization. Existing
scenes without these fields receive the compatibility defaults. Invalid values
leave the previous configuration intact. Updates preserve body/shape identity;
manual mass properties survive collider rebuilding.

WakeUp, Sleep, IsSleeping, ApplyTorque and AddForceAtPosition share the same runtime
as existing AddForce/AddImpulse/AddTorque. AddForceWithMode, AddTorqueWithMode and
AddForceAtPositionWithMode expose modes as 0=Force, 1=Acceleration, 2=Impulse,
3=VelocityChange. Invalid modes are ignored. Forces, torques, sleep and wake are
queued in order and consumed once at a safe fixed-step boundary. IsSleeping reads
the most recently submitted state, excluding kinematic bodies; it does not predict
pending commands. Reflected metadata does not establish complete C#/Lua facade
coverage; full multilingual physics API parity remains phase 8 work.

`Scene::GetPhysicsSettings/SetPhysicsSettings` reads or atomically validates
ScenePhysicsSettings. It contains gravity3D, gravity2D, solverSubsteps2D (1-64),
workerCount3D/2D (0-31) and interpolate (default false). Zero workers selects
inline execution. Gravity/subdivision changes retain current body/shape handles;
`HasPendingPhysicsWorkerSettings()` reports worker changes awaiting world recreation.
Scene serialization stores version 1 settings in the additive `physics` root
property. Missing properties default; invalid types, ranges, versions, duplicate
keys and nonfinite values reject loading before scene allocation.
When called under PhysicsMutationDeferral, SetPhysicsSettings updates validated
authoring configuration and queues gravity/subdivision writes. Native queries retain
their submitted state until safe synchronization. Outside deferral, those writes
commit before SetPhysicsSettings returns. Worker changes still require recreation.
That immediate world commit also drains earlier queued structural releases in
FIFO order; callers must release their query views before invoking it.

`SetPhysicsInterpolationAlpha` publishes a finite clamped presentation alpha;
RuntimePlaySession calls it after the frame callbacks. `ResetPhysicsInterpolation`
clears history at the current pose. `CapturePhysicsPresentationTransform(object)`
returns copied world matrix/position/rotation/scale and usesPhysicsPose. Disabled
interpolation and edit mode return the authoring transform. Dynamic bodies stay
independent of their parents, while visual descendants inherit their controller's
presentation delta. Rendering snapshots consume this result; it is never applied
to the scene Transform. BodyState/BodyState2D include stepIndex and poseRevision.
SetTransform, shape rebuild and RestoreState change poseRevision; a normal solver
step changes only stepIndex. These fields are runtime metadata and are not saved
with components or restored as historical revision tokens.

Both worlds also expose `GetRevision()`, an O(1) conservative mutation token.
It advances after exclusive native-world access; failed writes may advance it,
and read-only state capture and queries do not. It is distinct from topology,
per-body poseRevision and simulation stepIndex. Rendering observes this token in
both cache layers to notice native teleports/restores even while Play is paused.

`CameraComponent::GetRenderCamera()` captures the presentation pose for render
submission; `GetCamera()` retains its original authoring Transform and mutation
semantics. The render camera is transient, used on the game thread, and must be
frozen into the existing frame package before use by rendering workers.

`Rigidbody2DComponent` and `PhysicsCollider2DComponent` use reflected authoring
configuration and the scene-owned world. Collider component Size is full size
in meters; Local Rotation is authored in degrees. Rigidbody angular velocity
and Teleport rotation use radians. Force/velocity/teleport commands are applied
at fixed-step boundaries after shape assembly. A parent Rigidbody collects its
descendant colliders up to a nested Rigidbody; a standalone Collider is static.
Scene.SyncTransforms commits current authoring changes without stepping.

`Scene::TrySyncTransforms()` reports false inside contact dispatch or recursive
synchronization. `SyncTransforms()` retains its void compatibility entry point.
Outside those scopes, callers must release query views before synchronization.
`Scene::PhysicsMutationDeferral` provides a nestable game-thread scope for code
that changes components while retaining a query view; scope exit does not commit.
The scene uses it automatically for native and script contact listeners.
`GetPendingPhysicsCommandCount()` counts queued releases and world settings, excluding
per-rigidbody motion commands. QueuePhysicsRemoval overloads are the component
lifetime bridge for both dimensions; releases are drained at safe synchronization,
Stop and destruction. Both worlds provide IsValid for body and shape handles.

`Scene::Stop()` under physics deferral requests a stop without releasing bodies.
`IsPhysicsStopPending()` exposes that request. The next safe synchronization
performs lifecycle callbacks, drains releases and clears pending body motion.
RuntimePlaySession consumes that stop after contact dispatch, cancels remaining
catch-up ticks and skips Update/animation/LateUpdate on the stopped scene.
`RuntimePlaySession::End()` fails explicitly while its scene is in a physics
callback/synchronization; call it after that scope returns.

SceneManager immediate LoadScene, empty-scene creation, unload and sandbox
begin/discard refuse to replace a scene during physics deferral/synchronization.
Use `LoadAndPlayDelayed(path, absolute)` to queue a request; `Update()` keeps the
request pending during deferral and executes it at a safe boundary. A load listener
may queue the next load without the outer Update erasing it. Object-graph Prefab
instances create independent runtime handles; pending force commands are not copied.

The existing RigidbodyComponent now owns one 3D body independently of its
colliders. PhysicsColliderComponent keeps its legacy type and half-size field,
adds center/mask/friction/restitution, and attaches to the nearest enabled 3D
body. Its native shape identity is distinct from that body. A nested disabled
body stops ancestor collection. Both dimensions queue motion/force commands
for fixed-step boundaries. Dynamic world-pose writeback requires positive
body scale and uniformly scaled ancestors; unsupported transforms report a
component diagnostic. Symmetric static 3D primitives can bake mirrored scale.
The 3D/2D scene bridge passed Windows Debug automated integration validation.
Visible editor interaction and other platforms remain unverified.

Existing Rigidbody and Collider script bindings are a limited subset; their
presence does not establish full multilingual physics API coverage.

## Complex 2D geometry

`BuildPolygon2D(vertices, settings, output, error)` accepts one simple contour
with 3..256 meter-space vertices, either winding, without holes or a repeated
closing vertex. It rejects self-intersections, nonadjacent touching, repeated or
too-close vertices and adjacent backtracking. Collinear forward vertices are
removed. Valid concave contours are ear-clipped, then neighboring triangles are
merged into convex pieces of at most eight vertices. SDK hull validation precedes
polygon creation. Failure leaves output unchanged; no convex-hull approximation
fills the missing regions of a concave contour.

`BuildChain2D(vertices, ChainGeometrySettings2D, settings, output, error)` accepts
2..257 open vertices or 3..256 loop vertices. Each segment collides on its right
side. Neighboring vertices become ghost points, with optional explicit endpoint
ghosts for open-chain seams; otherwise endpoints extrapolate their adjacent edge.
Loops include their closing edge. Chains require static, non-trigger bodies.
`AddSegment` creates a two-sided edge. `AddChainSegment` takes four points in
previous/first/second/next order and creates an individually owned SDK segment.
This supports per-segment safe lifetime; the SDK documents greater memory use
than its bulk chain API. Chain/Tilemap performance acceptance remains separate.

Native input centers/vertices are bounded to +/-1e6 meters after local pose;
contour vertices and chain ghost neighbors must be at least 0.01 meters apart.
Primitive half-extents/radii range from 1e-6 to 1e6 meters, with capsule center separation
at least 0.01. Density is zero or 1e-6..1e4; friction is 0..1e4. Unsupported numeric
input fails before native creation, retaining the prior group on replacement.

`ReplaceShapes2D` records a group of PhysicsShapeDescription2D and previous shape
references. It shares the atomic publication/rollback behavior of the 3D group
command. Results expose `shapes2D`, with `shape2D` retaining the first-member ticket
convention. Wrong-body/stale/duplicate previous handles and duplicate positive
(owner, part ID) pairs reject. Empty input clears only the specified previous set.
Queries see complete groups and caller-buffer totals count individual pieces.
`QueryHit2D::subShapeId` identifies the returned part; legacy inline primitives
use zero. Builders number pieces/segments from one for that geometry revision;
editing the contour can change decomposition and part order.

PhysicsCollider2DComponent appends POLYGON=3, EDGE=4 and CHAIN=5, preserving the
old values 0..2. `vertices` is a reflected NLS::Array<Vector2>; `loop` controls
chain closure. Points use local meters, with Center and Local Rotation composed
through the actual signed Transform axes. Polygon/edge/chain points can bake
nonuniform local rotation without replacing the contour by a bounding shape.
Mirrored chain components reverse segment order to retain their authored front
side. Parent tilt/shear still rejects at the Transform boundary.

`GetShapeHandles()` borrows all members, `GetShapeHandle()` returns the first,
and `IsQueryReady()` validates every member. Invalid geometry or transforms keep
the previous group with LastError. External member deletion causes a complete
rebuild at safe sync. Normal rigidbody motion preserves the group; disable,
reparent, deferred destruction and Stop release all members. Scene/Prefab text
saves vertices/loop, never handles. Holes, Boolean Composite merging, Tilemap
content integration and complete array-query scripting remain pending.

Scene final cleanup is independent of one-shot gameplay OnDestroy. Edit-mode
query geometry can be recreated after Stop without another gameplay callback.
When an already-destroyed runtime component loses its owner, the scene uses an
explicit ReleaseToScene context to queue native cleanup before a destructor can
fall back to direct world mutation. This applies to both rigidbody and collider
families, including individual component removal. A normally live object keeps
its original callback/cleanup order; gameplay callbacks are not replayed.

## Reusable 3D Shape resources

`.physshape` uses JSON version 1 and between 1 and 256 parts. Each part has a
positive, unique uint32 `id` and a `type`: box, sphere, capsule, cylinder or mesh.
Box `halfExtents`, sphere `radius`, and capsule/cylinder `radius`/`halfHeight`
retain meter units and default to 0.5. Capsule half height excludes rounded ends.
Optional `position`, `rotation` (X/Y/Z/W quaternion), and signed `scale` are in the
Collider's local frame. Defaults are zero position, identity rotation and unit
scale. The Collider Center is added before applying its Transform.

```json
{"version":1,"shapes":[
  {"id":7,"type":"box","halfExtents":[1,0.25,0.5],"position":[-2,0,0]},
  {"id":19,"type":"sphere","radius":0.5,"position":[2,0,0]}
]}
```

Mesh parts require `mesh: {"guid":"..."}` referencing a `.collisionmesh` main
subasset. Every part can override the Collider material with
`material: {"guid":"..."}` referencing `.physmat`. Optional `fileID` must match
the GUID's canonical `collision-mesh:main` or `physics-material:main` identifier;
optional `type` is an asset FileType (default 2). No world handles enter artifacts.
Parts are canonicalized by ID, and referenced artifacts enter the existing
runtime dependency closure. Source compilation validates syntax/configuration;
dependency existence is checked by packaging and runtime resolution.

Select `SHAPE_ASSET` (appended enum value 5) and assign the typed `shapeResource`
PPtr on PhysicsColliderComponent. `GetShapeHandles()` borrows the current group;
`GetShapeHandle()` retains its compatibility meaning as the first shape. Copy
the handles if they must outlive synchronization. `IsQueryReady()` checks every
member. Resource or child mesh/material reload replaces the whole group while
preserving the owning Body. Invalid references, shear, mesh reflection and
unsupported dynamic/trigger concavity retain the previous group with LastError.
Resource spheres require uniform scale; capsules/cylinders require matching X/Z
radial scale. Legacy inline primitive scaling remains compatible.

Native `ReplaceShapes(body, previous, descriptions)` records owned inputs,
returns all handles in `PhysicsCommandResult::shapes` and exposes the first as
`shape` for ticket compatibility. Empty descriptions clear only the supplied
previous group. Duplicate previous handles, wrong-body handles and duplicate
positive (owner, part ID) pairs in the new group are rejected. Direct single-shape
replacement checks the previous shape's Body in both dimensions.

`QueryHit::subShapeId` and ContactEvent's `firstSubShapeId`/`secondSubShapeId`
identify resource parts; zero preserves legacy inline identity. Owner-pair events
carry a representative shape pair and up to four samples from that pair, not a
merged manifold across different parts. Check hasContactData before reading them.
