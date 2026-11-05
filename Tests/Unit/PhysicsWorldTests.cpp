#include <gtest/gtest.h>

#include <array>
#include <future>
#include <limits>
#include <TaskGraph/TaskGraph.h>

#include <Physics/PhysicsWorld.h>

namespace
{
using namespace NLS::Physics;
std::shared_ptr<PhysicsWorld> World()
{
    std::string error;
    auto world = PhysicsWorld::Create(error);
    EXPECT_NE(world,nullptr) << error;
    return world;
}
}

TEST(PhysicsWorldTests, RaycastReportsStableIdentityPositionNormalAndMaterial)
{
    auto world = World(); ASSERT_NE(world,nullptr);
    BodySettings settings; settings.position = {0,-1,0}; settings.material = 42;
    const auto body = world->AddBox({5,0.5f,5},settings);
    ASSERT_TRUE(body.IsValid());
    QueryHit hit;
    ASSERT_TRUE(world->Read()->Raycast({0,3,0},{0,-2,0},10,hit));
    EXPECT_EQ(hit.body,body);
    EXPECT_NEAR(hit.distance,3.5f,1e-5f);
    EXPECT_NEAR(hit.position.y,-0.5f,1e-5f);
    EXPECT_NEAR(hit.normal.y,1,1e-5f);
    EXPECT_EQ(hit.material,42u);
}

TEST(PhysicsWorldTests, PoseRevisionsDistinguishSimulationFromTeleportRebuildAndRestore)
{
    auto world = World(); ASSERT_TRUE(world); world->SetGravity({0,0,0});
    BodySettings settings; settings.dynamic = true; settings.kinematic = false;
    const auto body = world->CreateBody(settings);
    const auto shape = world->AttachBox(body,{0.5f,0.5f,0.5f}); ASSERT_TRUE(shape.IsValid());
    BodyState initial, moved; ASSERT_TRUE(world->GetState(body,initial));
    ASSERT_TRUE(world->SetVelocity(body,{2,0,0})); world->Step(0.1f);
    ASSERT_TRUE(world->GetState(body,moved));
    EXPECT_EQ(moved.stepIndex,initial.stepIndex+1); EXPECT_EQ(moved.poseRevision,initial.poseRevision);
    ASSERT_TRUE(world->SetTransform(body,{5,0,0},NLS::Maths::Quaternion::Identity));
    BodyState teleported; ASSERT_TRUE(world->GetState(body,teleported));
    EXPECT_EQ(teleported.stepIndex,moved.stepIndex); EXPECT_GT(teleported.poseRevision,moved.poseRevision);
    const auto extra = world->AttachSphere(body,0.25f); ASSERT_TRUE(extra.IsValid());
    BodyState rebuilt; ASSERT_TRUE(world->GetState(body,rebuilt)); EXPECT_GT(rebuilt.poseRevision,teleported.poseRevision);
    ASSERT_TRUE(world->Remove(extra)); ASSERT_TRUE(world->GetState(body,teleported)); EXPECT_GT(teleported.poseRevision,rebuilt.poseRevision);
    const auto snapshot = world->CaptureState(); world->Step(0.1f);
    std::string error; ASSERT_TRUE(world->RestoreState(snapshot,error)) << error;
    ASSERT_TRUE(world->GetState(body,rebuilt));
    EXPECT_EQ(rebuilt.stepIndex,snapshot.stepIndex); EXPECT_GT(rebuilt.poseRevision,teleported.poseRevision);
    EXPECT_TRUE(world->IsValid(body));
}

TEST(PhysicsWorldTests, FilteringAndGenerationPreventStaleOrForeignHandles)
{
    auto world = World(), other = World();
    ASSERT_NE(world,nullptr); ASSERT_NE(other,nullptr);
    BodySettings settings; settings.layer = 4;
    auto body = world->AddSphere(1,settings);
    QueryHit hit;
    QueryFilter filter; filter.layers = 2;
    EXPECT_FALSE(world->Read(filter)->Raycast({0,0,3},{0,0,-1},10,hit));
    filter.layers = 4; filter.ignored = {body};
    EXPECT_FALSE(world->Read(filter)->Raycast({0,0,3},{0,0,-1},10,hit));
    EXPECT_FALSE(other->Remove(body));
    ASSERT_TRUE(world->Remove(body));
    EXPECT_FALSE(world->Remove(body));
    auto replacement = world->AddSphere(1,settings);
    EXPECT_EQ(replacement.index,body.index);
    EXPECT_NE(replacement.generation,body.generation);
    EXPECT_FALSE(world->SetTransform(body,{1,2,3},NLS::Maths::Quaternion::Identity));
}

TEST(PhysicsWorldTests, AllHitsStreamsBeyondTheInternalBufferAndReturnsDistanceOrder)
{
    auto world = World(); ASSERT_NE(world,nullptr);
    for (int index = 0; index < 130; ++index)
    {
        BodySettings settings; settings.position = {0,0,static_cast<float>(index)*3};
        ASSERT_TRUE(world->AddSphere(0.5f,settings).IsValid());
    }
    const auto hits = world->Read()->RaycastAll({0,0,-2},{0,0,1},500);
    ASSERT_EQ(hits.size(),130u);
    for (size_t index = 1; index < hits.size(); ++index) EXPECT_LT(hits[index-1].distance,hits[index].distance);
}

TEST(PhysicsWorldTests, SphereSweepsAndOverlapRespectTriggersAndDynamicFilters)
{
    auto world = World(); ASSERT_NE(world,nullptr);
    BodySettings settings; settings.dynamic = true; settings.trigger = true;
    const auto body = world->AddBox({1,1,1},settings);
    ASSERT_TRUE(body.IsValid());
    QueryHit hit;
    EXPECT_FALSE(world->Read()->SweepSphere(0.5f,{0,3,0},{0,-1,0},10,hit));
    QueryFilter filter; filter.includeTriggers = true;
    ASSERT_TRUE(world->Read(filter)->SweepSphere(0.5f,{0,3,0},{0,-1,0},10,hit));
    EXPECT_NEAR(hit.distance,1.5f,0.001f);
    EXPECT_EQ(world->Read(filter)->OverlapSphere(0.25f,{0,0,0}).size(),1u);
    filter.includeDynamic = false;
    EXPECT_TRUE(world->Read(filter)->OverlapSphere(0.25f,{0,0,0}).empty());
}

TEST(PhysicsWorldTests, CooksTriangleGeometryAndRejectsBadIndices)
{
    auto world = World(); ASSERT_NE(world,nullptr);
    const std::array<NLS::Maths::Vector3,3> vertices{{{-2,0,-2},{0,0,2},{2,0,-2}}};
    const std::array<uint32_t,3> indices{0,1,2}, invalid{0,1,7};
    EXPECT_FALSE(world->AddTriangleMesh(vertices,invalid).IsValid());
    const auto body = world->AddTriangleMesh(vertices,indices);
    ASSERT_TRUE(body.IsValid());
    QueryHit hit;
    ASSERT_TRUE(world->Read()->Raycast({0,2,0},{0,-1,0},5,hit));
    EXPECT_EQ(hit.body,body);
    EXPECT_NEAR(hit.distance,2,1e-4f);
    EXPECT_EQ(hit.faceIndex,0u);
}

TEST(PhysicsWorldTests, ReadAccessRetainsSceneAndConcurrentQueriesUseIndependentBuffers)
{
    auto world = World(); ASSERT_NE(world,nullptr);
    ASSERT_TRUE(world->AddSphere(1).IsValid());
    auto access = world->Read();
    world.reset();
    auto first = std::async(std::launch::async,[&] { QueryHit hit; return access->Raycast({0,0,3},{0,0,-1},10,hit); });
    auto second = std::async(std::launch::async,[&] { QueryHit hit; return access->Raycast({0,3,0},{0,-1,0},10,hit); });
    EXPECT_TRUE(first.get()); EXPECT_TRUE(second.get());
}

TEST(PhysicsWorldTests, CookedFaceIndicesReferToSourceTriangles)
{
    auto world = World(); ASSERT_NE(world,nullptr);
    const std::array<NLS::Maths::Vector3,6> vertices{{{8,0,-2},{10,0,2},{12,0,-2},{-2,0,-2},{0,0,2},{2,0,-2}}};
    const std::array<uint32_t,6> triangles{0,1,2,3,4,5};
    ASSERT_TRUE(world->AddTriangleMesh(vertices,triangles).IsValid());
    QueryHit hit;
    ASSERT_TRUE(world->Read()->Raycast({0,2,0},{0,-1,0},5,hit));
    EXPECT_EQ(hit.faceIndex,1u);
    QueryFilter filter; filter.includeStatic = filter.includeDynamic = false;
    EXPECT_FALSE(world->Read(filter)->Raycast({0,2,0},{0,-1,0},5,hit));
}

TEST(PhysicsWorldTests, CompoundBodyKeepsIndependentShapeIdentityLayersAndMass)
{
    auto world = World(); ASSERT_TRUE(world);
    BodySettings bodySettings; bodySettings.dynamic = true; bodySettings.kinematic = false; bodySettings.useGravity = false; bodySettings.mass = 5;
    const auto body = world->CreateBody(bodySettings); ASSERT_TRUE(body.IsValid());
    ShapeSettings first; first.localPosition = {-1,0,0}; first.layer = 4; first.owner = 101;
    const auto left = world->AttachSphere(body, 0.5f, first); ASSERT_TRUE(left.IsValid());
    ShapeSettings second; second.localPosition = {1,0,0}; second.layer = 8; second.owner = 102;
    const auto right = world->AttachBox(body, {0.5f,0.5f,0.5f}, second); ASSERT_TRUE(right.IsValid());
    QueryHit hit; QueryFilter filter; filter.layers = 4;
    ASSERT_TRUE(world->Read(filter)->Raycast({-1,3,0},{0,-1,0},5,hit));
    EXPECT_EQ(hit.body, body); EXPECT_EQ(hit.shape, left); EXPECT_EQ(hit.owner, 101u);
    EXPECT_FALSE(world->Read(filter)->Raycast({1,3,0},{0,-1,0},5,hit));
    EXPECT_EQ(world->Read()->OverlapSphere(3,{0,0,0}).size(), 1u);
    BodyState state; ASSERT_TRUE(world->GetState(body, state)); EXPECT_NEAR(state.mass, 5, 1e-5f); EXPECT_GT(state.inertia.y, 0);
    ASSERT_TRUE(world->Remove(left)); EXPECT_FALSE(world->IsValid(left)); EXPECT_TRUE(world->IsValid(right));
    EXPECT_TRUE(world->IsValid(body)); EXPECT_EQ(world->GetStatistics().shapeCount, 1u);
    ASSERT_TRUE(world->Remove(body)); EXPECT_FALSE(world->IsValid(right)); EXPECT_EQ(world->GetStatistics().bodyCount, 0u);
}

TEST(PhysicsWorldTests, AllForceModesUseRealMassAndTorqueUsesInertia)
{
    for (const auto mode : {ForceMode::Force, ForceMode::Acceleration, ForceMode::Impulse, ForceMode::VelocityChange})
    {
        auto world = World(); ASSERT_TRUE(world);
        BodySettings settings; settings.dynamic = true; settings.kinematic = false; settings.useGravity = false;
        settings.mass = 2; settings.angularDamping = 0;
        const auto body = world->AddBox({0.5f,0.5f,0.5f},settings);
        ASSERT_TRUE(world->ApplyForce(body,{10,0,0},mode)); world->Step(0.1f);
        const float expected = mode == ForceMode::Force ? 0.5f : mode == ForceMode::Acceleration ? 1.0f : mode == ForceMode::Impulse ? 5.0f : 10.0f;
        EXPECT_NEAR(world->GetVelocity(body).x, expected, 1e-4f);
        ASSERT_TRUE(world->AddTorque(body,{0,0,1},ForceMode::VelocityChange));
        world->Step(0.1f);
        BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.angularVelocity.z,1,1e-4f);
    }
}

TEST(PhysicsWorldTests, LocksSleepAndKinematicTargetsHaveSeparateBehavior)
{
    auto world = World(); ASSERT_TRUE(world);
    BodySettings settings; settings.dynamic = true; settings.kinematic = false; settings.lockPositionY = true; settings.lockRotationZ = true;
    const auto body = world->AddBox({0.5f,0.5f,0.5f},settings);
    ASSERT_TRUE(world->AddTorque(body,{0,0,10},ForceMode::Impulse)); world->Step(0.1f);
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.position.y,0,1e-5f); EXPECT_NEAR(state.angularVelocity.z,0,1e-5f);
    ASSERT_TRUE(world->SetAwake(body,false)); ASSERT_TRUE(world->GetState(body,state)); EXPECT_FALSE(state.awake);
    ASSERT_TRUE(world->SetAwake(body,true));
    settings.kinematic = true;
    const auto kinematic = world->AddSphere(0.5f,settings);
    EXPECT_FALSE(world->ApplyForce(kinematic,{1,0,0},ForceMode::Impulse));
    ASSERT_TRUE(world->SetKinematicTarget(kinematic,{3,0,0},NLS::Maths::Quaternion::Identity)); world->Step(0.1f);
    ASSERT_TRUE(world->GetState(kinematic,state)); EXPECT_NEAR(state.position.x,3,1e-4f);
}

TEST(PhysicsWorldTests, ConvexCookingSupportsDynamicBodiesAndConcaveTrianglesAreRejected)
{
    auto world = World(); ASSERT_TRUE(world);
    const std::array<NLS::Maths::Vector3,8> vertices{{{-1,-1,-1},{1,-1,-1},{-1,1,-1},{1,1,-1},{-1,-1,1},{1,-1,1},{-1,1,1},{1,1,1}}};
    BodySettings settings; settings.dynamic = true; settings.kinematic = false;
    const auto body = world->CreateBody(settings); ASSERT_TRUE(world->AttachConvexMesh(body,vertices).IsValid());
    const std::array<uint32_t,3> indices{0,1,2};
    EXPECT_FALSE(world->AttachTriangleMesh(body,vertices,indices).IsValid());
    world->Step(0.1f); BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_LT(state.position.y,0);
    QueryHit hit; EXPECT_TRUE(world->Read()->Raycast({0,3,0},{0,-1,0},10,hit));
}

TEST(PhysicsWorldTests, SimulationMaskDoesNotChangeLegacyQueryLayerSemantics)
{
    auto world = World(); ASSERT_TRUE(world);
    ShapeSettings shape; shape.layer = 8; shape.collisionMask = 0;
    ASSERT_TRUE(world->AttachBox(world->CreateBody(),{1,1,1},shape).IsValid());
    QueryFilter filter; filter.layers = 8; QueryHit hit;
    EXPECT_TRUE(world->Read(filter)->Raycast({0,3,0},{0,-1,0},10,hit));
    BodySettings dynamic; dynamic.dynamic = true; dynamic.kinematic = false; dynamic.position = {0,3,0};
    const auto body = world->AddSphere(0.5f,dynamic);
    for (int i=0;i<60;++i) world->Step(1.0f/60);
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_LT(state.position.y,-1);
}

TEST(PhysicsWorldTests, ColliderPairEventsAggregateCompoundShapesAndContinueDuringSleep)
{
    auto world = World(); ASSERT_TRUE(world);
    ShapeSettings floorShape; floorShape.owner = 10; floorShape.restitution = 0;
    ASSERT_TRUE(world->AttachBox(world->CreateBody(),{5,0.5f,5},floorShape).IsValid());
    BodySettings settings; settings.dynamic = true; settings.kinematic = false; settings.position = {0,1.6f,0};
    const auto body = world->CreateBody(settings);
    ShapeSettings shape; shape.owner = 20; shape.restitution = 0; shape.localPosition = {-0.6f,0,0};
    ASSERT_TRUE(world->AttachBox(body,{0.5f,0.5f,0.5f},shape).IsValid());
    shape.localPosition = {0.6f,0,0}; ASSERT_TRUE(world->AttachBox(body,{0.5f,0.5f,0.5f},shape).IsValid());
    size_t enters=0; bool sawData=false;
    for(int i=0;i<120;++i)
    {
        world->Step(1.0f/60); const auto events=world->TakeContactEvents();
        EXPECT_LE(events.size(),1u);
        for(const auto& event:events)
        {
            enters += event.phase==ContactPhase::Enter;
            EXPECT_EQ(event.firstOwner,10u); EXPECT_EQ(event.secondOwner,20u);
            EXPECT_EQ(event.stepIndex,world->GetStatistics().stepIndex);
            sawData |= event.hasContactData && event.pointCount>0;
        }
    }
    EXPECT_EQ(enters,1u); EXPECT_TRUE(sawData);
    ASSERT_TRUE(world->SetAwake(body,false)); world->Step(1.0f/60);
    auto events=world->TakeContactEvents(); ASSERT_EQ(events.size(),1u); EXPECT_EQ(events[0].phase,ContactPhase::Stay);
    ASSERT_TRUE(world->Remove(body)); world->Step(1.0f/60);
    events=world->TakeContactEvents(); ASSERT_EQ(events.size(),1u); EXPECT_EQ(events[0].phase,ContactPhase::Exit); EXPECT_FALSE(events[0].hasContactData);
}

TEST(PhysicsWorldTests, SnapshotRestoresPoseVelocitySleepAndStepButRejectsChangedTopology)
{
    auto world = World(); ASSERT_TRUE(world);
    BodySettings settings; settings.dynamic = true; settings.kinematic = false; settings.velocity = {1,0,0};
    const auto body=world->AddSphere(0.5f,settings); world->Step(0.1f);
    const auto saved=world->CaptureState(); world->Step(0.1f);
    std::string error; ASSERT_TRUE(world->RestoreState(saved,error)) << error;
    BodyState state; ASSERT_TRUE(world->GetState(body,state));
    EXPECT_NEAR(state.position.x,saved.bodies[0].position.x,1e-6f); EXPECT_NEAR(state.velocity.y,saved.bodies[0].velocity.y,1e-6f);
    EXPECT_EQ(world->GetStatistics().stepIndex,saved.stepIndex);
    auto duplicate=saved; duplicate.bodies.push_back(saved.bodies[0]); EXPECT_FALSE(world->RestoreState(duplicate,error));
    ASSERT_TRUE(world->AttachSphere(body,0.25f).IsValid()); EXPECT_FALSE(world->RestoreState(saved,error));
    auto other=World(); ASSERT_TRUE(other); EXPECT_FALSE(other->RestoreState(saved,error));
}

TEST(PhysicsWorldTests, SharedTaskGraphCompletesPhysXWorkBeforeWorldRelease)
{
    NLS::Base::TaskGraph::TaskGraphConfig config; config.workerCount=2; config.retainedTerminalTaskCount=0;
    auto tasks=std::make_shared<NLS::Base::TaskGraph::TaskGraph>(config); ASSERT_TRUE(tasks->Start());
    WorldSettings settings; settings.tasks=tasks;
    std::string error; auto world=PhysicsWorld::Create(settings,error); ASSERT_TRUE(world) << error;
    BodySettings bodySettings; bodySettings.dynamic=true; bodySettings.kinematic=false;
    for(int i=0;i<128;++i) { bodySettings.position={float(i%16)*2,3+float(i/16)*2,0}; ASSERT_TRUE(world->AddSphere(0.5f,bodySettings).IsValid()); }
    world->Step(1.0f/60); world.reset();
    EXPECT_EQ(tasks->GetPendingTaskCount(),0u); EXPECT_EQ(tasks->GetRunningTaskCount(),0u); EXPECT_TRUE(tasks->IsStarted());
    tasks->Shutdown();
}

TEST(PhysicsWorldTests, CallerOwnedQueryBuffersReportTruncationAndKeepNearestHits)
{
    auto world=World(); ASSERT_TRUE(world);
    for(int i=0;i<130;++i) { BodySettings settings; settings.position={0,0,float(i)*3}; ASSERT_TRUE(world->AddSphere(0.5f,settings).IsValid()); }
    std::array<QueryHit,3> buffer;
    auto read=world->Read(); const auto all=read->RaycastAll({0,0,-2},{0,0,1},500);
    const auto limited=read->RaycastNonAlloc({0,0,-2},{0,0,1},500,buffer);
    EXPECT_EQ(limited.count,3u); EXPECT_EQ(limited.total,130u); EXPECT_TRUE(limited.truncated);
    for(size_t i=0;i<buffer.size();++i) { EXPECT_EQ(buffer[i].shape,all[i].shape); EXPECT_FLOAT_EQ(buffer[i].distance,all[i].distance); }
    const auto count=read->RaycastNonAlloc({0,0,-2},{0,0,1},500,{}); EXPECT_EQ(count.count,0u); EXPECT_EQ(count.total,130u); EXPECT_TRUE(count.truncated);
    const auto overlap=read->OverlapSphereNonAlloc(500,{0,0,0},buffer);
    EXPECT_EQ(overlap.count,3u); EXPECT_EQ(overlap.total,130u); EXPECT_TRUE(overlap.truncated); EXPECT_FALSE(buffer[0].hasPointAndNormal);
    const auto empty=read->RaycastNonAlloc({100,0,-2},{0,0,1},500,buffer); EXPECT_EQ(empty.count,0u); EXPECT_FALSE(empty.truncated);
}

TEST(PhysicsWorldTests, SdkCylinderRetainsCallbacksForQueriesDynamicsAndRemoval)
{
    auto world=World(); ASSERT_TRUE(world);
    ShapeSettings floor; floor.restitution=0;
    ASSERT_TRUE(world->AttachBox(world->CreateBody(),{5,0.5f,5},floor).IsValid());
    BodySettings settings; settings.dynamic=true; settings.kinematic=false; settings.position={0,3,0};
    const auto body=world->CreateBody(settings); ShapeSettings shape; shape.restitution=0;
    const auto cylinder=world->AttachCylinder(body,0.5f,1,shape); ASSERT_TRUE(cylinder.IsValid());
    QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({0,6,0},{0,-1,0},10,hit));
    EXPECT_EQ(hit.shape,cylinder); EXPECT_NEAR(hit.position.y,4,0.01f);
    for(int i=0;i<180;++i) world->Step(1.0f/60);
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.position.y,1.5f,0.05f); EXPECT_GT(state.inertia.y,0);
    ASSERT_TRUE(world->Remove(cylinder)); EXPECT_FALSE(world->IsValid(cylinder)); EXPECT_TRUE(world->IsValid(body));
    const auto replacement=world->AttachCylinder(body,0.4f,0.8f,shape); ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.index,cylinder.index); EXPECT_NE(replacement.generation,cylinder.generation);
}

TEST(PhysicsWorldTests, HeightFieldUsesCallerSamplesAndRejectsDynamicOrInvalidGrids)
{
    auto world=World(); ASSERT_TRUE(world);
    const auto terrain=world->CreateBody(); const std::array<float,9> heights{2,2,2,2,2,2,2,2,2};
    const auto field=world->AttachHeightField(terrain,3,3,heights,2,3); ASSERT_TRUE(field.IsValid());
    QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({1,6,1},{0,-1,0},10,hit)); EXPECT_EQ(hit.shape,field); EXPECT_NEAR(hit.position.y,2,0.001f);
    BodySettings settings; settings.dynamic=true; settings.kinematic=false; settings.position={1,4,1};
    const auto body=world->AddSphere(0.5f,settings);
    EXPECT_FALSE(world->AttachHeightField(body,3,3,heights).IsValid());
    EXPECT_FALSE(world->AttachHeightField(terrain,4,3,heights).IsValid());
    EXPECT_FALSE(world->AttachHeightField(terrain,3,3,heights,0,1).IsValid());
    for(int i=0;i<180;++i) world->Step(1.0f/60);
    BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.position.y,2.5f,0.05f);
}

TEST(PhysicsWorldTests, ContinuousCollisionStopsAFastBodyAtAThinWall)
{
    auto world=World(); ASSERT_TRUE(world);
    ShapeSettings shape; shape.restitution=0;
    ASSERT_TRUE(world->AttachBox(world->CreateBody(),{0.025f,5,5},shape).IsValid());
    BodySettings settings; settings.dynamic=true; settings.kinematic=false; settings.useGravity=false;
    settings.continuousCollision=true; settings.position={-3,0,0}; settings.velocity={100,0,0};
    const auto body=world->CreateBody(settings); ASSERT_TRUE(world->AttachSphere(body,0.1f,shape).IsValid());
    world->Step(0.1f); BodyState state; ASSERT_TRUE(world->GetState(body,state));
    EXPECT_LT(state.position.x,-0.1f);
}
