#include <gtest/gtest.h>
#include <Physics/PhysicsWorld2D.h>
#include <Physics/PhysicsWorld.h>
#include <array>
#include <future>
#include <chrono>
#include <limits>
#include <TaskGraph/TaskGraph.h>

namespace
{
using namespace NLS::Physics;
using NLS::Maths::Vector2;
std::shared_ptr<PhysicsWorld2D> World2D(Vector2 gravity = {0, -9.81f})
{
    std::string error;
    auto world = PhysicsWorld2D::Create({gravity, 4}, error);
    EXPECT_NE(world, nullptr) << error;
    return world;
}
BodySettings2D Dynamic2D(Vector2 position = {})
{
    BodySettings2D s; s.type = BodyType2D::Dynamic; s.position = position; return s;
}
}

TEST(PhysicsWorld2DTests, RealGravityAndWorldIsolation)
{
    auto first = World2D(), second = World2D({0, 0}); ASSERT_TRUE(first); ASSERT_TRUE(second);
    const auto a = first->CreateBody(Dynamic2D({0, 5}));
    const auto b = second->CreateBody(Dynamic2D({0, 5}));
    ASSERT_TRUE(first->AddCircle(a, 0.5f).IsValid()); ASSERT_TRUE(second->AddCircle(b, 0.5f).IsValid());
    for (int i = 0; i < 60; ++i) first->Step(1.0f / 60);
    BodyState2D sa, sb; ASSERT_TRUE(first->GetState(a, sa)); ASSERT_TRUE(second->GetState(b, sb));
    EXPECT_NEAR(sa.position.y, 5.0f - 4.905f, 0.06f);
    EXPECT_NEAR(sa.velocity.y, -9.81f, 0.001f);
    EXPECT_FLOAT_EQ(sb.position.y, 5);
    EXPECT_EQ(first->GetStatistics().stepIndex, 60u); EXPECT_EQ(second->GetStatistics().stepIndex, 0u);
}

TEST(PhysicsWorld2DTests, PoseRevisionsInvalidateHistoryOnlyForDiscontinuousChanges)
{
    auto world = World2D({0,0}); ASSERT_TRUE(world);
    const auto body = world->CreateBody(Dynamic2D());
    ASSERT_TRUE(world->AddBox(body,{0.5f,0.5f}).IsValid());
    BodyState2D initial, moved; ASSERT_TRUE(world->GetState(body,initial));
    ASSERT_TRUE(world->SetVelocity(body,{2,0})); world->Step(0.1f);
    ASSERT_TRUE(world->GetState(body,moved));
    EXPECT_EQ(moved.stepIndex,initial.stepIndex+1); EXPECT_EQ(moved.poseRevision,initial.poseRevision);
    ASSERT_TRUE(world->SetTransform(body,{5,0},0));
    BodyState2D teleported; ASSERT_TRUE(world->GetState(body,teleported));
    EXPECT_EQ(teleported.stepIndex,moved.stepIndex); EXPECT_GT(teleported.poseRevision,moved.poseRevision);
    const auto extra = world->AddCircle(body,0.25f); ASSERT_TRUE(extra.IsValid());
    BodyState2D rebuilt; ASSERT_TRUE(world->GetState(body,rebuilt)); EXPECT_GT(rebuilt.poseRevision,teleported.poseRevision);
    ASSERT_TRUE(world->Remove(extra)); ASSERT_TRUE(world->GetState(body,teleported)); EXPECT_GT(teleported.poseRevision,rebuilt.poseRevision);
    const auto snapshot = world->CaptureState(); world->Step(0.1f);
    std::string error; ASSERT_TRUE(world->RestoreState(snapshot,error)) << error;
    ASSERT_TRUE(world->GetState(body,rebuilt)); EXPECT_EQ(rebuilt.stepIndex,snapshot.stepIndex);
    EXPECT_GT(rebuilt.poseRevision,teleported.poseRevision); EXPECT_EQ(rebuilt.body,body);
}

TEST(PhysicsWorld2DTests, BoxCircleCapsuleAndConvexPolygonSettleOnTheFloor)
{
    for (int shape = 0; shape < 4; ++shape)
    {
        auto world = World2D(); ASSERT_TRUE(world);
        BodySettings2D ground; ground.position = {0, -0.5f};
        ASSERT_TRUE(world->AddBox(world->CreateBody(ground), {10, 0.5f}).IsValid());
        const auto body = world->CreateBody(Dynamic2D({0, 4}));
        ShapeHandle2D collider;
        if (shape == 0) collider = world->AddBox(body, {0.5f, 0.5f});
        if (shape == 1) collider = world->AddCircle(body, 0.5f);
        if (shape == 2) collider = world->AddCapsule(body, 0.25f, 0.5f);
        if (shape == 3)
        {
            const std::array<Vector2, 4> vertices{{{-0.5f,-0.5f},{0.5f,-0.5f},{0.5f,0.5f},{-0.5f,0.5f}}};
            collider = world->AddConvexPolygon(body, vertices);
        }
        ASSERT_TRUE(collider.IsValid()) << shape;
        for (int i = 0; i < 180; ++i) world->Step(1.0f / 60);
        BodyState2D state; ASSERT_TRUE(world->GetState(body, state));
        EXPECT_NEAR(state.position.y, shape == 2 ? 0.75f : 0.5f, 0.03f) << shape;
        EXPECT_NEAR(state.velocity.y, 0, 0.01f) << shape;
        EXPECT_GT(world->GetStatistics().contactCount, 0u);
    }
}

TEST(PhysicsWorld2DTests, CompoundShapesShareOneBodyAndRecomputeMass)
{
    auto world = World2D({0, 0}); ASSERT_TRUE(world);
    const auto body = world->CreateBody(Dynamic2D());
    ShapeSettings2D left; left.offset = {-1, 0};
    ShapeSettings2D right; right.offset = {1, 0};
    const auto first = world->AddBox(body, {0.5f, 0.5f}, left);
    const auto second = world->AddBox(body, {0.5f, 0.5f}, right);
    ASSERT_TRUE(first.IsValid()); ASSERT_TRUE(second.IsValid());
    EXPECT_EQ(world->GetStatistics().bodyCount, 1u); EXPECT_EQ(world->GetStatistics().shapeCount, 2u);
    BodyState2D state; ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.mass, 2, 1e-5f);
    ASSERT_TRUE(world->Remove(first)); ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.mass, 1, 1e-5f);
    ASSERT_TRUE(world->Remove(body)); EXPECT_FALSE(world->Remove(second));
    EXPECT_EQ(world->GetStatistics().bodyCount, 0u); EXPECT_EQ(world->GetStatistics().shapeCount, 0u);
    const auto replacement = world->CreateBody(Dynamic2D());
    EXPECT_EQ(body.index, replacement.index); EXPECT_NE(body.generation, replacement.generation);
    EXPECT_FALSE(world->GetState(body, state));
}

TEST(PhysicsWorld2DTests, ForceModesAndTorqueUsePhysicalMassAndInertia)
{
    auto world = World2D({0, 0}); ASSERT_TRUE(world);
    auto settings = Dynamic2D(); settings.autoMass = false; settings.mass = 2;
    const auto body = world->CreateBody(settings); ASSERT_TRUE(world->AddBox(body, {0.5f, 0.5f}).IsValid());
    BodyState2D state; ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.mass, 2, 1e-6f);
    ASSERT_TRUE(world->AddForce(body, {2, 0}, ForceMode2D::Impulse));
    ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.velocity.x, 1, 1e-6f);
    ASSERT_TRUE(world->AddForce(body, {1, 0}, ForceMode2D::VelocityChange));
    ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.velocity.x, 2, 1e-6f);
    ASSERT_TRUE(world->SetVelocity(body, {}));
    ASSERT_TRUE(world->AddForce(body, {2, 0})); world->Step(0.1f);
    ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.velocity.x, 0.1f, 1e-5f);
    ASSERT_TRUE(world->SetVelocity(body, {}));
    ASSERT_TRUE(world->AddForce(body, {1, 0}, ForceMode2D::Acceleration)); world->Step(0.1f);
    ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.velocity.x, 0.1f, 1e-5f);
    const auto inertia = state.inertia; ASSERT_GT(inertia, 0);
    ASSERT_TRUE(world->AddTorque(body, inertia, ForceMode2D::Impulse));
    ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.angularVelocity, 1, 1e-5f);
}

TEST(PhysicsWorld2DTests, RayAndCircleQueriesReturnActualShapeIdentityAndRespectFilters)
{
    auto world = World2D(); ASSERT_TRUE(world);
    BodySettings2D settings; settings.position = {0, -1}; const auto body = world->CreateBody(settings);
    ShapeSettings2D shape; shape.owner = 42; shape.layers = 4;
    const auto handle = world->AddBox(body, {5, 0.5f}, shape); ASSERT_TRUE(handle.IsValid());
    QueryHit2D hit; ASSERT_TRUE(world->Read()->Raycast({0, 3}, {0, -2}, 10, hit));
    EXPECT_EQ(hit.body, body); EXPECT_EQ(hit.shape, handle); EXPECT_EQ(hit.owner, 42u);
    EXPECT_NEAR(hit.distance, 3.5f, 0.001f); EXPECT_NEAR(hit.normal.y, 1, 0.001f); EXPECT_TRUE(hit.hasPointAndNormal);
    ASSERT_TRUE(world->Read()->CircleCast({0, 3}, 0.5f, {0, -1}, 10, hit)); EXPECT_NEAR(hit.distance, 3, 0.02f);
    const auto overlaps = world->Read()->OverlapCircle({0, -1}, 0.25f); ASSERT_EQ(overlaps.size(), 1u);
    EXPECT_EQ(overlaps[0].shape, handle); EXPECT_FALSE(overlaps[0].hasPointAndNormal);
    QueryFilter2D filter; filter.layers = 2; EXPECT_FALSE(world->Read(filter)->Raycast({0, 3}, {0, -1}, 10, hit));
    filter.layers = 4; filter.ignored = {body}; EXPECT_TRUE(world->Read(filter)->OverlapCircle({0, -1}, 1).empty());
}

TEST(PhysicsWorld2DTests, TriggerQueriesAndAllHitsAreFilteredAndDistanceSorted)
{
    auto world = World2D(); ASSERT_TRUE(world);
    for (int i = 0; i < 130; ++i)
    {
        BodySettings2D body; body.position = {float(i) * 3, 0};
        ShapeSettings2D s; s.trigger = i == 0;
        ASSERT_TRUE(world->AddCircle(world->CreateBody(body), 0.5f, s).IsValid());
    }
    EXPECT_EQ(world->Read()->RaycastAll({-2, 0}, {1, 0}, 500).size(), 129u);
    QueryFilter2D filter; filter.includeTriggers = true;
    const auto hits = world->Read(filter)->RaycastAll({-2, 0}, {1, 0}, 500); ASSERT_EQ(hits.size(), 130u);
    for (size_t i = 1; i < hits.size(); ++i) EXPECT_LT(hits[i - 1].distance, hits[i].distance);
}

TEST(PhysicsWorld2DTests, MotionLocksSleepAndTeleportVelocityPolicyAreEffective)
{
    auto world = World2D({0, 0}); ASSERT_TRUE(world);
    auto s = Dynamic2D(); s.lockPositionX = true; s.lockRotation = true;
    const auto body = world->CreateBody(s); ASSERT_TRUE(world->AddBox(body, {0.5f, 0.5f}).IsValid());
    ASSERT_TRUE(world->SetAwake(body, false)); EXPECT_EQ(world->GetStatistics().sleepingBodyCount, 1u);
    ASSERT_TRUE(world->AddForceAtPosition(body, {3, 4}, {1, 0}, ForceMode2D::Impulse)); world->Step(1.0f / 60);
    BodyState2D state; ASSERT_TRUE(world->GetState(body, state));
    EXPECT_NEAR(state.position.x, 0, 1e-6f); EXPECT_GT(state.position.y, 0); EXPECT_NEAR(state.rotation, 0, 1e-6f);
    const auto velocity = state.velocity;
    ASSERT_TRUE(world->SetTransform(body, {5, 6}, 0)); ASSERT_TRUE(world->GetState(body, state));
    EXPECT_EQ(state.velocity, velocity);
    ASSERT_TRUE(world->SetTransform(body, {7, 8}, 0, false)); ASSERT_TRUE(world->GetState(body, state));
    EXPECT_EQ(state.velocity, Vector2{});
}

TEST(PhysicsWorld2DTests, BulletDoesNotTunnelThroughThinStaticGeometry)
{
    auto world = World2D({0, 0}); ASSERT_TRUE(world);
    ASSERT_TRUE(world->AddBox(world->CreateBody(), {0.05f, 5}).IsValid());
    auto s = Dynamic2D({-3, 0}); s.velocity = {100, 0}; s.bullet = true;
    const auto body = world->CreateBody(s); ASSERT_TRUE(world->AddCircle(body, 0.1f).IsValid());
    world->Step(0.1f); BodyState2D state; ASSERT_TRUE(world->GetState(body, state)); EXPECT_LT(state.position.x, -0.1f);
}

TEST(PhysicsWorld2DTests, InvalidInputsAndForeignOrRecycledHandlesNeverReachBackendAssertions)
{
    auto world = World2D(), other = World2D(); ASSERT_TRUE(world); ASSERT_TRUE(other);
    const auto body = world->CreateBody();
    EXPECT_FALSE(other->AddCircle(body, 1).IsValid()); EXPECT_FALSE(world->AddCircle(body, -1).IsValid());
    EXPECT_FALSE(world->AddCapsule(body, 1, 0).IsValid()); EXPECT_FALSE(world->AddBox(body, {0, 1}).IsValid());
    const std::array<Vector2, 4> concave{{{0,0},{2,0},{0.2f,0.2f},{0,2}}};
    EXPECT_FALSE(world->AddConvexPolygon(body, concave).IsValid());
    auto invalid = Dynamic2D(); invalid.position.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(world->CreateBody(invalid).IsValid());
    std::string error; EXPECT_FALSE(PhysicsWorld2D::Create({{0,0}, 0}, error)); EXPECT_FALSE(error.empty());
    const auto old = world->AddCircle(body, 1); ASSERT_TRUE(world->Remove(old));
    const auto next = world->AddCircle(body, 1); EXPECT_EQ(old.index, next.index); EXPECT_NE(old.generation, next.generation);
    EXPECT_FALSE(world->Remove(old)); EXPECT_FALSE(other->Remove(body));
}

TEST(PhysicsWorld2DTests, ReadViewRetainsWorldAndConcurrentQueriesUseIndependentStorage)
{
    auto world = World2D(); ASSERT_TRUE(world);
    ASSERT_TRUE(world->AddCircle(world->CreateBody(), 1).IsValid());
    auto read = world->Read(); world.reset();
    auto first = std::async(std::launch::async, [&] { QueryHit2D hit; return read->Raycast({0,3},{0,-1},10,hit); });
    auto second = std::async(std::launch::async, [&] { QueryHit2D hit; return read->Raycast({3,0},{-1,0},10,hit); });
    EXPECT_TRUE(first.get()); EXPECT_TRUE(second.get());
}

TEST(PhysicsWorld2DTests, TwoAndThreeDimensionalSolversDoNotAdvanceEachOther)
{
    auto world = World2D(); ASSERT_TRUE(world);
    std::string error; auto spatial = PhysicsWorld::Create(error); ASSERT_TRUE(spatial) << error;
    const auto body = world->CreateBody(Dynamic2D({0, 3})); ASSERT_TRUE(world->AddCircle(body, 0.5f).IsValid());
    BodySettings s; s.dynamic = true; s.kinematic = false; s.position = {0,3,0};
    const auto body3D = spatial->AddSphere(0.5f, s); ASSERT_TRUE(body3D.IsValid());
    spatial->Step(0.1f);
    BodyState2D state; ASSERT_TRUE(world->GetState(body, state)); EXPECT_FLOAT_EQ(state.position.y, 3);
    world->Step(0.1f); ASSERT_TRUE(world->GetState(body, state)); EXPECT_LT(state.position.y, 3);
}

TEST(PhysicsWorld2DTests, SharedTaskGraphExecutesBackendTasksAndOutlivesEachWorld)
{
    NLS::Base::TaskGraph::TaskGraphConfig config; config.workerCount = 2;
    auto tasks = std::make_shared<NLS::Base::TaskGraph::TaskGraph>(config); ASSERT_TRUE(tasks->Start());
    WorldSettings2D settings; settings.tasks = tasks;
    std::string error; auto world = PhysicsWorld2D::Create(settings, error); ASSERT_TRUE(world) << error;
    for (int i = 0; i < 128; ++i)
    {
        const auto body = world->CreateBody(Dynamic2D({float(i % 16), float(i / 16) * 1.1f}));
        ASSERT_TRUE(world->AddBox(body, {0.49f, 0.49f}).IsValid());
    }
    for (int i = 0; i < 10; ++i) world->Step(1.0f / 60);
    EXPECT_GT(tasks->GetRetainedTaskCount(), 0u);
    EXPECT_EQ(tasks->GetPendingTaskCount(), 0u); EXPECT_EQ(tasks->GetRunningTaskCount(), 0u);
    world.reset(); EXPECT_TRUE(tasks->IsStarted()); tasks->Shutdown();
}

TEST(PhysicsWorld2DTests, MutationBatchDoesNotExposeACompoundRebuildToConcurrentReaders)
{
    auto world = World2D(); ASSERT_TRUE(world);
    const auto body = world->CreateBody(); const auto first = world->AddCircle(body, 1); ASSERT_TRUE(first.IsValid());
    std::promise<void> entered;
    std::future<size_t> reader;
    {
        auto batch = world->BeginMutationBatch();
        ASSERT_TRUE(world->Remove(first));
        reader = std::async(std::launch::async, [&]
        { entered.set_value(); return world->Read()->OverlapCircle({0,0}, 2).size(); });
        entered.get_future().wait();
        EXPECT_EQ(reader.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
        EXPECT_TRUE(world->AddBox(body, {0.5f,0.5f}).IsValid());
    }
    EXPECT_EQ(reader.get(), 1u);
}

TEST(PhysicsWorld2DTests, QueryLayersRemainIndependentOfTheSimulationCollisionMask)
{
    auto world = World2D({0,0}); ASSERT_TRUE(world);
    ShapeSettings2D shape; shape.layers = 8; shape.collisionMask = 0;
    const auto body = world->CreateBody(); ASSERT_TRUE(world->AddCircle(body, 1, shape).IsValid());
    QueryFilter2D filter; filter.layers = 8;
    QueryHit2D hit;
    EXPECT_TRUE(world->Read(filter)->Raycast({0,3},{0,-1},10,hit));
    EXPECT_EQ(hit.body, body);
    EXPECT_EQ(world->Read(filter)->RaycastAll({0,3},{0,-1},10).size(), 1u);
    EXPECT_EQ(world->Read(filter)->OverlapCircle({0,0}, 0.5f).size(), 1u);
    EXPECT_TRUE(world->Read(filter)->CircleCast({0,3},0.25f,{0,-1},10,hit));
    filter.layers = 4;
    EXPECT_FALSE(world->Read(filter)->Raycast({0,3},{0,-1},10,hit));
}

TEST(PhysicsWorld2DTests, InlineTaskGraphWithNoRetainedRecordsStillWaitsForAcceptedTasks)
{
    NLS::Base::TaskGraph::TaskGraphConfig config;
    config.workerCount = 0; config.forceInlineExecution = true; config.retainedTerminalTaskCount = 0;
    auto tasks = std::make_shared<NLS::Base::TaskGraph::TaskGraph>(config); ASSERT_TRUE(tasks->Start());
    WorldSettings2D settings; settings.tasks = tasks;
    std::string error; auto world = PhysicsWorld2D::Create(settings, error); ASSERT_TRUE(world) << error;
    const auto body = world->CreateBody(Dynamic2D({0,3})); ASSERT_TRUE(world->AddCircle(body, 0.5f).IsValid());
    world->Step(0.1f);
    BodyState2D state; ASSERT_TRUE(world->GetState(body, state)); EXPECT_LT(state.position.y, 3);
    EXPECT_EQ(tasks->GetPendingTaskCount(), 0u); EXPECT_EQ(tasks->GetRunningTaskCount(), 0u);
    world.reset(); tasks->Shutdown();
}

TEST(PhysicsWorld2DTests, CallerOwnedQueryBuffersReportRequiredCapacityAndNearestHits)
{
    auto world=World2D(); ASSERT_TRUE(world);
    for(int i=0;i<130;++i) { BodySettings2D settings; settings.position={float(i)*3,0}; ASSERT_TRUE(world->AddCircle(world->CreateBody(settings),0.5f).IsValid()); }
    std::array<QueryHit2D,3> buffer;
    auto read=world->Read(); const auto all=read->RaycastAll({-2,0},{1,0},500);
    const auto limited=read->RaycastNonAlloc({-2,0},{1,0},500,buffer);
    EXPECT_EQ(limited.count,3u); EXPECT_EQ(limited.total,130u); EXPECT_TRUE(limited.truncated);
    for(size_t i=0;i<buffer.size();++i) { EXPECT_EQ(buffer[i].shape,all[i].shape); EXPECT_FLOAT_EQ(buffer[i].distance,all[i].distance); }
    const auto count=read->RaycastNonAlloc({-2,0},{1,0},500,{}); EXPECT_EQ(count.count,0u); EXPECT_EQ(count.total,130u); EXPECT_TRUE(count.truncated);
    const auto overlap=read->OverlapCircleNonAlloc({0,0},500,buffer);
    EXPECT_EQ(overlap.count,3u); EXPECT_EQ(overlap.total,130u); EXPECT_TRUE(overlap.truncated); EXPECT_FALSE(buffer[0].hasPointAndNormal);
    const auto empty=read->RaycastNonAlloc({-2,5},{1,0},500,buffer); EXPECT_EQ(empty.count,0u); EXPECT_FALSE(empty.truncated);
}

TEST(PhysicsWorld2DTests, SnapshotRestoresPoseVelocitySleepAndStepForUnchangedTopology)
{
    auto world=World2D(); ASSERT_TRUE(world);
    const auto body=world->CreateBody(Dynamic2D({0,3})); ASSERT_TRUE(world->AddCircle(body,0.5f).IsValid());
    world->Step(0.1f); const auto saved=world->CaptureState(); world->Step(0.1f);
    std::string error; ASSERT_TRUE(world->RestoreState(saved,error)) << error;
    BodyState2D state; ASSERT_TRUE(world->GetState(body,state));
    EXPECT_FLOAT_EQ(state.position.y,saved.bodies[0].position.y); EXPECT_FLOAT_EQ(state.velocity.y,saved.bodies[0].velocity.y);
    EXPECT_EQ(world->GetStatistics().stepIndex,saved.stepIndex); EXPECT_EQ(state.awake,saved.bodies[0].awake);
    ASSERT_TRUE(world->SetAwake(body,false)); const auto sleeping=world->CaptureState();
    ASSERT_TRUE(world->SetAwake(body,true)); ASSERT_TRUE(world->RestoreState(sleeping,error));
    ASSERT_TRUE(world->GetState(body,state)); EXPECT_FALSE(state.awake);
    const auto before=world->CaptureState(); auto invalid=before; invalid.bodies[0].position.x=std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(world->RestoreState(invalid,error));
    ASSERT_TRUE(world->AddBox(body,{0.2f,0.2f}).IsValid()); EXPECT_FALSE(world->RestoreState(before,error));
    auto other=World2D(); ASSERT_TRUE(other); EXPECT_FALSE(other->RestoreState(saved,error));
}
