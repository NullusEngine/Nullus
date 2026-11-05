#include <gtest/gtest.h>
#include <Physics/Geometry2D.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <future>
#include <chrono>
#include <limits>
#include <set>

namespace
{
using namespace NLS::Physics;
using NLS::Maths::Vector2;
const std::vector<Vector2> Elbow{{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}};
std::shared_ptr<PhysicsWorld2D> GeometryWorld()
{ std::string error; auto world=PhysicsWorld2D::Create(error); EXPECT_TRUE(world)<<error; return world; }
double Area(const std::vector<Vector2>& vertices)
{
    double area=0; for(size_t i=0;i<vertices.size();++i) { const auto a=vertices[i],b=vertices[(i+1)%vertices.size()]; area+=double(a.x)*b.y-double(a.y)*b.x; }
    return std::abs(area)*0.5;
}
std::vector<ShapeHandle2D> Publish(const std::shared_ptr<PhysicsWorld2D>& world,BodyHandle2D body,std::vector<PhysicsShapeDescription2D> parts)
{
    PhysicsCommandBuffer commands(world); const auto ticket=commands.ReplaceShapes2D(body,{},std::move(parts));
    EXPECT_EQ(commands.Commit().failed,0u); return commands.GetResult(ticket).shapes2D;
}
}

TEST(PhysicsGeometry2DTests, ConcavePolygonPreservesItsNotchAndReturnsPerPartQueryIdentity)
{
    std::vector<PhysicsShapeDescription2D> parts; std::string error; ShapeSettings2D settings; settings.owner=73;
    ASSERT_TRUE(BuildPolygon2D(Elbow,settings,parts,error))<<error; ASSERT_GE(parts.size(),2u); EXPECT_LE(parts.size(),4u);
    double area=0; for(const auto& part:parts) area+=Area(part.geometry.vertices); EXPECT_NEAR(area,7,1e-6);
    auto world=GeometryWorld(); ASSERT_TRUE(world); const auto body=world->CreateBody(); const auto shapes=Publish(world,body,parts); ASSERT_EQ(shapes.size(),parts.size());
    QueryHit2D hit; ASSERT_TRUE(world->Read()->Raycast({3,5},{0,-1},10,hit)); EXPECT_NEAR(hit.position.y,1,1e-4f); EXPECT_EQ(hit.owner,73u); EXPECT_GT(hit.subShapeId,0u);
    EXPECT_TRUE(world->Read()->OverlapCircle({3,3},0.1f).empty()); EXPECT_FALSE(world->Read()->CircleCast({3,3},0.1f,{1,0},0.5f,hit));
    ASSERT_TRUE(world->Read()->CircleCast({3,3},0.1f,{-1,0},5,hit)); EXPECT_NEAR(hit.position.x,1,0.02f);
    std::array<QueryHit2D,1> output; const auto result=world->Read()->OverlapCircleNonAlloc({1,1},10,output);
    EXPECT_EQ(result.total,parts.size()); EXPECT_TRUE(result.truncated); EXPECT_GT(output[0].subShapeId,0u);
}

TEST(PhysicsGeometry2DTests, EitherWindingAndCollinearVerticesDecomposeWithoutChangingArea)
{
    auto vertices=Elbow; vertices.insert(vertices.begin()+1,Vector2{2,0});
    std::vector<PhysicsShapeDescription2D> parts; std::string error;
    for(int winding=0;winding<2;++winding)
    {
        ASSERT_TRUE(BuildPolygon2D(vertices,{},parts,error))<<error;
        double total=0; for(const auto& part:parts) { total+=Area(part.geometry.vertices); EXPECT_LE(part.geometry.vertices.size(),8u); }
        EXPECT_NEAR(total,7,1e-6); std::reverse(vertices.begin(),vertices.end());
    }
    // Varying nonconvex contours exercise bounded ear selection and convex merging.
    for(int count=8;count<=48;count+=4)
    {
        vertices.clear(); for(int i=0;i<count;++i) { const float angle=float(i)*6.28318530718f/count,radius=i%2?2.f:3.f; vertices.push_back({radius*std::cos(angle),radius*std::sin(angle)}); }
        ASSERT_TRUE(BuildPolygon2D(vertices,{},parts,error))<<count<<":"<<error;
        double total=0; for(const auto& part:parts) total+=Area(part.geometry.vertices); EXPECT_NEAR(total,Area(vertices),1e-4);
    }
}

TEST(PhysicsGeometry2DTests, MalformedContoursNeverChangeThePreviousOutputOrReachNativeAssertions)
{
    std::vector<PhysicsShapeDescription2D> output(1); output[0].settings.owner=99; std::string error;
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for(const auto& invalid:std::vector<std::vector<Vector2>>{{},{ {0,0},{1,0} },{{0,0},{1,1},{0,1},{1,0}},
        {{0,0},{2,0},{1,0},{1,1}},{{0,0},{1,0},{1,0},{0,1}},{{0,0},{1,0},{1,1},{0,0}},
        {{0,0},{0.001f,0},{0,1}},{{0,0},{1,0},{2,0}},{{0,0},{nan,0},{0,1}},{{0,0},{1e30f,0},{0,1}}})
    {
        EXPECT_FALSE(BuildPolygon2D(invalid,{},output,error)); EXPECT_FALSE(error.empty()); ASSERT_EQ(output.size(),1u); EXPECT_EQ(output[0].settings.owner,99u);
    }
    EXPECT_FALSE(BuildPolygon2D(std::vector<Vector2>(257),{},output,error));
}

TEST(PhysicsGeometry2DTests, ConvexContoursAboveEightVerticesProduceValidBoundedNativeParts)
{
    std::vector<Vector2> points; for(int i=0;i<24;++i) { const float a=i*6.28318530718f/24; points.push_back({2*std::cos(a),2*std::sin(a)}); }
    std::vector<PhysicsShapeDescription2D> parts; std::string error; ASSERT_TRUE(BuildPolygon2D(points,{},parts,error))<<error; EXPECT_GT(parts.size(),1u);
    auto world=GeometryWorld(); ASSERT_TRUE(world); const auto handles=Publish(world,world->CreateBody(),parts); ASSERT_EQ(handles.size(),parts.size());
    double area=0; for(const auto& part:parts) area+=Area(part.geometry.vertices); EXPECT_NEAR(area,Area(points),1e-4);
    QueryHit2D hit; ASSERT_TRUE(world->Read()->Raycast({0,5},{0,-1},10,hit)); EXPECT_NEAR(hit.position.y,2,0.005f);
}

TEST(PhysicsGeometry2DTests, DecomposedDynamicMassAndImpulseUseTheActualConcaveArea)
{
    auto world=GeometryWorld(); ASSERT_TRUE(world); BodySettings2D settings; settings.type=BodyType2D::Dynamic; settings.gravityScale=0;
    const auto body=world->CreateBody(settings); ShapeSettings2D shape; shape.density=2;
    std::vector<PhysicsShapeDescription2D> parts; std::string error; ASSERT_TRUE(BuildPolygon2D(Elbow,shape,parts,error)); Publish(world,body,parts);
    BodyState2D state; ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.mass,14,1e-4f); EXPECT_GT(state.inertia,0);
    ASSERT_TRUE(world->AddForce(body,{28,0},ForceMode2D::Impulse)); world->Step(0.1f); ASSERT_TRUE(world->GetState(body,state)); EXPECT_NEAR(state.velocity.x,2,1e-4f);
}

TEST(PhysicsGeometry2DTests, AtomicGroupRollbackPreservesOldGeometryMotionAndUnconsumedForce)
{
    auto world=GeometryWorld(); ASSERT_TRUE(world); BodySettings2D settings; settings.type=BodyType2D::Dynamic; settings.autoMass=false; settings.mass=2;
    settings.gravityScale=0; settings.velocity={3,0}; settings.angularVelocity=1;
    const auto body=world->CreateBody(settings),other=world->CreateBody(); const auto old=world->AddBox(body,{0.5f,0.5f});
    BodyState2D before; ASSERT_TRUE(world->GetState(body,before)); ASSERT_TRUE(world->AddForce(body,{4,0}));
    PhysicsShapeDescription2D valid,bad; valid.settings.offset={2,0}; valid.settings.subShapeId=1; bad.settings.subShapeId=2; bad.geometry.type=CollisionGeometryType2D::Circle; bad.geometry.radius=-1;
    PhysicsCommandBuffer commands(world); const auto rejected=commands.ReplaceShapes2D(body,{old},{valid,bad});
    EXPECT_EQ(commands.Commit().failed,1u); EXPECT_EQ(commands.GetResult(rejected).error,PhysicsCommandError::BackendRejected); EXPECT_TRUE(world->IsValid(old));
    BodyState2D after; ASSERT_TRUE(world->GetState(body,after)); EXPECT_NEAR(after.mass,before.mass,1e-5f); EXPECT_NEAR(after.inertia,before.inertia,1e-5f); EXPECT_NEAR(after.velocity.x,3,1e-5f); EXPECT_NEAR(after.velocity.y,0,1e-5f);
    commands.ReplaceShapes2D(other,{old},{valid}); commands.ReplaceShapes2D(body,{old,old},{valid}); commands.ReplaceShapes2D(body,{old},{valid,valid});
    commands.ReplaceShapes2D(body,{old},std::vector<PhysicsShapeDescription2D>(MaxCompoundShapes2D+1)); EXPECT_EQ(commands.Commit().failed,4u); EXPECT_TRUE(world->IsValid(old));
    world->Step(0.1f); ASSERT_TRUE(world->GetState(body,after)); EXPECT_NEAR(after.velocity.x,3.2f,1e-4f);
    commands.ReplaceShapes2D(body,{old},{}); EXPECT_EQ(commands.Commit().applied,1u); EXPECT_FALSE(world->IsValid(old)); EXPECT_TRUE(world->IsValid(body));
}

TEST(PhysicsGeometry2DTests, RecordedGroupsOwnTheirInputsAndReadViewsObserveCompleteGroups)
{
    auto world=GeometryWorld(); ASSERT_TRUE(world); const auto body=world->CreateBody(); const auto old=world->AddBox(body,{4,4});
    std::vector<PhysicsShapeDescription2D> parts; std::string error; ASSERT_TRUE(BuildPolygon2D(Elbow,{},parts,error));
    const auto expected=parts.size(); PhysicsCommandBuffer commands(world); const auto ticket=commands.ReplaceShapes2D(body,{old},parts); parts.clear();
    auto held=world->Read(); std::promise<void> started; auto future=std::async(std::launch::async,[&]{started.set_value(); return commands.Commit();}); started.get_future().wait();
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(25)),std::future_status::timeout); EXPECT_EQ(held->OverlapCircle({0,0},10).size(),1u); held.reset();
    ASSERT_EQ(future.get().applied,1u); const auto result=commands.GetResult(ticket); ASSERT_EQ(result.shapes2D.size(),expected); EXPECT_EQ(result.shape2D,result.shapes2D.front());
    EXPECT_FALSE(world->IsValid(old)); EXPECT_EQ(world->Read()->OverlapCircle({0,0},10).size(),expected);
}

TEST(PhysicsGeometry2DTests, OneSidedChainHasGhostNeighborsRealCollisionAndPerSegmentIdentity)
{
    auto world=GeometryWorld(); ASSERT_TRUE(world); std::vector<PhysicsShapeDescription2D> parts; std::string error;
    ShapeSettings2D settings; settings.owner=81; const std::vector<Vector2> points{{5,0},{0,0},{-5,0}};
    ASSERT_TRUE(BuildChain2D(points,{},settings,parts,error))<<error; ASSERT_EQ(parts.size(),2u); EXPECT_EQ(parts[0].geometry.vertices[3],points[2]); EXPECT_EQ(parts[1].geometry.vertices[0],points[0]);
    const auto body=world->CreateBody(); const auto handles=Publish(world,body,parts); QueryHit2D hit;
    ASSERT_TRUE(world->Read()->CircleCast({2,3},0.2f,{0,-1},6,hit)); EXPECT_EQ(hit.subShapeId,1u); EXPECT_EQ(hit.owner,81u);
    EXPECT_FALSE(world->Read()->CircleCast({2,-3},0.2f,{0,1},6,hit));
    BodySettings2D ball; ball.type=BodyType2D::Dynamic; ball.position={-2,3}; const auto dynamic=world->CreateBody(ball); ASSERT_TRUE(world->AddCircle(dynamic,0.25f).IsValid());
    for(int i=0;i<180;++i) world->Step(1.f/60); BodyState2D state; ASSERT_TRUE(world->GetState(dynamic,state)); EXPECT_NEAR(state.position.y,0.25f,0.025f);
    ASSERT_TRUE(world->Remove(handles[0])); EXPECT_TRUE(world->IsValid(handles[1])); EXPECT_FALSE(world->IsValid(handles[0]));
    ASSERT_TRUE(world->Read()->Raycast({-4,2},{0,-1},4,hit)); EXPECT_EQ(hit.subShapeId,2u);
}

TEST(PhysicsGeometry2DTests, ChainSeamSlidesWithoutGhostBumpsAndLoopsHaveCompleteClosure)
{
    auto world=GeometryWorld(); ASSERT_TRUE(world); std::vector<PhysicsShapeDescription2D> parts; std::string error; ShapeSettings2D shape; shape.friction=0;
    ASSERT_TRUE(BuildChain2D(std::vector<Vector2>{{5,0},{0,0},{-5,0}},{},shape,parts,error)); Publish(world,world->CreateBody(),parts);
    BodySettings2D settings; settings.type=BodyType2D::Dynamic; settings.position={3,0.3f}; const auto ball=world->CreateBody(settings); ASSERT_TRUE(world->AddCircle(ball,0.25f,shape).IsValid());
    for(int i=0;i<60;++i) world->Step(1.f/60); ASSERT_TRUE(world->SetVelocity(ball,{-3,0}));
    for(int i=0;i<120;++i) { world->Step(1.f/60); BodyState2D state; ASSERT_TRUE(world->GetState(ball,state)); EXPECT_LT(state.position.y,0.29f); EXPECT_NEAR(state.velocity.x,-3,0.02f); }
    ASSERT_TRUE(BuildChain2D(std::vector<Vector2>{{10,0},{12,0},{12,2},{10,2}},{true},{},parts,error)); ASSERT_EQ(parts.size(),4u);
    EXPECT_EQ(parts.back().geometry.vertices[2],Vector2(10,0)); EXPECT_EQ(parts.back().geometry.vertices[3],Vector2(12,0));
    Publish(world,world->CreateBody(),parts); QueryHit2D hit; ASSERT_TRUE(world->Read()->CircleCast({9,1},0.1f,{1,0},5,hit)); EXPECT_EQ(hit.subShapeId,4u);
    const auto previous=parts; EXPECT_FALSE(BuildChain2D(std::vector<Vector2>{{0,0},{1,1},{0,1},{1,0}},{},{},parts,error)); EXPECT_EQ(parts.size(),previous.size());
    ShapeSettings2D sensor; sensor.trigger=true; EXPECT_FALSE(BuildChain2D(std::vector<Vector2>{{1,0},{0,0}},{},sensor,parts,error));
    EXPECT_FALSE(world->AddChainSegment(ball,previous[0].geometry.vertices).IsValid());
}

TEST(PhysicsGeometry2DTests, TwoSidedEdgesValidateNativeGeometryBeforeCreation)
{
    auto world=GeometryWorld(); ASSERT_TRUE(world); const auto body=world->CreateBody(); ShapeSettings2D shape; shape.offset={0,2}; shape.subShapeId=34;
    const auto segment=world->AddSegment(body,{-2,0},{2,0},shape); ASSERT_TRUE(segment.IsValid()); QueryHit2D hit;
    ASSERT_TRUE(world->Read()->Raycast({0,4},{0,-1},5,hit)); EXPECT_EQ(hit.subShapeId,34u); EXPECT_NEAR(hit.position.y,2,1e-5f);
    ASSERT_TRUE(world->Read()->Raycast({0,0},{0,1},5,hit)); EXPECT_EQ(hit.shape,segment);
    EXPECT_FALSE(world->AddSegment(body,{0,0},{0.001f,0}).IsValid()); EXPECT_FALSE(world->AddSegment(body,{0,0},{1e30f,0}).IsValid());
    EXPECT_FALSE(world->AddChainSegment(body,std::vector<Vector2>{{0,0},{0,0},{1,0},{2,0}}).IsValid());
    ShapeSettings2D invalid; invalid.offset={std::numeric_limits<float>::infinity(),0}; EXPECT_FALSE(world->AddSegment(body,{0,0},{1,0},invalid).IsValid());
    EXPECT_FALSE(world->AddConvexPolygon(body,std::vector<Vector2>{{0,0},{1e30f,0},{0,1e30f}}).IsValid()); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
    EXPECT_FALSE(world->AddCapsule(body,0.5f,1e-5f).IsValid()); EXPECT_FALSE(world->AddCapsule(body,0.5f,1e30f).IsValid());
    EXPECT_FALSE(world->AddCircle(body,1e30f).IsValid()); EXPECT_FALSE(world->AddBox(body,{1e30f,1e30f}).IsValid());
    ShapeSettings2D hugeDensity; hugeDensity.density=1e30f; EXPECT_FALSE(world->AddCircle(body,1,hugeDensity).IsValid());
    hugeDensity.density=1e-30f; EXPECT_FALSE(world->AddCircle(body,1,hugeDensity).IsValid()); EXPECT_EQ(world->GetStatistics().shapeCount,1u);
}
