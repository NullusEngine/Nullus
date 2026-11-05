#include <gtest/gtest.h>
#include <Physics/PhysicsCommandBuffer.h>
#include <Physics/CollisionMesh.h>
#include <array>
#include <limits>

namespace
{
using namespace NLS;
using namespace Physics;
CollisionMeshSource Tetrahedron()
{
    CollisionMeshSource source; source.vertices={{0,0,0},{2,0,0},{0,2,0},{0,0,2}}; return source;
}
std::shared_ptr<PhysicsWorld> CookingWorld()
{ std::string error; auto world=PhysicsWorld::Create(error); EXPECT_TRUE(world)<<error; return world; }
BodySettings Falling(Maths::Vector3 position)
{ BodySettings body; body.dynamic=true; body.kinematic=false; body.position=position; return body; }
}

TEST(PhysicsCookingTests, ConvexRoundTripUsesRealGeometryAndRetainsNativeLifetime)
{
    std::string error; auto cooked=CookedCollisionMesh::Cook(Tetrahedron(),error); ASSERT_TRUE(cooked)<<error;
    EXPECT_GT(cooked->GetCookedByteCount(),16u); auto restored=CookedCollisionMesh::Deserialize(cooked->Serialize(),error); ASSERT_TRUE(restored)<<error;
    cooked.reset(); auto world=CookingWorld(); const auto body=world->CreateBody();
    const auto shape=world->AttachCollisionMesh(body,restored); ASSERT_TRUE(shape.IsValid());
    auto read=world->Read(); QueryHit hit; ASSERT_TRUE(read->Raycast({-2,0.25f,0.25f},{1,0,0},10,hit)); EXPECT_NEAR(hit.distance,2,1e-4f);
    EXPECT_FALSE(read->Raycast({-2,1.5f,1.5f},{1,0,0},10,hit)); // Inside its AABB, outside its tetrahedral faces.
    restored.reset(); world.reset(); ASSERT_TRUE(read->Raycast({-2,0.25f,0.25f},{1,0,0},10,hit)); EXPECT_EQ(hit.shape,shape);
}

TEST(PhysicsCookingTests, MirroredTriangleCookingPreservesFrontFacesAndSourceIndices)
{
    CollisionMeshSource source; source.kind=CollisionMeshKind::Triangle; source.vertices={{-1,0,-1},{1,0,-1},{0,0,1}};
    source.indices={0,2,1}; source.bakeScale={-2,1,1}; source.weldVertices=true;
    std::string error; auto cooked=CookedCollisionMesh::Cook(source,error); ASSERT_TRUE(cooked)<<error;
    auto world=CookingWorld(); auto shape=world->AttachCollisionMesh(world->CreateBody(),cooked); ASSERT_TRUE(shape.IsValid());
    QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({0,3,0},{0,-1,0},10,hit)); EXPECT_NEAR(hit.position.y,0,1e-5f);
    EXPECT_GT(hit.normal.y,0.9f); EXPECT_EQ(hit.faceIndex,0u); EXPECT_EQ(cooked->GetBakedScale(),Maths::Vector3(-2,1,1));
    auto normal=source; normal.bakeScale={2,1,1}; auto other=CookedCollisionMesh::Cook(normal,error); ASSERT_TRUE(other);
    EXPECT_NE(cooked->Serialize(),other->Serialize());
}

TEST(PhysicsCookingTests, CookedHeightfieldProducesRealStaticCollisionAtBakedHeight)
{
    CollisionMeshSource source; source.kind=CollisionMeshKind::HeightField; source.rows=3; source.columns=3;
    source.heights.assign(9,2); source.rowSpacing=0.5f; source.columnSpacing=0.25f; source.bakeScale={2,3,4};
    std::string error; const auto data=CookedCollisionMesh::Cook(source,error); ASSERT_TRUE(data)<<error;
    const auto loaded=CookedCollisionMesh::Deserialize(data->Serialize(),error); ASSERT_TRUE(loaded)<<error;
    auto world=CookingWorld(); ASSERT_TRUE(world->AttachCollisionMesh(world->CreateBody(),loaded).IsValid());
    ShapeSettings outOfBounds; outOfBounds.localPosition={0,6e6f,0};
    EXPECT_FALSE(world->AttachCollisionMesh(world->CreateBody(),loaded,outOfBounds,Maths::Vector3{1e6f,1e6f,1e6f}).IsValid());
    QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({0.75f,20,0.75f},{0,-1,0},30,hit)); EXPECT_NEAR(hit.position.y,6,1e-3f);
    const auto ball=world->AddSphere(0.25f,Falling({0.75f,12,0.75f})); ASSERT_TRUE(ball.IsValid());
    for(int i=0;i<360;++i) world->Step(1.f/60);
    BodyState state; ASSERT_TRUE(world->GetState(ball,state)); EXPECT_NEAR(state.position.y,6.25f,0.04f);
}

TEST(PhysicsCookingTests, CookedConcaveReplacementRejectsDynamicAndTriggerWithoutRemovingValidShape)
{
    CollisionMeshSource source; source.kind=CollisionMeshKind::Triangle; source.vertices={{-1,0,-1},{1,0,-1},{0,0,1}}; source.indices={0,2,1};
    std::string error; auto cooked=CookedCollisionMesh::Cook(source,error); ASSERT_TRUE(cooked)<<error;
    auto world=CookingWorld(); auto dynamic=Falling({0,2,0}); const auto body=world->CreateBody(dynamic);
    const auto original=world->AttachBox(body,{0.5f,0.5f,0.5f}); ASSERT_TRUE(original.IsValid());
    PhysicsCommandBuffer commands(world); CollisionGeometry3D geometry; geometry.type=CollisionGeometryType3D::CookedMesh; geometry.cookedMesh=cooked;
    const auto ticket=commands.ReplaceShape(body,original,geometry); const auto report=commands.Commit();
    EXPECT_EQ(report.failed,1u); EXPECT_EQ(commands.GetResult(ticket).status,PhysicsCommandStatus::Failed); EXPECT_TRUE(world->IsValid(original));
    ShapeSettings trigger; trigger.trigger=true; EXPECT_FALSE(world->AttachCollisionMesh(world->CreateBody(),cooked,trigger).IsValid());
    EXPECT_EQ(world->GetStatistics().shapeCount,1u);
}

TEST(PhysicsCookingTests, MalformedSourcesAndCorruptStreamsAreRejectedWithoutBackendAssertions)
{
    std::string error; auto source=Tetrahedron(); source.vertices[0].x=std::numeric_limits<float>::infinity(); EXPECT_FALSE(CookedCollisionMesh::Cook(source,error));
    source=Tetrahedron(); source.vertices[3]={1,1,0}; EXPECT_FALSE(CookedCollisionMesh::Cook(source,error));
    source=Tetrahedron(); source.convexVertexLimit=4; EXPECT_FALSE(CookedCollisionMesh::Cook(source,error));
    source=Tetrahedron(); source.bakeScale={0,1,1}; EXPECT_FALSE(CookedCollisionMesh::Cook(source,error));
    source.kind=CollisionMeshKind::Triangle; source.indices={0,1,99}; EXPECT_FALSE(CookedCollisionMesh::Cook(source,error));
    source.kind=CollisionMeshKind::HeightField; source.vertices.clear(); source.indices.clear(); source.rows=2; source.columns=2; source.heights={0};
    EXPECT_FALSE(CookedCollisionMesh::Cook(source,error));
    source.heights.assign(4,0); source.bakeScale={1,1,1}; source.rowSpacing=source.columnSpacing=1e-4f;
    auto tinyGrid=CookedCollisionMesh::Cook(source,error); ASSERT_TRUE(tinyGrid)<<error;
    auto world=CookingWorld(); EXPECT_FALSE(world->AttachCollisionMesh(world->CreateBody(),tinyGrid,{},Maths::Vector3{1e-6f,1,1e-6f}).IsValid());
    auto good=CookedCollisionMesh::Cook(Tetrahedron(),error); ASSERT_TRUE(good)<<error; auto bytes=good->Serialize();
    EXPECT_FALSE(CookedCollisionMesh::Deserialize({},error));
    for(const auto length:std::array<size_t,4>{1,16,bytes.size()/2,bytes.size()-1}) EXPECT_FALSE(CookedCollisionMesh::Deserialize(std::span(bytes).first(length),error));
    bytes[bytes.size()/2]^=1; EXPECT_FALSE(CookedCollisionMesh::Deserialize(bytes,error)); EXPECT_NE(error.find("checksum"),std::string::npos);
}

TEST(PhysicsCookingTests, OwnedCookedInputSupportsDeferredAssemblyAndIndependentWorlds)
{
    std::string error; auto source=Tetrahedron(); auto cooked=CookedCollisionMesh::Cook(source,error); ASSERT_TRUE(cooked)<<error;
    std::vector<std::shared_ptr<PhysicsWorld>> worlds;
    for(int i=0;i<6;++i)
    {
        auto world=CookingWorld(); PhysicsCommandBuffer commands(world); const auto body=commands.CreateBody();
        CollisionGeometry3D geometry; geometry.type=CollisionGeometryType3D::CookedMesh; geometry.cookedMesh=cooked; geometry.meshScale={2,1,1};
        const auto shape=commands.AttachShape(body,std::move(geometry)); EXPECT_EQ(commands.Commit().applied,2u);
        const auto result=commands.GetResult(shape); ASSERT_TRUE(result.shape.IsValid()); worlds.push_back(world);
    }
    source.vertices.clear(); cooked.reset();
    for(auto& world:worlds)
    { QueryHit hit; ASSERT_TRUE(world->Read()->Raycast({3,0.1f,-1},{0,0,1},5,hit)); EXPECT_NEAR(hit.distance,1,1e-4f); }
}

TEST(PhysicsCookingTests, MaterialCombineModesControlActualRebound)
{
    const auto rebound=[](MaterialCombineMode mode)
    {
        auto world=CookingWorld(); ShapeSettings floor; floor.restitution=0.2f; floor.restitutionCombine=mode;
        BodySettings floorBody; floorBody.position={0,-0.5f,0}; world->AttachBox(world->CreateBody(floorBody),{10,0.5f,10},floor);
        ShapeSettings ball; ball.restitution=0.8f; ball.restitutionCombine=mode;
        const auto handle=world->CreateBody(Falling({0,4,0})); world->AttachSphere(handle,0.25f,ball);
        for(int i=0;i<180;++i) { world->Step(1.f/120); const auto speed=world->GetVelocity(handle).y; if(speed>0.1f) return speed; }
        ADD_FAILURE()<<"Ball never rebounded"; return 0.f;
    };
    const auto maximum=rebound(MaterialCombineMode::Maximum),average=rebound(MaterialCombineMode::Average),minimum=rebound(MaterialCombineMode::Minimum);
    EXPECT_GT(maximum,average*1.4f); EXPECT_GT(average,minimum*1.8f);
    auto world=CookingWorld(); ShapeSettings bad; bad.frictionCombine=static_cast<MaterialCombineMode>(255);
    EXPECT_FALSE(world->AttachSphere(world->CreateBody(),1,bad).IsValid());
}
