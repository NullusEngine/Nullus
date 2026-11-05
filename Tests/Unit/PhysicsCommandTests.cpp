#include <gtest/gtest.h>
#include <Physics/PhysicsCommandBuffer.h>
#include <array>
#include <chrono>
#include <future>
#include <limits>
#include <thread>
#include <set>

namespace
{
using namespace NLS::Physics;
using NLS::Maths::Vector2;
using NLS::Maths::Vector3;
std::shared_ptr<PhysicsWorld> CommandWorld3D()
{
    std::string error; auto world=PhysicsWorld::Create(error); EXPECT_TRUE(world) << error; return world;
}
std::shared_ptr<PhysicsWorld2D> CommandWorld2D()
{
    std::string error; auto world=PhysicsWorld2D::Create({},error); EXPECT_TRUE(world) << error; return world;
}
}

TEST(PhysicsCommandTests, DefersThreeDimensionalBodyAndCompoundShapesInRecordedOrder)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world);
    PhysicsCommandBuffer commands(world);
    BodySettings bodySettings; bodySettings.dynamic=true; bodySettings.kinematic=false; bodySettings.useGravity=false; bodySettings.velocity={2,0,0};
    const auto body=commands.CreateBody(bodySettings);
    CollisionGeometry3D geometry; ShapeSettings shape; shape.localPosition={-1,0,0}; shape.owner=72;
    const auto first=commands.AttachShape(body,geometry,shape);
    shape.localPosition={1,0,0}; const auto second=commands.AttachShape(body,geometry,shape);
    EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(commands.GetPendingCount(),3u);
    EXPECT_EQ(commands.GetResult(body).status,PhysicsCommandStatus::Pending);
    const auto report=commands.Commit(); EXPECT_EQ(report.applied,3u); EXPECT_EQ(report.failed,0u);
    EXPECT_EQ(world->GetStatistics().bodyCount,1u); EXPECT_EQ(world->GetStatistics().shapeCount,2u);
    const auto handle=commands.GetResult(body).body; ASSERT_TRUE(handle.IsValid());
    EXPECT_TRUE(commands.GetResult(first).shape.IsValid()); EXPECT_TRUE(commands.GetResult(second).shape.IsValid());
    world->Step(0.1f); BodyState state; ASSERT_TRUE(world->GetState(handle,state)); EXPECT_NEAR(state.position.x,0.2f,1e-5f);
    EXPECT_EQ(commands.Commit().processed,0u);
    commands.RemoveShape(first); commands.RemoveBody(body); const auto removed=commands.Commit();
    EXPECT_EQ(removed.applied,2u); EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(world->GetStatistics().shapeCount,0u);
}

TEST(PhysicsCommandTests, CompoundReplacementPublishesStablePartIdsAndInvalidatesTheEntireOldGroup)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); const auto body=world->CreateBody();
    const auto old=world->AttachBox(body,{4,0.5f,0.5f}); PhysicsCommandBuffer commands(world);
    PhysicsShapeDescription3D a,b; a.settings.owner=b.settings.owner=42;
    a.settings.subShapeId=7; b.settings.subShapeId=19; a.settings.localPosition={-2,0,0}; b.settings.localPosition={2,0,0};
    std::vector<PhysicsShapeDescription3D> input{a,b}; const auto ticket=commands.ReplaceShapes(body,{old},input);
    input[0].geometry.halfExtents={}; input.clear(); EXPECT_TRUE(world->IsValid(old));
    ASSERT_EQ(commands.Commit().applied,1u); const auto result=commands.GetResult(ticket); ASSERT_EQ(result.shapes.size(),2u);
    EXPECT_EQ(result.shape,result.shapes.front()); EXPECT_FALSE(world->IsValid(old)); EXPECT_EQ(world->GetStatistics().shapeCount,2u);
    QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({-2,0,-3},{0,0,1},6,hit)); EXPECT_EQ(hit.subShapeId,7u); EXPECT_EQ(hit.owner,42u);
    ASSERT_TRUE(world->Read()->SweepSphere(0.1f,{2,0,-3},{0,0,1},6,hit)); EXPECT_EQ(hit.subShapeId,19u);
    std::array<QueryHit,1> limited; const auto overlap=world->Read()->OverlapSphereNonAlloc(5,{0,0,0},limited);
    EXPECT_EQ(overlap.total,2u); EXPECT_TRUE(overlap.truncated); EXPECT_TRUE(limited[0].subShapeId==7 || limited[0].subShapeId==19);
    for(auto shape:result.shapes) EXPECT_EQ(world->GetShapeBody(shape),body);
    commands.RemoveShape(ticket); ASSERT_EQ(commands.Commit().applied,1u); EXPECT_FALSE(world->IsValid(result.shape));
    EXPECT_TRUE(world->IsValid(result.shapes.back()));
}

TEST(PhysicsCommandTests, FailedCompoundAttachmentRollsBackNewGeometryWithoutLosingMassVelocityOrForce)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); BodySettings settings; settings.dynamic=true; settings.kinematic=false;
    settings.useGravity=false; settings.mass=2; settings.velocity={3,0,0}; settings.linearDamping=0;
    const auto body=world->CreateBody(settings); const auto old=world->AttachBox(body,{1,0.5f,0.5f});
    BodyState before; ASSERT_TRUE(world->GetState(body,before)); ASSERT_TRUE(world->AddForce(body,{4,0,0}));
    PhysicsShapeDescription3D valid,bad; valid.settings.subShapeId=1; valid.geometry.halfExtents={2,1,1};
    bad.settings.subShapeId=2; bad.geometry.type=CollisionGeometryType3D::Sphere; bad.geometry.radius=-1;
    PhysicsCommandBuffer commands(world); const auto ticket=commands.ReplaceShapes(body,{old},{valid,bad});
    EXPECT_EQ(commands.Commit().failed,1u); EXPECT_EQ(commands.GetResult(ticket).error,PhysicsCommandError::BackendRejected);
    EXPECT_TRUE(world->IsValid(old)); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
    BodyState after; ASSERT_TRUE(world->GetState(body,after)); EXPECT_FLOAT_EQ(after.mass,before.mass);
    EXPECT_NEAR(after.inertia.x,before.inertia.x,1e-5f); EXPECT_NEAR(after.inertia.y,before.inertia.y,1e-5f);
    EXPECT_FLOAT_EQ(after.velocity.x,3); world->Step(0.1f); EXPECT_NEAR(world->GetVelocity(body).x,3.2f,1e-5f);
}

TEST(PhysicsCommandTests, ReplacementRejectsOtherBodiesStaleHandlesAndDuplicatePartIdentitiesInBothDimensions)
{
    auto world=CommandWorld3D(); auto flat=CommandWorld2D(); ASSERT_TRUE(world); ASSERT_TRUE(flat);
    const auto body=world->CreateBody(),other=world->CreateBody(); const auto shape=world->AttachSphere(other,0.5f);
    PhysicsCommandBuffer commands(world); commands.ReplaceShape(body,shape,CollisionGeometry3D{});
    commands.ReplaceShapes(body,{shape},{PhysicsShapeDescription3D{}});
    commands.ReplaceShapes(other,{shape,shape},{PhysicsShapeDescription3D{}});
    PhysicsShapeDescription3D part; part.settings.owner=4; part.settings.subShapeId=12;
    commands.ReplaceShapes(other,{shape},{part,part}); EXPECT_EQ(commands.Commit().failed,4u);
    EXPECT_TRUE(world->IsValid(shape)); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
    ASSERT_TRUE(world->Remove(shape)); commands.ReplaceShapes(other,{shape},{part}); EXPECT_EQ(commands.Commit().failed,1u);
    const auto a=flat->CreateBody(),b=flat->CreateBody(); const auto planar=flat->AddCircle(b,0.5f);
    PhysicsCommandBuffer planarCommands(flat); planarCommands.ReplaceShape(a,planar,CollisionGeometry2D{});
    EXPECT_EQ(planarCommands.Commit().failed,1u); EXPECT_TRUE(flat->IsValid(planar)); EXPECT_EQ(flat->GetShapeBody(planar),b);
    auto foreign=CommandWorld3D(); const auto foreignShape=foreign->AttachSphere(foreign->CreateBody(),0.5f);
    commands.ReplaceShapes(body,{foreignShape},{part}); EXPECT_EQ(commands.Commit().failed,1u); EXPECT_TRUE(foreign->IsValid(foreignShape));
}

TEST(PhysicsCommandTests, CompoundLimitsAndEmptyReplacementPreserveUnrelatedShapes)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); const auto body=world->CreateBody();
    const auto old=world->AttachBox(body,{0.5f,0.5f,0.5f}),unrelated=world->AttachSphere(body,0.2f);
    PhysicsCommandBuffer commands(world); commands.ReplaceShapes(body,{old},std::vector<PhysicsShapeDescription3D>(MaxCompoundShapes3D+1));
    EXPECT_EQ(commands.Commit().failed,1u); EXPECT_TRUE(world->IsValid(old));
    const auto cleared=commands.ReplaceShapes(body,{old},{}); EXPECT_EQ(commands.Commit().applied,1u);
    const auto result=commands.GetResult(cleared); EXPECT_TRUE(result.shapes.empty()); EXPECT_FALSE(result.shape.IsValid());
    EXPECT_FALSE(world->IsValid(old)); EXPECT_TRUE(world->IsValid(unrelated)); EXPECT_TRUE(world->IsValid(body));
}

TEST(PhysicsCommandTests, ReadViewsSeeWholeCompoundGroupsAcrossCommit)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); const auto body=world->CreateBody();
    const auto old=world->AttachBox(body,{3,0.5f,0.5f}); PhysicsShapeDescription3D a,b;
    a.settings.localPosition={-1,0,0}; b.settings.localPosition={1,0,0}; PhysicsCommandBuffer commands(world);
    commands.ReplaceShapes(body,{old},{a,b}); auto held=world->Read(); std::promise<void> started;
    auto consumer=std::async(std::launch::async,[&]{started.set_value(); return commands.Commit();}); started.get_future().wait();
    EXPECT_EQ(consumer.wait_for(std::chrono::milliseconds(25)),std::future_status::timeout);
    std::array<QueryHit,4> buffer; EXPECT_EQ(held->OverlapSphereNonAlloc(5,{0,0,0},buffer).total,1u); EXPECT_EQ(buffer[0].shape,old);
    held.reset(); EXPECT_EQ(consumer.get().applied,1u); EXPECT_EQ(world->Read()->OverlapSphereNonAlloc(5,{0,0,0},buffer).total,2u);
}

TEST(PhysicsCommandTests, CompoundContactPairsAggregateOwnersAndKeepRepresentativeSubshapeIdsThroughSleepAndExit)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); ShapeSettings floor; floor.owner=900; floor.subShapeId=90; floor.restitution=0;
    ASSERT_TRUE(world->AttachBox(world->CreateBody(),{5,0.5f,5},floor).IsValid());
    BodySettings settings; settings.dynamic=true; settings.kinematic=false; settings.position={0,0.99f,0};
    const auto body=world->CreateBody(settings); PhysicsShapeDescription3D a,b; a.settings.owner=b.settings.owner=20;
    a.settings.subShapeId=7; b.settings.subShapeId=19; a.settings.localPosition={-1,0,0}; b.settings.localPosition={1,0,0};
    a.settings.restitution=b.settings.restitution=0; PhysicsCommandBuffer commands(world); const auto ticket=commands.ReplaceShapes(body,{}, {a,b});
    ASSERT_EQ(commands.Commit().applied,1u); world->Step(1.f/60); auto events=world->TakeContactEvents(); ASSERT_EQ(events.size(),1u);
    EXPECT_EQ(events[0].phase,ContactPhase::Enter); EXPECT_EQ(events[0].firstOwner,20u); EXPECT_EQ(events[0].secondOwner,900u);
    EXPECT_TRUE(events[0].firstSubShapeId==7 || events[0].firstSubShapeId==19); EXPECT_EQ(events[0].secondSubShapeId,90u); EXPECT_TRUE(events[0].hasContactData);
    for(int i=0;i<120;++i) { world->Step(1.f/60); events=world->TakeContactEvents(); ASSERT_EQ(events.size(),1u); EXPECT_EQ(events[0].phase,ContactPhase::Stay); }
    ASSERT_TRUE(world->SetAwake(body,false)); world->Step(1.f/60); events=world->TakeContactEvents(); ASSERT_EQ(events.size(),1u);
    EXPECT_EQ(events[0].phase,ContactPhase::Stay); EXPECT_EQ(events[0].secondSubShapeId,90u);
    ASSERT_TRUE(world->Remove(body)); world->Step(1.f/60); events=world->TakeContactEvents(); ASSERT_EQ(events.size(),1u);
    EXPECT_EQ(events[0].phase,ContactPhase::Exit); EXPECT_EQ(events[0].secondSubShapeId,90u); EXPECT_FALSE(events[0].hasContactData);
}

TEST(PhysicsCommandTests, WorldConfigurationWaitsForCommitAndChangesRealSolverMotion)
{
    auto world=CommandWorld3D(); auto planar=CommandWorld2D(); ASSERT_TRUE(world); ASSERT_TRUE(planar);
    ASSERT_TRUE(world->SetGravity({0,0,0})); ASSERT_TRUE(planar->SetGravity({0,0}));
    BodySettings spatial; spatial.dynamic=true; spatial.kinematic=false;
    const auto body=world->AddBox({0.5f,0.5f,0.5f},spatial);
    BodySettings2D flat; flat.type=BodyType2D::Dynamic;
    const auto body2D=planar->CreateBody(flat); ASSERT_TRUE(planar->AddBox(body2D,{0.5f,0.5f}).IsValid());
    PhysicsCommandBuffer commands(world), commands2D(planar);
    const auto revision=world->GetRevision(), revision2D=planar->GetRevision();
    {
        const auto read=world->Read(); const auto read2D=planar->Read();
        commands.SetGravity(Vector3{2,-3,4}); commands2D.SetGravity(Vector2{-4,6}); commands2D.SetSolverSubsteps(8);
        EXPECT_EQ(world->GetRevision(),revision); EXPECT_EQ(planar->GetRevision(),revision2D);
        EXPECT_EQ(planar->GetSolverSubsteps(),4u);
    }
    EXPECT_EQ(commands.Commit().applied,1u); EXPECT_EQ(commands2D.Commit().applied,2u);
    EXPECT_EQ(planar->GetSolverSubsteps(),8u); world->Step(0.1f); planar->Step(0.1f);
    const auto velocity=world->GetVelocity(body); EXPECT_NEAR(velocity.x,0.2f,1e-5f);
    EXPECT_NEAR(velocity.y,-0.3f,1e-5f); EXPECT_NEAR(velocity.z,0.4f,1e-5f);
    BodyState2D state; ASSERT_TRUE(planar->GetState(body2D,state));
    EXPECT_NEAR(state.velocity.x,-0.4f,1e-5f); EXPECT_NEAR(state.velocity.y,0.6f,1e-5f);
}

TEST(PhysicsCommandTests, RejectsInvalidAndWrongDimensionWorldConfiguration)
{
    auto world=CommandWorld3D(); auto planar=CommandWorld2D(); ASSERT_TRUE(world); ASSERT_TRUE(planar);
    PhysicsCommandBuffer commands(world), commands2D(planar);
    const auto wrongGravity=commands.SetGravity(Vector2{0,0}); const auto wrongSolver=commands.SetSolverSubsteps(4);
    commands.SetGravity(Vector3{0,std::numeric_limits<float>::quiet_NaN(),0});
    const auto wrong2D=commands2D.SetGravity(Vector3{0,0,0});
    commands2D.SetSolverSubsteps(0); commands2D.SetSolverSubsteps(65);
    commands2D.SetGravity(Vector2{std::numeric_limits<float>::infinity(),0});
    EXPECT_EQ(commands.Commit().failed,3u); EXPECT_EQ(commands2D.Commit().failed,4u);
    EXPECT_EQ(commands.GetResult(wrongGravity).error,PhysicsCommandError::WrongDimension);
    EXPECT_EQ(commands.GetResult(wrongSolver).error,PhysicsCommandError::WrongDimension);
    EXPECT_EQ(commands2D.GetResult(wrong2D).error,PhysicsCommandError::WrongDimension);
    EXPECT_EQ(planar->GetSolverSubsteps(),4u);
}

TEST(PhysicsCommandTests, DefersTwoDimensionalBodiesAndUsesMassAfterShapeAssembly)
{
    auto world=CommandWorld2D(); ASSERT_TRUE(world); PhysicsCommandBuffer commands(world);
    BodySettings2D settings; settings.type=BodyType2D::Dynamic; settings.gravityScale=0;
    const auto body=commands.CreateBody(settings); CollisionGeometry2D geometry;
    commands.AttachShape(body,geometry); ShapeSettings2D shape; shape.offset={2,0}; commands.AttachShape(body,geometry,shape);
    EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(commands.Commit().applied,3u);
    BodyState2D state; const auto handle=commands.GetResult(body).body2D;
    ASSERT_TRUE(world->GetState(handle,state)); EXPECT_NEAR(state.mass,2,1e-5f);
    ASSERT_TRUE(world->AddForce(handle,{2,0},ForceMode2D::Impulse)); world->Step(0.1f);
    ASSERT_TRUE(world->GetState(handle,state)); EXPECT_NEAR(state.velocity.x,1,1e-5f);
    commands.RemoveBody(body); EXPECT_EQ(commands.Commit().applied,1u); EXPECT_EQ(world->GetStatistics().shapeCount,0u);
}

TEST(PhysicsCommandTests, CopiesMeshInputAndRetainsOldShapeWhenReplacementIsRejected)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); PhysicsCommandBuffer commands(world);
    const auto body=commands.CreateBody(); CollisionGeometry3D mesh; mesh.type=CollisionGeometryType3D::TriangleMesh;
    mesh.vertices={{-2,0,-2},{0,0,2},{2,0,-2}}; mesh.indices={0,1,2};
    const auto shape=commands.AttachShape(body,mesh);
    mesh.vertices.clear(); mesh.indices={0,0,900};
    EXPECT_EQ(commands.Commit().applied,2u);
    QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_NEAR(hit.position.y,0,1e-5f);
    const auto original=commands.GetResult(shape).shape;
    const auto rejected=commands.ReplaceShape(body,shape,mesh); const auto report=commands.Commit();
    EXPECT_EQ(report.failed,1u); EXPECT_EQ(commands.GetResult(rejected).error,PhysicsCommandError::BackendRejected);
    ASSERT_TRUE(world->Read()->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_EQ(hit.shape,original);
    CollisionGeometry3D box; const auto replacement=commands.ReplaceShape(body,shape,box);
    EXPECT_EQ(commands.Commit().applied,1u); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
    ASSERT_TRUE(world->Read()->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_EQ(hit.shape,commands.GetResult(replacement).shape);
    EXPECT_NEAR(hit.position.y,0.5f,1e-5f);
}

TEST(PhysicsCommandTests, RejectsBadPolygonAndFailedDependenciesBeforeCallingBox2D)
{
    auto world=CommandWorld2D(); ASSERT_TRUE(world); PhysicsCommandBuffer commands(world);
    const auto body=commands.CreateBody(BodySettings2D{});
    CollisionGeometry2D polygon; polygon.type=CollisionGeometryType2D::ConvexPolygon;
    polygon.vertices={{-1,-1},{1,-1},{0,0},{1,1},{-1,1}};
    const auto invalid=commands.AttachShape(body,polygon);
    const auto dependent=commands.RemoveShape(invalid);
    BodySettings2D bad; bad.position.x=std::numeric_limits<float>::quiet_NaN();
    const auto failedBody=commands.CreateBody(bad); const auto failedShape=commands.AttachShape(failedBody,CollisionGeometry2D{});
    const auto report=commands.Commit(); EXPECT_EQ(report.applied,1u); EXPECT_EQ(report.failed,4u);
    EXPECT_EQ(commands.GetResult(dependent).error,PhysicsCommandError::InvalidReference);
    EXPECT_EQ(commands.GetResult(failedShape).error,PhysicsCommandError::InvalidReference);
    EXPECT_EQ(world->GetStatistics().shapeCount,0u);
}

TEST(PhysicsCommandTests, TicketsAndHandlesCannotCrossBuffersDimensionsOrGenerations)
{
    auto a=CommandWorld3D(), b=CommandWorld3D(); ASSERT_TRUE(a); ASSERT_TRUE(b);
    auto two=CommandWorld2D(); ASSERT_TRUE(two);
    PhysicsCommandBuffer first(a), second(b), otherDimension(two);
    const auto ticket=first.CreateBody(); first.Commit();
    const auto foreign=second.AttachShape(ticket,CollisionGeometry3D{});
    const auto wrongDimension=otherDimension.CreateBody(BodySettings{});
    EXPECT_EQ(second.Commit().failed,1u); EXPECT_EQ(otherDimension.Commit().failed,1u);
    EXPECT_EQ(second.GetResult(foreign).error,PhysicsCommandError::InvalidReference);
    EXPECT_EQ(otherDimension.GetResult(wrongDimension).error,PhysicsCommandError::WrongDimension);
    const auto old=first.GetResult(ticket).body; ASSERT_TRUE(a->Remove(old));
    const auto live=a->CreateBody(); ASSERT_EQ(live.index,old.index); EXPECT_NE(live.generation,old.generation);
    first.RemoveBody(old); second.RemoveBody(live);
    EXPECT_EQ(first.Commit().skipped,1u); EXPECT_EQ(second.Commit().skipped,1u); EXPECT_TRUE(a->IsValid(live));
    ASSERT_TRUE(first.Reset()); EXPECT_EQ(first.GetResult(ticket).status,PhysicsCommandStatus::Invalid);
    const auto afterReset=first.AttachShape(ticket,CollisionGeometry3D{}); first.Commit();
    EXPECT_EQ(first.GetResult(afterReset).error,PhysicsCommandError::InvalidReference); EXPECT_TRUE(a->IsValid(live));
}

TEST(PhysicsCommandTests, CancellingPendingWorkAndExpiringWorldsDoNotCreateOrRetainObjects)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); std::weak_ptr<PhysicsWorld> weak=world;
    PhysicsCommandBuffer commands(world); const auto cancelled=commands.CreateBody();
    ASSERT_TRUE(commands.Reset()); EXPECT_EQ(commands.GetResult(cancelled).status,PhysicsCommandStatus::Invalid);
    EXPECT_EQ(commands.Commit().processed,0u); EXPECT_EQ(world->GetStatistics().bodyCount,0u);
    const auto pending=commands.CreateBody(); world.reset(); EXPECT_TRUE(weak.expired());
    EXPECT_EQ(commands.Commit().failed,1u); EXPECT_EQ(commands.GetResult(pending).error,PhysicsCommandError::ExpiredWorld);
    ASSERT_TRUE(commands.Reset()); EXPECT_EQ(commands.GetResultCount(),0u);
}

TEST(PhysicsCommandTests, ConcurrentProducersOnlyRecordAndConsumerAppliesOneSnapshot)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); PhysicsCommandBuffer commands(world);
    std::array<std::future<void>,4> producers;
    for (auto& producer:producers) producer=std::async(std::launch::async,[&] { for (int i=0;i<16;++i) commands.CreateBody(); });
    for (auto& producer:producers) producer.get();
    EXPECT_EQ(world->GetStatistics().bodyCount,0u); EXPECT_EQ(commands.GetPendingCount(),64u);
    const auto report=commands.Commit(); EXPECT_EQ(report.applied,64u); EXPECT_EQ(report.failed,0u);
    EXPECT_EQ(world->GetStatistics().bodyCount,64u);
}

TEST(PhysicsCommandTests, WorkRecordedDuringCommitWaitsForTheNextCommit)
{
    auto world=CommandWorld3D(); ASSERT_TRUE(world); PhysicsCommandBuffer commands(world);
    const auto first=commands.CreateBody();
    auto heldRead=world->Read(); auto consumer=std::async(std::launch::async,[&] { return commands.Commit(); });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!commands.IsCommitting() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    if (!commands.IsCommitting()) { heldRead.reset(); consumer.get(); FAIL() << "Consumer did not enter Commit"; }
    const auto second=commands.CreateBody(); EXPECT_FALSE(commands.Reset()); EXPECT_TRUE(commands.Commit().busy);
    EXPECT_EQ(commands.GetResult(first).status,PhysicsCommandStatus::Pending);
    heldRead.reset(); const auto report=consumer.get(); EXPECT_EQ(report.applied,1u); EXPECT_EQ(report.pending,1u);
    EXPECT_EQ(commands.GetResult(second).status,PhysicsCommandStatus::Pending);
    EXPECT_EQ(commands.Commit().applied,1u); EXPECT_EQ(world->GetStatistics().bodyCount,2u);
}

TEST(PhysicsCommandTests, DynamicsUpdatesPreserveSubmittedForcesAndNativeGeometryOutlivesRecords)
{
    auto world=CommandWorld3D(), direct=CommandWorld3D(); ASSERT_TRUE(world); ASSERT_TRUE(direct); BodyHandle body;
    BodySettings controlSettings; controlSettings.dynamic=true; controlSettings.kinematic=false; controlSettings.useGravity=false;
    const auto control=direct->CreateBody(controlSettings); ASSERT_TRUE(direct->AttachCylinder(control,0.5f,0.5f).IsValid());
    ASSERT_TRUE(direct->AddForce(control,{20,0,0})); controlSettings.mass=2; ASSERT_TRUE(direct->UpdateDynamics(control,controlSettings));
    direct->Step(0.1f); BodyState expected; ASSERT_TRUE(direct->GetState(control,expected));
    {
        PhysicsCommandBuffer commands(world); BodySettings settings; settings.dynamic=true; settings.kinematic=false; settings.useGravity=false;
        const auto created=commands.CreateBody(settings); CollisionGeometry3D cylinder; cylinder.type=CollisionGeometryType3D::Cylinder;
        commands.AttachShape(created,cylinder); EXPECT_EQ(commands.Commit().applied,2u); body=commands.GetResult(created).body;
        ASSERT_TRUE(world->AddForce(body,{20,0,0})); settings.mass=2; commands.UpdateDynamics(body,settings);
        EXPECT_EQ(commands.Commit().applied,1u); ASSERT_TRUE(commands.Reset());
    }
    world->Step(0.1f); BodyState state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.velocity.x,expected.velocity.x,1e-5f);
    EXPECT_GT(state.velocity.x,0); EXPECT_NEAR(state.mass,2,1e-5f);
    QueryHit hit; EXPECT_TRUE(world->Read()->Raycast({state.position.x,3,0},{0,-1,0},10,hit));
}
