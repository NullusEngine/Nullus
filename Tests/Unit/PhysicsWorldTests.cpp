#include <gtest/gtest.h>

#include <array>
#include <future>
#include <limits>

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
