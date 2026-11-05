# Nullus Physics Testing

The main solution now builds these behavioral suites into `NullusUnitTests`.
Use `--gtest_filter="Physics*.*:PlayPhysicsTests.*:RuntimePlaySessionTests.*"`
for focused runs. The per-feature executable names below describe the earlier
validation layout; the assertions remain in the aggregate. `NullusPhysicsBenchmark`
is built only with `NLS_BUILD_PERFORMANCE_TESTS=ON`.

## Focused native suite

`NullusPhysicsTests` links the runtime physics adapter and GoogleTest, without
Editor or a graphics window. It includes the existing PhysicsWorldTests and
new PhysicsClockTests, PhysicsWorld2DTests, PhysicsCommandTests,
PhysicsBodyPropertiesTests and PhysicsShapeQueryTests. It covers real backend compound
shapes, force modes, queries, task completion, filtering, contact aggregation,
sleep, and 3D snapshot restoration. Configure with NLS_BUILD_TESTS=ON. On Windows use the
repository BuildCleanEnvironment wrapper and serialize builds with /m:1.

CTest label: physics;headless. Run with
`ctest --test-dir build -C Debug -R "^NullusPhysicsTests$" --output-on-failure`.

## Required integration gates

NullusPhysicsSceneTests includes 3D/2D scene geometry, parent ownership,
Transform authority, shape rebuilds, Stop cleanup, particles and serialization.
It links Engine without Editor. NullusPlayModeTests and NullusPlayLoopNoUiTests
verify the scheduler and scripting bridge separately.

Run PhysicsSceneTests, PlayPhysicsTests, RuntimePlaySessionTests and runtime
input scheduling tests after changes to the engine bridge. Include scene
round trips, particles, Prefabs, repeated Play/Stop and event-time destruction.
Native runtime tests do not validate new managed bindings or editor interaction.

The phase 1 follow-up covers versioned scene physics settings, legacy defaults,
invalid-input rejection, cloning, in-place gravity/subdivision updates and pending
worker changes. Presentation tests use real SDK bodies, scene proxy extraction
and camera capture. They check alpha-only extraction invalidation, frozen proxy
ownership, authoritative query poses, discontinuity resets, reverse creation
order, mixed-dimension hierarchies and 2D rotations crossing pi. Play tests check
pause, TimeScale-zero manual Step, retained clock debt and resume without a
backwards presentation jump. Pure pose-history tests alone are not render
integration acceptance. GPU pixels and visible Editor interaction remain separate.

Structural command tests use actual PhysX/Box2D worlds. They cover FIFO
body/shape dependencies, copied triangle input, failed replacement retaining the
old shape, invalid polygons, wrong dimensions/worlds/generations, cancellation,
expired worlds and concurrent producers. A retained read view holds a consumer
commit while another producer enqueues, proving that the later work stays pending
and Reset/overlapping Commit cannot invalidate the running snapshot. A direct SDK
adapter sequence is the force-preservation oracle for in-place dynamics updates.

Scene tests cover runtime creation, authoring deferral, contact-time deletion
and creation under retained reads, remove/readd, disable/reenable, stale native
shape reconstruction, clone isolation and eight repeated Play/Stop cycles in both
dimensions. The script dispatch regression uses a real 3D collision, holds a
query view in OnCollisionEnter, removes a collider and queues a teleport. It checks
that nested synchronization cannot commit there, structural release happens at a
safe boundary, and TimeScale-zero Step applies motion on the next fixed tick.

The command-buffer follow-up passed 56 native, 50 scene, 38 PlaySession and one
no-UI test (145 total, zero failures/skips). Validation uses isolated DLL output;
record actual loaded module hashes and reject test XML older than those modules.
Shared compiler objects and PDBs may still be owned by concurrent builds. A later
refresh that cannot acquire those files is recorded separately and does not count
as a successful full Editor/Player build. Preserve all owning processes.

The lifecycle follow-up passed 58 native, 56 scene, 40 PlaySession and one no-UI
test (155 total, zero failures/skips). PhysicsLifecycleTests instantiates real
Prefab object graphs with both solvers, verifies independent compound bodies
and uncopied forces, reparents colliders through two rigidbodies and static
fallback, and retains an independent nested dynamic body. Saved scenes exercise
eight sandbox replacements and a second delayed load queued by a load listener.
Contact/deferral tests retain real query views while editing gravity/subdivisions
and requesting Stop. Script-host probes use FakeScriptBackend with real PhysX
collisions; existing Lua/C# integration tests remain in the same full play suite.
They do not establish new Lua/C# world-settings bindings.

The Play probes check that Stop cancels remaining catch-up ticks and subsequent
Update/LateUpdate, rejects session teardown inside the callback, and that
TimeScale-zero Step commits queued world settings without rebuilding bodies.
Logs, actual process exit codes and XML are in build/physics-validation/lifecycle/Debug.
Executables use its run subdirectory; generic CTest commands require first building
the normal output directory, and must not substitute stale shared binaries for
this isolated validation snapshot.

Benchmark 1k/5k/10k bodies, static geometry, queries, joints, triggers, tilemaps
and multiple worlds. Record hardware, configuration, thread count, warmup,
duration, step percentiles, active bodies and memory. Measurements must come
from real runs. CUDA results and Windows/macOS/Linux results are separate.

Actual commands, counts, failures and unperformed gates are recorded in
PhysicsImplementationStatus.md; this file is not a claim that tests passed.

## Phase 2 body controls and query gate

The follow-up adds 7 native body-property tests, 10 shape-query tests and 6 scene
body-control tests. Analytical impulses verify off-center and rotated principal
inertia, all four force/torque modes and physical body units under scaled geometry.
Tests cover initial sleep, automatic sleeping thresholds, wake followed by geometry
rebuild, lock/unlock without replacement, invalid inputs without partial mutation,
real text round trips and removal of all seven new fields for legacy defaults.

Queries use rotated boxes and Y-axis capsules/cylinders, exact cylinder flat caps,
and a cooked tetrahedron whose first sweep contact differs from its bounding box.
The tetrahedron uses an explicit SDK GJK tolerance; primitive precise sweeps have
tighter analytical checks. Additional tests exercise 180 sweep and 150 overlap
hits beyond the SDK callback's 64-element block, zero/small caller capacity,
per-shape ownership, source-body exclusion, retained geometry after world-wrapper
destruction, stale/foreign/concave source rejection and initial-overlap validity.
Scene tests compose native capsule-local alignment rather than reapplying it.

Final isolated gates use build/physics-validation/phase2/Debug: 75 native,
62 scene, 40 Play and 1 no-UI tests (178 total). Record process exit codes and
reject old XML or mismatched loaded modules. The generated API change initially
exposed stale C# fixture assemblies; rebuilding NullusPlayManagedFixture against
the current manifest resolves that mismatch. No running Editor DLL is deployed.
The full Play suite retains C#/Lua compatibility coverage; new physics facade
parity, visible editing, complete resources and other platforms remain separate.

## Physics resource and offline-cooking gate

PhysicsCookingTests adds seven real-SDK cases: cooked tetrahedron geometry,
mirrored triangle winding/remapping, sampled heightfield collision, unsupported
dynamic/trigger replacement, malformed inputs/corrupt streams, shared/deferred
geometry lifetime and material-combine rebound. Unsupported heightfield scales
and excessive bounds reject before SDK geometry operations.

NullusPhysicsAssetTests is an independent headless Engine/Cook gate. Its eleven
tests use real metadata, ArtifactWriter, ArtifactDB and RuntimeManifestBuilder,
covering cold/warm cache, bake/meta/backend/target changes, native CPU rejection,
failed/cancelled cooks, artifact identity/hash/target validation, immutable reload,
authoring snapshot boundaries, typed-reference text round trips and scene cloning.
A runtime test deletes source/meta files and removes the ArtifactDB before loading
the manifest and simulating a ball on the cooked floor. Another validates actual
2D and 3D rebound from the same material. These are runtime-manifest tests, not
full packaged Player acceptance.

NullusPhysicsAssetEditorTests adds four tests through the real AssetDatabaseFacade.
They reopen both imported types, reject invalid reimports, mutate source/meta from
compile/final-write callbacks and cancel final publication. They check that the
prior artifact survives and the job terminates. Editor library validation uses
isolated output; confirm that the test linker selects that newly built static
library rather than the older default-output library. Visible UI remains separate.

The combined isolated gates are 82 native, 11 asset, 62 scene, 40 Play, 1 no-UI and
4 editor-import tests (200 total when all pass). Actual results, module/source
hashes, build outcomes and unperformed gates belong in ImplementationStatus and
the immutable validation JSON. Do not add the historical 178-test snapshot to this
total; that baseline is preserved separately.

## Reusable Shape resource gate

Six additional native command tests cover whole-group publication, stable query
part IDs, partial-attachment rollback with mass/velocity/pending-force retention,
wrong-body/stale/duplicate references, group limits/clearing, retained read views
and real owner-aggregated contact IDs through sleeping Stay and Exit. Both
dimensions reject single-shape replacement against the wrong owning Body.

Eight additional Engine/Cook tests validate strict source schema/IDs/poses/refs,
canonical ordering, exact rotated box/sphere/capsule/cylinder geometry, mirrored
primitive offsets, transitive mesh/material manifest closure and actual rebound
after deleting source/meta/ArtifactDB, child-mesh and Shape reload, unresolved
dependencies, shear and scale rejection, mesh reflection/concavity rejection,
clone/serialization/disable/Stop/Play, stale-member recovery, rotated parent-body
ownership and whole-group callback-time destruction. Failed updates retain the
old complete geometry. The original lock/unlock scene regression checks that
ordinary body rotation does not replace handles.

Two additional real Editor importer tests verify Shape dependency publication,
source reload, invalid-source retention and stale/cancelled final publication.
Explicitly link the private EditorProject archive; the default generated test
project can otherwise resolve an older archive in the shared output directory.

Final current gates are 88 native, 19 resource, 62 scene, 42 Play, 1 no-UI and
6 editor tests (218 total; zero failures/skips). Shape-specific additions account
for sixteen cases; two other Play tests are present in the concurrent checkout.
Record the tested executables, loaded DLL hashes, archive/link response, sources,
exit codes and fresh XML in WindowsDebug-20261029-Shapes.json. Preserve historical
200-test and earlier evidence. Full documentation, visible interaction, packaged
Player and platform gates are separate results.

## Complex 2D geometry and final cleanup gate

Ten additional native tests exercise actual concave missing regions and per-part
queries, both windings and collinear cleanup, deterministic varying concave
contours, invalid input/output retention, convex input above eight vertices,
area-derived compound mass/impulse, atomic rollback with pending force and
wrong-body/duplicate/limit rejection, immutable recording and retained readers,
one-sided chain support and part identity, ghost-seam sliding and loop closure,
and two-sided edges with numeric rejection before native creation.

Six scene shape tests cover actual contours and retained invalid edits, mirrored
nonuniform local rotation under a simulated parent, chain front preservation and
stale-member recovery, text/clone/legacy array defaults, repeated Stop/Play with
deferred whole-group destruction, and unsupported dynamic chains. Four more
lifecycle tests cover Stop-to-edit query recreation, removal of all four physics
component families while holding reads, retained worlds after scene destruction,
no replayed OnDestroy and preservation of ordinary live callback order. Tests
use actual native worlds and geometry, not bounding-box approximations.

The new destruction test exposed a real owner-context loss after RuntimeDestroy.
The fix queues final native cleanup through Scene, independently of gameplay
callback state. The pre-fix test timed out and only its verified private process
was stopped. Final runs have bounded process timeouts; a timeout is a failure.
Three initial C# schema mismatches require rebuilding the isolated Play fixture
against the final ScriptApi.json rather than deploying a running Editor DLL.

Final gates are 98 native, 19 resource, 72 scene, 42 Play, 1 no-UI and 6 real
Editor importer tests: 238 passed, zero failures/skips. Record exact loaded DLLs,
fresh XML, source/fixture hashes, private archive and response file in
WindowsDebug-20261029-Complex2D.json. Twenty tests are new; historical snapshots
are not added to the current total. Memory/performance of loose chain segments,
Boolean Composite/holes, Tilemap integration, visible editing, Player and other
platforms remain separate gates.

The final Play gate freezes private copies of the shared API manifest and loaded
managed assemblies before starting, then verifies their source content hashes
again after successful exit. Record those copies and both observations. A later
identical-content shared-file regeneration can change mtime without changing the
tested input; it does not replace the captured provenance. Runtime DLLs and test
XML must still match the final build. Changed content during a run fails the gate.

## Benchmark executable

Build NullusPhysicsBenchmark and run it with an output JSON filename. It uses
real native bodies and rays at 1k/5k/10k counts in both dimensions. The recorded
Windows Debug run and interpretation are in PhysicsBenchmarkBaseline.md.
Backend memory unavailable from the adapters is null, while measured process
working set is separately labeled. Benchmark completion is not a performance
threshold pass and is not automatically part of the unit suite.
