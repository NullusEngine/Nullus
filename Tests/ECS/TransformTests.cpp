#include <Engine/ECS/TransformHierarchy.h>
#include <ECS/Serialization/WorldArchive.h>
#include "JobTestSupport.h"
#include <Math/Transform.h>
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

namespace
{
using namespace NLS::ECS;
using namespace NLS::Engine::ECSIntegration;
Entity Make(World& world, float x, Entity parent = {})
{
    auto entity = world.CreateEntity(); LocalTransform local; local.position[0] = x; AddTransform(world, entity, local, parent); return entity;
}
void Update(World& world) { world.Systems().RunPhase(SystemPhase::LateUpdate, {}); }
TEST(ECSTransform, HierarchyMatchesExistingMathTransformWithRotationAndNonuniformScale)
{
    World world; auto& system = RegisterTransformSystem(world);
    auto root = world.CreateEntity(), child = world.CreateEntity();
    const auto half = std::sqrt(0.5f);
    AddTransform(world, root, {{10, 2, 0}, {0, 0, half, half}, {2, 3, 1}});
    AddTransform(world, child, {{1, 2, 0}}, root); Update(world);
    const auto& output = world.ReadComponent<WorldTransform>(child);
    EXPECT_NEAR(output.matrix[3], 4, 0.0001f); EXPECT_NEAR(output.matrix[7], 4, 0.0001f);
    NLS::Maths::Transform legacyRoot({10, 2, 0}, {0, 0, half, half}, {2, 3, 1});
    NLS::Maths::Transform legacyChild(NLS::Maths::Vector3{1, 2, 0}); legacyChild.SetParent(legacyRoot);
    const auto& legacyMatrix = legacyChild.GetWorldMatrix();
    for (size_t i = 0; i < 16; ++i) EXPECT_NEAR(output.matrix[i], legacyMatrix.data[i], 0.0001f);
    legacyChild.RemoveParent();
    EXPECT_EQ(system.GetStatistics().levels, 2u); EXPECT_EQ(system.GetStatistics().recomputed, 2u);
}
TEST(ECSTransform, QuietFramesDoNotWriteOutputsAndParentChangesPropagateOnlyToDescendants)
{
    World world; auto& system = RegisterTransformSystem(world);
    const auto root = Make(world, 10), child = Make(world, 2, root), unrelated = Make(world, 90);
    Update(world); const auto rootRevision = world.ReadComponent<WorldTransform>(root).revision;
    const auto otherRevision = world.ReadComponent<WorldTransform>(unrelated).revision;
    const auto cursor = world.GetChangeToken(); Update(world);
    EXPECT_EQ(system.GetStatistics().recomputed, 0u);
    EXPECT_EQ(world.Query<Read<WorldTransform>>().Changed<WorldTransform>(cursor).Count(), 0u);
    world.WriteComponent<LocalTransform>(root).position[0] = 20; Update(world);
    EXPECT_EQ(system.GetStatistics().recomputed, 2u); EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(child).matrix[3], 22);
    EXPECT_EQ(world.ReadComponent<WorldTransform>(root).revision, rootRevision + 1);
    EXPECT_EQ(world.ReadComponent<WorldTransform>(unrelated).revision, otherRevision);
}
TEST(ECSTransform, ReparentAndDestroyedParentDetachAtTheSafePoint)
{
    World world; auto& system = RegisterTransformSystem(world);
    const auto first = Make(world, 2), second = Make(world, 5), child = Make(world, 3, first);
    Update(world); EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(child).matrix[3], 5);
    SetTransformParent(world, child, second); Update(world); EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(child).matrix[3], 8);
    world.DestroyEntity(second); Update(world);
    EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(child).matrix[3], 3); EXPECT_FALSE(world.HasComponent<Parent>(child));
    EXPECT_EQ(system.GetStatistics().staleParents, 1u); Update(world); EXPECT_EQ(system.GetStatistics().recomputed, 0u);
}
TEST(ECSTransform, RecycledAndForeignParentsNeverAttachToAnUnrelatedEntity)
{
    World world; RegisterTransformSystem(world); World other; RegisterTransformComponents(other);
    const auto parent = Make(world, 99), child = Make(world, 3, parent); world.DestroyEntity(parent);
    const auto replacement = Make(world, 500); EXPECT_EQ(replacement.index, parent.index);
    Update(world); EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(child).matrix[3], 3);
    const auto foreign = Make(other, 100); world.AddComponent<Parent>(child, {foreign}); Update(world);
    EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(child).matrix[3], 3); EXPECT_FALSE(world.HasComponent<Parent>(child));
}
TEST(ECSTransform, CyclesAreRejectedBeforeOutputChangesAndCanBeRepaired)
{
    World world; RegisterTransformSystem(world); const auto root = Make(world, 1), child = Make(world, 2, root); Update(world);
    const auto before = world.ReadComponent<WorldTransform>(child);
    EXPECT_THROW(SetTransformParent(world, root, child), std::invalid_argument);
    world.AddComponent<Parent>(root, {child});
    EXPECT_THROW(Update(world), std::invalid_argument);
    EXPECT_EQ(world.ReadComponent<WorldTransform>(child), before);
    SetTransformParent(world, root, {}); Update(world); EXPECT_EQ(world.ReadComponent<WorldTransform>(child), before);
}
TEST(ECSTransform, DeepHierarchyUsesIterativeOrdering)
{
    World world; auto& system = RegisterTransformSystem(world); Entity parent;
    for (size_t i = 0; i < 4096; ++i)
    {
        const auto entity = world.CreateEntity(); world.AddComponent<LocalTransform>(entity, {{1, 0, 0}});
        if (parent.IsValid()) world.AddComponent<Parent>(entity, {parent}); parent = entity;
    }
    Update(world); EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(parent).matrix[3], 4096);
    EXPECT_EQ(system.GetStatistics().levels, 4096u);
}
TEST(ECSTransform, WideHierarchyUsesJoinedJobSystemBatchesInParentOrder)
{
    WorldConfig config; config.minimumParallelEntities = 64; config.chunksPerJob = 4;
    World world(config); auto& system = RegisterTransformSystem(world); const auto root = Make(world, 10);
    std::vector<Entity> children;
    for (size_t i = 0; i < 2048; ++i) children.push_back(Make(world, float(i), root));
    ECSTest::JobSystemScope jobs(4);
    Update(world); EXPECT_GT(system.GetStatistics().jobs, 0u);
    for (size_t i = 0; i < children.size(); ++i) EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(children[i]).matrix[3], float(i + 10));
    world.WriteComponent<LocalTransform>(root).position[0] = 20; Update(world);
    for (size_t i = 0; i < children.size(); ++i) EXPECT_FLOAT_EQ(world.ReadComponent<WorldTransform>(children[i]).matrix[3], float(i + 20));
    world.DestroyEntities(children); // All accepted jobs are joined before mutation.
    EXPECT_EQ(world.GetEntityCount(), 1u);
}
TEST(ECSTransform, SingleThreadDebugModeAvoidsWorkerDispatch)
{
    WorldConfig config; config.singleThreaded = true; config.minimumParallelEntities = 1;
    World world(config); auto& system = RegisterTransformSystem(world);
    for (size_t i = 0; i < 128; ++i) Make(world, float(i));
    ECSTest::JobSystemScope jobs(2);
    Update(world); EXPECT_EQ(system.GetStatistics().jobs, 0u); EXPECT_EQ(system.GetStatistics().recomputed, 128u);
}
TEST(ECSTransform, PrefabRoundTripRemapsParentsAndRebuildsDerivedOutputs)
{
    World source; RegisterTransformSystem(source); const auto parent = Make(source, 7), child = Make(source, 4, parent); Update(source);
    const std::array records{PersistentEntity{parent, "parent", {}}, PersistentEntity{child, "child", {}}};
    const auto prefab = WorldArchive::Parse(WorldArchive::Encode(WorldArchive::Capture(source, records, DocumentKind::Prefab)));
    World target; RegisterTransformSystem(target);
    const auto first = WorldArchive::InstantiatePrefab(target, prefab), second = WorldArchive::InstantiatePrefab(target, prefab);
    EXPECT_FALSE(target.HasComponent<WorldTransform>(first.Find("parent"))); source.Close(); Update(target);
    EXPECT_EQ(target.ReadComponent<Parent>(first.Find("child")).entity, first.Find("parent"));
    target.WriteComponent<LocalTransform>(first.Find("parent")).position[0] = 20; Update(target);
    EXPECT_FLOAT_EQ(target.ReadComponent<WorldTransform>(first.Find("child")).matrix[3], 24);
    EXPECT_FLOAT_EQ(target.ReadComponent<WorldTransform>(second.Find("child")).matrix[3], 11);
}
TEST(ECSTransform, InvalidInputsAndOverflowLeavePreviousWorldOutputsIntact)
{
    World world; RegisterTransformSystem(world); const auto root = Make(world, 1), child = Make(world, 2, root); Update(world);
    const auto before = world.ReadComponent<WorldTransform>(child); LocalTransform zero; zero.rotation = {};
    const auto empty = world.CreateEntity(); EXPECT_THROW(AddTransform(world, empty, zero), std::invalid_argument);
    EXPECT_FALSE(world.HasComponent<LocalTransform>(empty));
    world.WriteComponent<LocalTransform>(root).scale[0] = std::numeric_limits<float>::max();
    world.WriteComponent<LocalTransform>(child).scale[0] = std::numeric_limits<float>::max();
    EXPECT_THROW(Update(world), std::overflow_error); EXPECT_EQ(world.ReadComponent<WorldTransform>(child), before);
    world.WriteComponent<LocalTransform>(root).position[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(Update(world), std::invalid_argument); EXPECT_EQ(world.ReadComponent<WorldTransform>(child), before);
}
}
