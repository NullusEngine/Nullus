# Nullus Physics Architecture

## Baseline and decisions

The implementation extends the existing PhysX 5.6.1 adapter, pinned by commit
09ff24f3279b735e672ff27b155cbf49f6296f4d and archive SHA-256 in
`ThirdParty/physx.cmake`. Particle collision, scene queries, existing component
type identities and saved half-extents remain compatible. Box2D 3.2.0 is the
selected independent 2D backend; it must be pinned and use the version's C API.
The implemented Box2D pin is 178495ba35695aefe9c321e569d2277f06db3b90,
with the archive checksum in ThirdParty/box2d.cmake and its MIT license preserved.

The baseline has a scene-owned PhysX query world, world/index/generation body
handles, box/sphere/capsule components, triangle cooking, basic dynamics and
collision callbacks. Rigidbody and Play code includes pre-existing uncommitted
work. There is no existing full constraint, character, material authoring,
2D physics or advanced-physics implementation to claim complete.

## Boundaries

`NLS_Physics` owns backend lifetime, safe object handles, queries, stepping,
commands and event snapshots. Engine components own serialized configuration
and stable asset/object references. Editor and scripting consume that runtime;
the runtime must not require Editor. Each scene independently owns a 2D and a
3D world; solvers do not collide with each other.

One rigidbody owns one backend body, collecting colliders from its object and
descendants up to the next same-dimension rigidbody. Standalone colliders are
static. Shape identity maps back to its collider and subshape. Two enabled
rigidbodies of different dimensions cannot both control the same Transform.

## Scheduling and lifetime

The Play session is the sole automatic fixed-step scheduler. Default interval:
1/60 second, maximum 8 fixed ticks per frame, maximum debt 0.25 second. Box2D
solver subdivisions are separately configured (default 4). Scaling occurs once.
Pause adds no debt. Manual Step executes exactly one fixed tick and one
Update/animation/LateUpdate sequence, including at TimeScale zero, without
consuming the ordinary accumulator. Overflow debt is measured, never hidden.

Tick order: drain previous commands; run FixedUpdate; apply current commands
and kinematic targets; step the worlds; collect contacts; write authoritative
poses; dispatch events on the game thread; publish presentation snapshots.
Worker callbacks cannot execute scripts, access scene objects or mutate a
solving world. Destruction joins backend tasks before releasing world storage.

PhysicsCommandBuffer records owned, immutable geometry/settings snapshots for
one weakly referenced world. Multiple producers may enqueue; one owning-thread
consumer commits a FIFO snapshot under a native mutation batch. Tickets identify
the buffer, reset generation and result slot, and can reference earlier successful
body/shape commands. Work recorded during a commit waits for the next commit.
Expired worlds, foreign handles and rejected geometry produce explicit results.
Reset cancels pending work and invalidates tickets without undoing applied work.

Scene components create/rebuild native objects during synchronization. Release
queues safe body/shape handles in scene-owned buffers instead of mutating a world
from component destruction. Synchronization drains releases before assembly and
after replacements; Stop and scene destruction drain them before releasing world
ownership. Duplicate or stale releases cannot remove a recycled replacement.
Stopping also clears unconsumed rigidbody motion/force commands, including repeated
Play/Stop on the same scene with retained references to previous native worlds.

Native contact listeners and script contact dispatch execute inside a scene
PhysicsMutationDeferral scope. Component creation, destruction and motion commands
cannot mutate a retained query view in that scope. TrySyncTransforms returns false
there, and reentrant simulation is rejected. The next safe synchronization applies
structural changes; motion/force commands remain queued for a fixed boundary.
Direct native mutation still requires releasing read views before committing.

World gravity and Box2D subdivision edits use the same owned command buffers.
Validated authoring settings are available immediately; their native writes wait
for the next safe commit when a callback holds read views. Outside deferral they
commit in place without recreating bodies. Stop requested inside a callback also
waits for safe synchronization. Stop lifecycle callbacks run under deferral, and
the Play session cancels remaining catch-up ticks and later gameplay phases once
the stop commits.

SceneManager refuses immediate load/unload/sandbox teardown during physics
callbacks or synchronization. LoadAndPlayDelayed owns its request until a safe
Update. The pending callback is moved out before invocation, preserving another
request queued by a scene-load listener. RuntimePlaySession::End reports a callback
lifetime error until the caller returns to a safe boundary. Native-world references
retained across runtime scene replacement remain queryable and contain no retired
bodies; the suspended editing scene retains its independent worlds and objects.

Dynamic simulation owns world pose. Explicit authored pose edits become a
single teleport; physics writeback does not enqueue another teleport. Parent
motion does not implicitly move a dynamic body, including frames without a
fixed tick. Both dimensions synchronize and write world poses parent-first,
independently of creation order. State capture/restore is available separately
in each native world; it excludes solver caches.

ScenePhysicsSettings owns gravity in both dimensions, Box2D solver subdivisions,
worker counts and an optional interpolation flag (default off). The additive,
versioned scene root `physics` field survives scene text round trips and cloning.
Absent fields use defaults; malformed settings reject scene instantiation before
objects or native geometry are created. Gravity and subdivisions update existing
worlds in place. Worker changes apply on next world creation and have an explicit
pending-settings query. Fixed-step timing remains solely in RuntimePlaySession
and project configuration; partial project overrides preserve existing values.

When enabled, PhysicsPoseHistory retains two authoritative poses and publishes
copied presentation transforms. Translation is linear and rotation follows the
shortest quaternion arc. The rigid world-space presentation delta also applies
to descendants until the next active Rigidbody. Mesh/light/skybox proxy extraction,
Sprite capture, particle view transforms and game cameras use this path; native
queries and authoritative Transforms retain the latest submitted physics pose.
Changing only alpha invalidates render extraction without changing physics state.
Body stepIndex and poseRevision distinguish continuous simulation from teleport,
restore, shape replacement and body reconstruction. Discontinuities clear history.
Pause and manual Step publish the latest pose and clear the previous pair, so
resuming with retained fractional debt does not interpolate backwards. Render
sampling never mutates history or writes a scene Transform.

The native worlds expose a constant-time mutation revision separate from the
step index. Both game-thread source collection and the RenderScene synchronization
fast path observe it when interpolation is enabled. Direct native teleport/restore
can therefore invalidate a paused frame without changing authoring data. Shared
reads do not advance this token, and stationary frames retain cache reuse.

## 3D dynamics and query ownership

Automatic mass properties follow actual attached geometry. Manual mass properties
use an explicit body-local center and rotated principal-inertia frame, in physical
units independent of authoring scale. In-place dynamics edits retain handles and
pending forces. startAwake is creation configuration; shape assembly/rebuilding
preserves current sleep state, including a body awakened after creation. Scene
sleep/wake/force commands obey the same deferred commit boundary as other motion.

Shape queries operate under the existing retained read view. Primitive descriptions
are temporary geometry values; a ShapeHandle borrows the native cooked geometry
and automatically ignores its owning compound body. The view retains geometry and
custom-cylinder callbacks with the world storage. Querying never cooks. Source
pose is explicit and includes the native local alignment returned by
GetShapeWorldPose. Concave meshes and heightfields are targets rather than source
volumes; rejection, invalid handles and numeric input errors have explicit status.
Bounded outputs retain nearest shape hits and count truncation without heap
allocation for result storage. Initial overlaps carry validity flags instead of
invented contact positions. Precise primitive sweeps and tolerance-based SDK
convex sweeps remain distinct query paths.

## Resources and compatibility

The implemented PhysicsMaterial/CollisionMeshAsset pipeline uses existing meta
GUIDs, PPtr/ObjectIdentifier references, ArtifactDB, native
artifact containers and runtime manifests. Backend pointers and handles are
never persisted. Cooking keys include source GUID/hash, parameters, backend
version and target. JSON sources feed the same immutable compiler from shared
Cook and editor import. PhysX convex, triangle and heightfield streams are cooked
offline and loaded once per shared immutable resource, independently of worlds.
Native compatibility includes OS, CPU and endianness; cooking for another native
platform/architecture fails explicitly and requires that target's host.

Publication checks source/meta revisions and cancellation before and during
artifact writing, retaining prior manifests on rejection. Resource loading checks
the full content SHA-256 and typed GUID/fileID/schema/target identity before
replacing its data. Reimport changes immutable snapshots while retaining Object
identity; old shapes retain the earlier native mesh until safely released. Scene
synchronization owns one lazy authoring ArtifactDB snapshot across collider
lookups; runtime lookups use the active manifest. This implements the resource
compiler/publication boundary, without claiming a complete background editor UI.

PhysicsMaterial supplies separate coefficients for both dimensions and PhysX
combine modes. Existing inline collider coefficients remain the default when no
material is assigned. Cooked mesh scale is separate from baked source scale;
reflections are baked with corrected triangle winding. Shear and unbaked reflected
mesh transforms reject with component diagnostics and retain prior valid shapes.
The raw sampled heightfield source remains independent of Terrain content tools.

Preserve existing type names, enum values and half-extents; old scenes upgrade
in memory and are only rewritten on explicit save.

Reusable `.physshape` resources contain versioned configuration and stable
material/mesh references. A single Collider owns a bounded group of native
shapes; positive resource part IDs survive reordering and resource reload.
Compiler output follows the normal ImportedArtifact dependency closure. Dangling
references can be authored, but packaging reports missing dependencies and safe
scene synchronization retains the previous complete geometry group.

`PhysicsCommandBuffer::ReplaceShapes` validates all old handles against the
requested Body and builds the new group inside one mutation batch. A failed
attachment removes the newly attached shapes before returning; old shapes are
removed only after every new attachment succeeds. Retained readers observe a
complete group. World revision changes and solver-cache rollback are not promised
on a rejected replacement. Component dirty detection compares signed axes in
Body-local space, so normal rigidbody rotation does not reconstruct shapes.

## External 2D integration contract

Physics resource lifetime also crosses Stop-to-edit transitions: a component's
gameplay OnDestroy may already have run while its edit query geometry is live
again. Scene cleanup explicitly releases that geometry with its scene context
before final owner loss. Individual component-removal events carry the same
context. This keeps retained reads safe without replaying gameplay destruction;
normal live callback order remains unchanged.

The native 2D geometry builders validate bounded simple contours before calling
Box2D. Ear clipping and convex merging preserve concave empty regions. One
Collider can now own several native polygon pieces or chain segments, each with
an owner and revision-local part ID. ReplaceShapes2D uses the same atomic batch
and rollback logic as 3D groups. Query results carry the actual segment/piece
identity, and retained readers cannot observe a partially replaced group.

Chain segments use neighboring ghost vertices and one-sided collision. The
adapter owns each SDK loose segment independently to match ShapeHandle2D
lifetime, rather than exporting backend chain IDs. This trades bulk-chain memory
sharing for safe individual ownership; no large Tilemap memory/performance claim
is made. Components serialize meter-space vertices and loop configuration, bake
signed local axes, and release/recover all members through the normal scene bridge.

The other 2D-content task owns sprites, PPU conversion, tilesets and tilemap
editing/rendering. Physics consumes immutable chunk snapshots containing stable
source identity, chunk coordinates, revision, cell size in meters, per-cell
collision contours/transforms and material/filter/one-way properties. Dirty and
deleted chunk notifications drive updates. Physics merges compatible adjacent
geometry and refreshes neighboring seams. No second tilemap asset system is
introduced. The editing adapter and platformer acceptance remain Partial until
that task's real source is integrated.

## Advanced capabilities and validation

Reuse PhysX CCT, joints and Vehicle2. Ragdolls use the existing Skeleton bone
IDs/signature, Animator capture and SkinnedMeshRenderer publication boundary;
physics must publish the blended pose before skinning. Pre-fractured assets
activate real rigid fragments; buoyancy applies forces rather than simulating
fluids. PhysX deformable volumes/surfaces require GPU dynamics: an optional CUDA
module on Windows/Linux NVIDIA hardware, disabled by default and unsupported
on macOS. Do not report a CPU soft-body or universal GPU cloth implementation.

All stages require focused builds, behavioral tests and regression checks.
Track implementation, automated validation, visible interaction and platform
validation independently in PhysicsImplementationStatus.md. No stage advances
on the strength of compiling placeholder interfaces. Network foundations do
not promise exact solver rollback or cross-platform deterministic simulation.
