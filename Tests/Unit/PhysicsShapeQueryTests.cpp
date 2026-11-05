#include <gtest/gtest.h>
#include <Physics/PhysicsWorld.h>
#include <array>
#include <cmath>
#include <limits>

namespace
{
using namespace NLS;
using namespace Physics;
std::shared_ptr<PhysicsWorld> ShapeQueryWorld()
{
    std::string error; auto world=PhysicsWorld::Create(error); EXPECT_TRUE(world) << error; return world;
}
BodyHandle QueryBox(PhysicsWorld& world, Maths::Vector3 position, const ShapeSettings& shape = {}, bool dynamic = false)
{
    BodySettings settings; settings.position=position; settings.dynamic=dynamic; settings.kinematic=false; settings.useGravity=false;
    const auto body=world.CreateBody(settings); EXPECT_TRUE(world.AttachBox(body,{0.5f,0.5f,0.5f},shape).IsValid()); return body;
}
}

TEST(PhysicsShapeQueryTests, RotatedBoxAndYUpCapsuleSweepsUseActualGeometry)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); ShapeSettings shape; shape.owner=71; shape.material=9;
    const auto target=QueryBox(*world,{5,0,0},shape); auto read=world->Read(); QueryHit hit; ShapeQueryStatus status;
    QueryGeometry3D geometry; geometry.type=QueryGeometryType3D::Box; geometry.halfExtents={3,0.25f,0.25f};
    ASSERT_TRUE(read->SweepShape(geometry,{},Maths::Quaternion::Identity,{3,0,0},10,hit,&status));
    EXPECT_EQ(status,ShapeQueryStatus::Success); EXPECT_EQ(hit.body,target); EXPECT_EQ(hit.owner,71u); EXPECT_EQ(hit.material,9u); EXPECT_NEAR(hit.distance,1.5f,1e-4f);
    const Maths::Quaternion quarter{0,0,std::sqrt(0.5f),std::sqrt(0.5f)};
    ASSERT_TRUE(read->SweepShape(geometry,{},quarter,{1,0,0},10,hit)); EXPECT_NEAR(hit.distance,4.25f,1e-4f);
    geometry.type=QueryGeometryType3D::Capsule; geometry.radius=0.25f; geometry.halfHeight=1;
    ASSERT_TRUE(read->SweepShape(geometry,{},Maths::Quaternion::Identity,{1,0,0},10,hit)); EXPECT_NEAR(hit.distance,4.25f,1e-4f);
    ASSERT_TRUE(read->SweepShape(geometry,{},quarter,{1,0,0},10,hit)); EXPECT_NEAR(hit.distance,3.25f,1e-4f);
    EXPECT_TRUE(hit.hasPointAndNormal); EXPECT_FALSE(hit.initialOverlap); EXPECT_NEAR(hit.position.x,4.5f,1e-4f); EXPECT_NEAR(hit.normal.x,-1,1e-4f);
}

TEST(PhysicsShapeQueryTests, OverlapsReturnIndependentShapeIdentityAndPreserveLegacyBodyDeduplication)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); const auto body=world->CreateBody();
    ShapeSettings first; first.localPosition={-1,0,0}; first.owner=10; first.material=2; const auto a=world->AttachBox(body,{0.25f,0.25f,0.25f},first);
    first.localPosition={1,0,0}; first.owner=20; first.material=3; const auto b=world->AttachBox(body,{0.25f,0.25f,0.25f},first);
    auto read=world->Read(); QueryGeometry3D geometry; geometry.type=QueryGeometryType3D::Box; geometry.halfExtents={2,1,1};
    const auto hits=read->OverlapShape(geometry,{},Maths::Quaternion::Identity); ASSERT_EQ(hits.size(),2u);
    EXPECT_EQ(hits[0].shape,a); EXPECT_EQ(hits[1].shape,b); EXPECT_EQ(hits[0].body,hits[1].body);
    EXPECT_EQ(hits[0].owner,10u); EXPECT_EQ(hits[1].owner,20u); EXPECT_EQ(hits[0].material,2u); EXPECT_EQ(hits[1].material,3u);
    EXPECT_FALSE(hits[0].hasPointAndNormal); EXPECT_EQ(hits[0].position,Maths::Vector3{}); EXPECT_EQ(read->OverlapSphere(3,{}).size(),1u);
}

TEST(PhysicsShapeQueryTests, AllSweepsAndBoundedOutputsProcessEveryNativeTouchBatch)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world);
    for (int i=1;i<=180;++i) QueryBox(*world,{float(i)*2,0,0});
    const auto revision=world->GetRevision(); auto read=world->Read(); QueryGeometry3D geometry; geometry.radius=0.1f;
    const auto hits=read->SweepShapeAll(geometry,{},Maths::Quaternion::Identity,{1,0,0},400); ASSERT_EQ(hits.size(),180u);
    for (size_t i=1;i<hits.size();++i) EXPECT_LT(hits[i-1].distance,hits[i].distance);
    std::array<QueryHit,5> output; const auto bounded=read->SweepShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{1,0,0},400,output);
    EXPECT_EQ(bounded.status,ShapeQueryStatus::Success); EXPECT_EQ(bounded.buffer.count,5u); EXPECT_EQ(bounded.buffer.total,180u); EXPECT_TRUE(bounded.buffer.truncated);
    for (size_t i=0;i<output.size();++i) { EXPECT_EQ(output[i].shape,hits[i].shape); EXPECT_NEAR(output[i].distance,hits[i].distance,1e-5f); }
    const auto count=read->SweepShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{1,0,0},400,{});
    EXPECT_EQ(count.buffer.count,0u); EXPECT_EQ(count.buffer.total,180u); EXPECT_TRUE(count.buffer.truncated);
    const auto empty=read->SweepShapeNonAlloc(geometry,{400,0,0},Maths::Quaternion::Identity,{1,0,0},10,output);
    EXPECT_EQ(empty.buffer.total,0u); EXPECT_FALSE(empty.buffer.truncated); EXPECT_EQ(world->GetRevision(),revision);
}

TEST(PhysicsShapeQueryTests, OverlapCountsRemainExactBeyondInternalBufferCapacity)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); const auto body=world->CreateBody();
    for (uint64_t i=1;i<=150;++i) { ShapeSettings shape; shape.owner=i; ASSERT_TRUE(world->AttachSphere(body,0.1f,shape).IsValid()); }
    auto read=world->Read(); QueryGeometry3D geometry; std::array<QueryHit,2> output;
    const auto hits=read->OverlapShape(geometry,{},Maths::Quaternion::Identity); EXPECT_EQ(hits.size(),150u);
    const auto bounded=read->OverlapShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,output);
    EXPECT_EQ(bounded.buffer.count,2u); EXPECT_EQ(bounded.buffer.total,150u); EXPECT_TRUE(bounded.buffer.truncated);
    EXPECT_EQ(output[0].owner,1u); EXPECT_EQ(output[1].owner,2u);
    const auto count=read->OverlapShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{}); EXPECT_EQ(count.buffer.total,150u); EXPECT_TRUE(count.buffer.truncated);
}

TEST(PhysicsShapeQueryTests, CookedConvexTemplatesExcludeTheirWholeBodyAndSurviveRetainedReads)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); const auto source=world->CreateBody();
    const std::array<Maths::Vector3,4> vertices{{{-1,0,-1},{1,0,-1},{0,0,1},{0,2,0}}};
    const auto convex=world->AttachConvexMesh(source,vertices); ASSERT_TRUE(convex.IsValid()); ShapeSettings child; child.localPosition={1,0,0};
    ASSERT_TRUE(world->AttachSphere(source,0.25f,child).IsValid()); const auto target=QueryBox(*world,{5,2,0});
    auto read=world->Read(); QueryGeometry3D geometry; geometry.type=QueryGeometryType3D::Shape; geometry.shape=convex;
    Maths::Vector3 position; Maths::Quaternion rotation; ASSERT_TRUE(read->GetShapeWorldPose(convex,position,rotation)); world.reset();
    QueryHit hit; ASSERT_TRUE(read->SweepShape(geometry,position,rotation,{1,0,0},10,hit)); EXPECT_EQ(hit.body,target);
    // The tetrahedron touches at 4.25; its bounding box would touch at 3.5.
    // Convex queries retain the SDK's support-margin/GJK tolerance.
    EXPECT_NEAR(hit.distance,4.25f,0.005f);
    EXPECT_TRUE(read->OverlapShape(geometry,position,rotation).empty());
}

TEST(PhysicsShapeQueryTests, InvalidForeignAndUnsupportedSourcesNeverReachBackendAssertions)
{
    auto world=ShapeQueryWorld(), foreign=ShapeQueryWorld(); ASSERT_TRUE(world); ASSERT_TRUE(foreign);
    const auto body=world->CreateBody(); const auto stale=world->AttachBox(body,{1,1,1}); ASSERT_TRUE(world->Remove(stale)); ASSERT_TRUE(world->AttachBox(body,{1,1,1}).IsValid());
    const auto foreignShape=foreign->AttachSphere(foreign->CreateBody(),1); const auto cylinder=world->AttachCylinder(body,0.5f,1);
    const std::array<Maths::Vector3,3> vertices{{{-1,0,-1},{0,0,1},{1,0,-1}}}; const std::array<uint32_t,3> indices{0,1,2};
    const auto mesh=world->AttachTriangleMesh(body,vertices,indices); ASSERT_TRUE(mesh.IsValid()); ASSERT_TRUE(cylinder.IsValid());
    const auto revision=world->GetRevision(); auto read=world->Read(); QueryGeometry3D geometry; geometry.type=QueryGeometryType3D::Shape;
    for (const auto shape:std::array{stale,foreignShape,ShapeHandle{}})
    {
        geometry.shape=shape; const auto result=read->OverlapShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{}); EXPECT_EQ(result.status,ShapeQueryStatus::InvalidShape);
    }
    for (const auto shape:std::array{mesh})
    {
        geometry.shape=shape; const auto result=read->SweepShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{1,0,0},10,{});
        EXPECT_EQ(result.status,ShapeQueryStatus::UnsupportedGeometry);
    }
    QueryHit unchanged; unchanged.owner=987; ShapeQueryStatus status; geometry.type=QueryGeometryType3D::Sphere; geometry.radius=0;
    EXPECT_FALSE(read->SweepShape(geometry,{},Maths::Quaternion::Identity,{1,0,0},10,unchanged,&status)); EXPECT_EQ(status,ShapeQueryStatus::InvalidInput); EXPECT_EQ(unchanged.owner,987u);
    geometry.radius=0.5f; EXPECT_EQ(read->OverlapShapeNonAlloc(geometry,{},Maths::Quaternion{0,0,0,0},{}).status,ShapeQueryStatus::InvalidInput);
    EXPECT_EQ(read->SweepShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{},10,{}).status,ShapeQueryStatus::InvalidInput);
    EXPECT_EQ(read->SweepShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{1,0,0},1e10f,{}).status,ShapeQueryStatus::InvalidInput);
    geometry.radius=std::numeric_limits<float>::max(); EXPECT_EQ(read->OverlapShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{}).status,ShapeQueryStatus::InvalidInput);
    geometry.radius=0.5f; EXPECT_EQ(read->OverlapShapeNonAlloc(geometry,{std::numeric_limits<float>::max(),0,0},Maths::Quaternion::Identity,{}).status,ShapeQueryStatus::InvalidInput);
    geometry.type=static_cast<QueryGeometryType3D>(255); EXPECT_EQ(read->OverlapShapeNonAlloc(geometry,{},Maths::Quaternion::Identity,{}).status,ShapeQueryStatus::InvalidInput);
    EXPECT_EQ(world->GetRevision(),revision);
}

TEST(PhysicsShapeQueryTests, LayerTriggerDynamicAndIgnoredBodyFiltersMatchExistingQuerySemantics)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); ShapeSettings shape; shape.layer=2; shape.owner=11; shape.material=7;
    const auto near=QueryBox(*world,{2,0,0},shape); shape.layer=4; shape.trigger=true; shape.owner=22; const auto sensor=QueryBox(*world,{4,0,0},shape);
    shape.layer=2; shape.trigger=false; shape.owner=33; const auto dynamic=QueryBox(*world,{6,0,0},shape,true);
    QueryGeometry3D geometry; geometry.radius=0.1f; QueryFilter filter; filter.layers=2;
    auto hits=world->Read(filter)->SweepShapeAll(geometry,{},Maths::Quaternion::Identity,{1,0,0},10); ASSERT_EQ(hits.size(),2u); EXPECT_EQ(hits[0].body,near); EXPECT_EQ(hits[1].body,dynamic);
    filter.ignored={near}; filter.includeDynamic=false; EXPECT_TRUE(world->Read(filter)->SweepShapeAll(geometry,{},Maths::Quaternion::Identity,{1,0,0},10).empty());
    filter.layers=4; filter.includeTriggers=true; hits=world->Read(filter)->SweepShapeAll(geometry,{},Maths::Quaternion::Identity,{1,0,0},10);
    ASSERT_EQ(hits.size(),1u); EXPECT_EQ(hits[0].body,sensor); EXPECT_EQ(hits[0].owner,22u); EXPECT_EQ(hits[0].material,7u);
    filter.layers=2; filter.includeStatic=false; filter.includeDynamic=true; hits=world->Read(filter)->SweepShapeAll(geometry,{},Maths::Quaternion::Identity,{1,0,0},10);
    ASSERT_EQ(hits.size(),1u); EXPECT_EQ(hits[0].body,dynamic);
}

TEST(PhysicsShapeQueryTests, InitialOverlapDoesNotInventAContactPointIncludingTheLegacySphereEntry)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); const auto target=QueryBox(*world,{}); auto read=world->Read(); QueryGeometry3D geometry; geometry.radius=0.25f;
    QueryHit hit; ASSERT_TRUE(read->SweepShape(geometry,{},Maths::Quaternion::Identity,{1,0,0},10,hit));
    EXPECT_EQ(hit.body,target); EXPECT_TRUE(hit.initialOverlap); EXPECT_FALSE(hit.hasPointAndNormal); EXPECT_FLOAT_EQ(hit.distance,0); EXPECT_EQ(hit.position,Maths::Vector3{});
    ASSERT_TRUE(read->SweepSphere(0.25f,{},{1,0,0},10,hit)); EXPECT_TRUE(hit.initialOverlap); EXPECT_FALSE(hit.hasPointAndNormal);
    const auto hits=read->OverlapShape(geometry,{},Maths::Quaternion::Identity); ASSERT_EQ(hits.size(),1u); EXPECT_FALSE(hits[0].hasPointAndNormal);
}

TEST(PhysicsShapeQueryTests, ValidMissAndDisabledCategoriesHaveSuccessfulStatusAndNoOutputWrites)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); QueryBox(*world,{5,0,0}); QueryGeometry3D geometry; ShapeQueryStatus status=ShapeQueryStatus::InvalidInput;
    QueryHit hit; hit.owner=77;
    EXPECT_FALSE(world->Read()->SweepShape(geometry,{},Maths::Quaternion::Identity,{-1,0,0},10,hit,&status)); EXPECT_EQ(status,ShapeQueryStatus::Success); EXPECT_EQ(hit.owner,77u);
    QueryFilter filter; filter.includeStatic=false; filter.includeDynamic=false;
    EXPECT_EQ(world->Read(filter)->OverlapShapeNonAlloc(geometry,{5,0,0},Maths::Quaternion::Identity,{}).status,ShapeQueryStatus::Success);
    Maths::Vector3 position{8,9,10}; Maths::Quaternion rotation=Maths::Quaternion::Identity;
    EXPECT_FALSE(world->Read()->GetShapeWorldPose({},position,rotation)); EXPECT_EQ(position,Maths::Vector3(8,9,10));
}

TEST(PhysicsShapeQueryTests, CylinderCastsUseExactFlatCapsForPrimitivesAndRetainedCustomShapes)
{
    auto world=ShapeQueryWorld(); ASSERT_TRUE(world); const auto source=world->CreateBody();
    const auto cylinder=world->AttachCylinder(source,0.25f,1); ASSERT_TRUE(cylinder.IsValid()); const auto target=QueryBox(*world,{5,0,0});
    QueryFilter filter; filter.ignored={source}; auto read=world->Read(filter);
    QueryGeometry3D geometry; geometry.type=QueryGeometryType3D::Cylinder; geometry.radius=0.25f; geometry.halfHeight=1;
    QueryHit hit; ASSERT_TRUE(read->SweepShape(geometry,{},Maths::Quaternion::Identity,{1,0,0},10,hit));
    EXPECT_EQ(hit.body,target); EXPECT_NEAR(hit.distance,4.25f,1e-3f);
    const Maths::Quaternion quarter{0,0,std::sqrt(0.5f),std::sqrt(0.5f)};
    ASSERT_TRUE(read->SweepShape(geometry,{},quarter,{1,0,0},10,hit)); EXPECT_NEAR(hit.distance,3.5f,1e-3f);
    geometry.type=QueryGeometryType3D::Shape; geometry.shape=cylinder; read.reset(); read=world->Read(); world.reset();
    ASSERT_TRUE(read->SweepShape(geometry,{},quarter,{1,0,0},10,hit)); EXPECT_EQ(hit.body,target); EXPECT_NEAR(hit.distance,3.5f,1e-3f);
}
