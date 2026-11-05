# Nullus Physics Editor

The reflected Physics/Collider component supports Box, Sphere, Capsule,
Cylinder and COLLISION_MESH, half-extents, radius, half-height, center, layer/mask, friction,
restitution and trigger configuration. Rigidbody adds angular velocity,
damping and CCD to its existing mass/kinematic/gravity/velocity settings. Reflected
3D body settings also include a six-bit Constraints mask, Auto Mass Properties,
Center Of Mass, Inertia Tensor, Mass Frame Rotation, Start Awake and Sleep
Threshold. Position XYZ use bits 1/2/4 and rotation XYZ bits 8/16/32; 63 freezes all.
Disable Auto Mass Properties to use the manual center/inertia frame. These values
are meters and kg*m^2 in the body frame; scaling Transform changes collision
geometry without scaling authored manual mass properties. Sleep/wake are runtime
commands, and changing Start Awake does not reset a currently running body.
Round-trip and legacy-default tests exercise actual reflection generation; visible
Inspector editing of the added controls still requires interactive acceptance.
Physics 2D/Rigidbody and Physics 2D/Collider expose the independent Box2D
runtime. The 2D collider Size is a full meter size; 3D Half Extents retain the
legacy half-size meaning. 2D local rotation is authored in degrees; runtime
2D rotations and angular velocities use radians.

Physics Debugger, dedicated collider handles, dedicated material/constraint/character and
advanced-physics authoring are not implemented by this documentation. They are
tracked individually in PhysicsImplementationStatus.md. Edit mode updates
query geometry without advancing simulation. Play uses the existing scene
sandbox and Stop must restore the original editing scene and Undo history.

The new Step contract is exactly one fixed tick, even at TimeScale zero,
followed by one Update/animation/LateUpdate sequence. Visible mouse-driven
acceptance is required separately from timing unit tests.

Scene world settings now round-trip through scene serialization and cloning.
The native ScenePhysicsSettings API configures gravity, solver subdivisions,
worker counts and optional presentation interpolation. A dedicated editor control
for these settings is still pending. Runtime worker changes are pending until
the next world creation; gravity and solver subdivisions update in place.

Interpolation affects copied rendering transforms and game-camera capture.
Inspector, scripts and queries continue to observe authoritative poses. Pause,
Step, teleports and shape reconstruction clear interpolation history. Automated
proxy/camera checks are separate from visible motion and mouse-driven acceptance.

Runtime component creation and removal now commit at safe synchronization
boundaries. Contact callbacks may delete an object, add replacement colliders or
queue motion while a query view remains open; the backend changes on the next
safe commit. Edit-mode geometry still updates without advancing simulation.
Stop clears pending motion and releases native bodies even when code retains
the old worlds. Automated lifecycle checks cover these paths; visible repeated
Play/Stop, Prefab editing and Physics Debugger interaction remain unverified.

The runtime now defers world-setting changes and Stop while a contact callback
holds queries. A queued Stop ends the current catch-up batch at a safe boundary.
Immediate scene teardown is refused in that callback; queued scene loads wait
until it returns. Tests instantiate real 2D/3D Prefab object graphs, reparent
colliders between bodies while preserving nested dynamic poses, replace saved
runtime scenes eight times, and restore the retained editing scene. These are
runtime checks; they do not establish mouse-driven Prefab/editor acceptance.

Physics resources now use `.physmat` and `.collisionmesh` JSON sources, normal
`.meta` GUIDs and the existing import/reimport entry points. Both collider families
have a typed physicsMaterial reference; 3D has collisionMesh plus shape value 4.
Source formats and units are documented in PhysicsAPI.md. The importer publishes
normal native artifacts and ArtifactDB records; reimport retains resource identity
and publishes new immutable data at the next safe scene sync. Invalid or stale
imports and cancellation retain previous artifacts. No running editor binary is
replaced by the isolated validation builds.

AssetDatabaseFacade import/reopen, invalid reimport, source/meta edits at compile
and final-write progress callbacks, and cancellation have focused automated gates.
These exercise the editor's real import implementation without launching a window.
Dedicated asset creation/inspection tools, drag/drop, Undo/Redo and visible
Inspector/Play/save/reload acceptance remain Partial or Not Started as tracked.

The 2D Collider appends Polygon, Edge and Chain shape values. Its reflected
Vertices array uses local meters; Loop closes a Chain. Polygon requires one
simple contour without a duplicate closing point, Edge exactly two vertices,
and Chain at least two (three when closed). Existing Box/Circle/Capsule fields
and enum values remain compatible. The existing reflection pipeline carries the
array and loop through text serialization and clone. Dedicated polygon handles,
contour drawing, multi-path Composite UI and mouse-driven Inspector acceptance
remain separate work; exposing metadata is not that acceptance.

Invalid geometry, unsupported dynamic/trigger chains or invalid Transform edits
retain the previous native group and report LastError. Native tests cover actual
concave empty regions, chain fronts, ghost seams, mass and group rollback. Scene
tests cover mirrored local frames, serialization and deferred whole-group release.

Deleting an object or individual physical component after Stop and edit-query
recreation now retains the scene's release context. A held query view continues
to see the previous geometry until the next safe commit. Final cleanup does not
replay OnDestroy, and ordinary live-object callback order remains compatible.

`.physshape` now uses the same import/reimport path. The Collider's reflected
Shape Asset enum value and typed Shape Resource reference configure its group.
Each part can reference an existing collision mesh and optionally override its
material. All parts share the Collider's layer, mask and Trigger settings.
An imported revision becomes native geometry at safe Scene synchronization;
invalid revisions or unresolved children leave the previous group available.
Reordering parts preserves stable part IDs, while successful replacement
invalidates old native handles. Disabling, reparenting and callback-time deletion
release the whole group through the normal scene command bridge.

Two additional real-importer tests cover Shape reference publication, reload,
invalid source retention, stale final publication and cancellation. A focused
source-independent manifest test resolves both mesh and per-part material
dependencies. These automated paths do not establish a dedicated Shape asset
editor, resource creation menu, drag/drop, Undo/Redo or visible Inspector acceptance.
