#include "TestComponents.h"
#include <limits>

namespace NLS::ECS
{
class WorldTestAccess
{
public:
    static Entity ExhaustGeneration(World& world, Entity entity)
    {
        auto& slot = world.m_slots[entity.index];
        slot.generation = std::numeric_limits<uint32_t>::max();
        entity.generation = slot.generation;
        slot.location.chunk->Entities()[slot.location.row] = entity;
        return entity;
    }
    static void ExhaustChanges(World& world) { world.m_version = std::numeric_limits<uint64_t>::max(); }
};
}
using namespace NLS::ECS;
using namespace ECSTest;

namespace
{
struct MoveOnly
{
    std::unique_ptr<int> value;
    MoveOnly() = default;
    explicit MoveOnly(int number) : value(std::make_unique<int>(number)) {}
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
};
struct Fallible
{
    inline static int remaining = 0, alive = 0;
    Fallible() { if (--remaining == 0) throw std::runtime_error("batch constructor failed"); ++alive; }
    Fallible(Fallible&&) noexcept { ++alive; }
    ~Fallible() { --alive; }
};
}

TEST(ECSCore, EntityReuseRejectsStaleAndForeignHandles)
{
    World first, second;
    const auto entity = first.CreateEntity();
    EXPECT_TRUE(first.IsAlive(entity)); EXPECT_FALSE(second.IsAlive(entity));
    EXPECT_TRUE(first.DestroyEntity(entity)); EXPECT_FALSE(first.DestroyEntity(entity));
    const auto replacement = first.CreateEntity();
    EXPECT_EQ(replacement.index, entity.index); EXPECT_NE(replacement.generation, entity.generation);
    EXPECT_FALSE(first.IsAlive(entity)); EXPECT_FALSE(first.IsAlive({}));
    EXPECT_EQ(first.GetEntityCount(), 1u);
}
TEST(ECSCore, ExhaustedEntityGenerationRetiresSlot)
{
    World world;
    const auto entity = WorldTestAccess::ExhaustGeneration(world, world.CreateEntity());
    ASSERT_TRUE(world.DestroyEntity(entity));
    EXPECT_NE(world.CreateEntity().index, entity.index); EXPECT_FALSE(world.IsAlive(entity));
}
TEST(ECSCore, CloseInvalidatesHandlesAndWorldIdsAreUnique)
{
    World world;
    const auto entity = world.CreateEntity(); const auto id = world.GetId();
    world.Close(); EXPECT_FALSE(world.IsAlive(entity)); EXPECT_TRUE(world.GetLifetime().expired());
    EXPECT_THROW(world.CreateEntity(), std::logic_error);
    World replacement; EXPECT_NE(replacement.GetId(), id); EXPECT_FALSE(replacement.IsAlive(entity));
}
TEST(ECSCore, StableRegistrationDetectsSchemaAndIdConflicts)
{
    ComponentRegistry registry;
    const auto& position = registry.Register<Position>("example.Position");
    EXPECT_EQ(position.id, StableComponentId("example.Position"));
    EXPECT_EQ(&position, &registry.Register<Position>("example.Position"));
    EXPECT_THROW(registry.Register<Position>("different.Position"), std::invalid_argument);
    EXPECT_THROW(registry.Register<Position>("example.Position", ComponentKind::Data, 2), std::invalid_argument);
    ComponentRegistration conflict{"example.Velocity"}; conflict.stableId = position.id;
    EXPECT_THROW(registry.Register<Velocity>(conflict), std::invalid_argument);
    conflict.name = "example.Position"; conflict.stableId = position.id + 1;
    EXPECT_THROW(registry.Register<Velocity>(conflict), std::invalid_argument);
    EXPECT_THROW(registry.Register<Velocity>("invalid.Tag", ComponentKind::Tag), std::invalid_argument);
    ComponentRegistry other; EXPECT_EQ(other.Register<Position>("example.Position").id, position.id);
}
TEST(ECSCore, ComponentOperationsAndCanonicalArchetypes)
{
    World world; Register(world);
    const auto a = world.CreateEntity();
    world.AddComponent<Position>(a, {1, 2, 3}); world.AddComponent<Velocity>(a, {4, 5, 6});
    const auto b = world.CreateEntity(Signature<Velocity, Position>(world));
    EXPECT_EQ(world.GetEntityArchetype(a), world.GetEntityArchetype(b));
    EXPECT_EQ(world.ReadComponent<Position>(a).x, 1);
    world.SetComponent<Position>(a, {7, 8, 9}); EXPECT_EQ(world.ReadComponent<Position>(a).z, 9);
    EXPECT_THROW(world.AddComponent<Position>(a), std::invalid_argument);
    EXPECT_TRUE(world.RemoveComponent<Velocity>(a)); EXPECT_FALSE(world.RemoveComponent<Velocity>(a));
    EXPECT_EQ(world.TryGetComponent<Velocity>(a), nullptr);
    EXPECT_THROW(world.ReadComponent<Velocity>(a), std::invalid_argument);
    EXPECT_EQ(world.ReadComponent<Position>(a).x, 7);
}
TEST(ECSCore, NontrivialMigrationSwapBackAndDestruction)
{
    ASSERT_EQ(Tracked::alive, 0);
    {
        World world; Register(world);
        std::vector<Entity> entities;
        for (int i = 0; i < 700; ++i)
        { auto e = world.CreateEntity(); world.AddComponent<Tracked>(e, Tracked(std::to_string(i))); entities.push_back(e); }
        EXPECT_EQ(Tracked::alive, 700);
        for (size_t i = 0; i < entities.size(); ++i) world.AddComponent<Position>(entities[i], {float(i), 0, 0});
        for (size_t i = 0; i < entities.size(); i += 2) world.DestroyEntity(entities[i]);
        for (size_t i = 1; i < entities.size(); i += 2)
        {
            EXPECT_EQ(world.ReadComponent<Tracked>(entities[i]).text, std::to_string(i));
            EXPECT_EQ(world.ReadComponent<Tracked>(entities[i]).values[0], "buffer");
            EXPECT_EQ(world.ReadComponent<Position>(entities[i]).x, float(i));
            world.RemoveComponent<Position>(entities[i]);
        }
        EXPECT_EQ(Tracked::alive, 350);
    }
    EXPECT_EQ(Tracked::alive, 0);
}
TEST(ECSCore, ThrowingConstructorsAndCopiesLeaveSourceIntact)
{
    World world; Register(world);
    const auto entity = world.CreateEntity(Signature<Position>(world));
    world.SetComponent<Position>(entity, {42, 0, 0});
    Tracked value("keep");
    Tracked::failCopy = true;
    EXPECT_THROW(world.AddComponent(entity, world.GetComponentType<Tracked>().id, &value), std::runtime_error);
    Tracked::failCopy = false;
    EXPECT_FALSE(world.HasComponent<Tracked>(entity)); EXPECT_EQ(world.ReadComponent<Position>(entity).x, 42);
    Tracked::failConstruction = true;
    EXPECT_THROW(world.CreateEntity(Signature<Position, Tracked>(world)), std::runtime_error);
    Tracked::failConstruction = false;
    EXPECT_EQ(world.GetEntityCount(), 1u); EXPECT_EQ(Tracked::alive, 1);
}
TEST(ECSCore, CorrectAlignmentAndOversizedLayoutDiagnostic)
{
    World world; Register(world);
    auto entity = world.CreateEntity(Signature<Position, Aligned>(world));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&world.ReadComponent<Aligned>(entity)) % alignof(Aligned), 0u);
    world.SetComponent<Position>(entity, {13, 0, 0});
    EXPECT_THROW(world.AddComponent<Large>(entity), std::length_error);
    EXPECT_EQ(world.ReadComponent<Position>(entity).x, 13); EXPECT_FALSE(world.HasComponent<Large>(entity));
}
TEST(ECSCore, TagsDoNotAllocateColumnsAndBuffersOwnTheirMemory)
{
    World world; Register(world);
    const auto entity = world.CreateEntity();
    world.AddComponent<Tag>(entity);
    world.AddComponent<DynamicBuffer<std::string>>(entity, {{"first", "second"}});
    world.AddComponent<Position>(entity);
    world.WriteComponent<DynamicBuffer<std::string>>(entity).values.push_back("third");
    EXPECT_EQ(world.ReadComponent<DynamicBuffer<std::string>>(entity).values[2], "third");
    EXPECT_TRUE(world.HasComponent<Tag>(entity));
    EXPECT_THROW(world.ReadComponent<Tag>(entity), std::invalid_argument);
    EXPECT_TRUE(world.RemoveComponent<Tag>(entity));
}
TEST(ECSCore, SharedValuesPartitionChunksAndMigrateWithoutMutatingPeers)
{
    World world; Register(world);
    const auto entities = world.CreateEntities(40, Signature<Position, Group>(world));
    world.SetComponent<Group>(entities[0], {7});
    world.SetComponent<Position>(entities[0], {5, 0, 0});
    EXPECT_EQ(world.ReadComponent<Group>(entities[0]).value, 7);
    EXPECT_EQ(world.ReadComponent<Group>(entities[1]).value, 0);
    EXPECT_EQ(world.GetEntityArchetype(entities[0]), world.GetEntityArchetype(entities[1]));
    EXPECT_THROW(world.WriteComponent<Group>(entities[0]), std::invalid_argument);
    EXPECT_EQ(world.Query<Read<Group>>().Count(), 40u);
    world.SetComponent<Group>(entities[0], {0});
    EXPECT_EQ(world.ReadComponent<Position>(entities[0]).x, 5);
}
TEST(ECSCore, EmptyChunksAreReusedAndCanBeTrimmed)
{
    WorldConfig config; config.chunkBytes = 512; config.retainedEmptyChunksPerArchetype = 1;
    World world(config); Register(world);
    auto entities = world.CreateEntities(200, Signature<Position>(world));
    const auto allocated = world.GetStatistics().chunkAllocations;
    EXPECT_GT(allocated, 1u); EXPECT_EQ(world.DestroyEntities(entities), entities.size());
    entities = world.CreateEntities(200, Signature<Position>(world));
    EXPECT_EQ(world.GetStatistics().chunkAllocations, allocated);
    world.DestroyEntities(entities); world.TrimEmptyChunks();
    EXPECT_EQ(world.GetStatistics().chunks, 1u);
}
TEST(ECSCore, BatchLifecycleAndSceneOwnershipAreIndependent)
{
    World world; Register(world);
    auto entities = world.CreateEntities(12000, Signature<Position, Velocity>(world));
    for (size_t i = 0; i < entities.size(); ++i) world.SetScene(entities[i], i % 2 + 1);
    EXPECT_EQ(world.UnloadScene(1), 6000u); EXPECT_EQ(world.GetEntityCount(), 6000u);
    EXPECT_EQ(world.DestroyEntities(entities), 6000u); EXPECT_EQ(world.GetEntityCount(), 0u);
    EXPECT_THROW(world.UnloadScene(0), std::invalid_argument);
}
TEST(ECSCore, WorldResourcesAndForeignComponentAccess)
{
    World first, second; Register(first); Register(second);
    first.SetResource<Position>({3, 0, 0}); second.SetResource<Position>({8, 0, 0});
    first.WriteResource<Position>().x = 4;
    EXPECT_EQ(first.ReadResource<Position>().x, 4); EXPECT_EQ(second.ReadResource<Position>().x, 8);
    auto entity = first.CreateEntity(Signature<Position>(first));
    EXPECT_THROW(second.ReadComponent<Position>(entity), std::invalid_argument);
}
TEST(ECSCore, ChangeVersionExhaustionIsDiagnosedBeforeWriting)
{
    World world; Register(world);
    auto entity = world.CreateEntity(Signature<Position>(world));
    WorldTestAccess::ExhaustChanges(world);
    EXPECT_THROW(world.SetComponent<Position>(entity, {1, 0, 0}), std::overflow_error);
    EXPECT_EQ(world.ReadComponent<Position>(entity).x, 0);
    // A closed world may release values without generating new stamps.
}
TEST(ECSCore, MoveOnlyComponentsAndResourcesRetainOwnershipAcrossMigration)
{
    World world; Register(world); world.RegisterComponent<MoveOnly>("tests.MoveOnly");
    const auto entity = world.CreateEntity(); world.AddComponent<MoveOnly>(entity, MoveOnly(42));
    const auto* pointer = world.ReadComponent<MoveOnly>(entity).value.get();
    world.AddComponent<Tag>(entity); world.RemoveComponent<Tag>(entity);
    EXPECT_EQ(world.ReadComponent<MoveOnly>(entity).value.get(), pointer);
    EXPECT_EQ(*world.ReadComponent<MoveOnly>(entity).value, 42);
    world.SetResource<MoveOnly>(MoveOnly(9)); EXPECT_EQ(*world.ReadResource<MoveOnly>().value, 9);
    MoveOnly value(7);
    EXPECT_THROW(world.SetComponent(entity, world.GetComponentType<MoveOnly>().id, &value), std::invalid_argument);
    EXPECT_EQ(*world.ReadComponent<MoveOnly>(entity).value, 42);
}
TEST(ECSCore, BatchConstructorFailureRollsBackEveryCreatedEntity)
{
    World world; world.RegisterComponent<Fallible>("tests.Fallible");
    Fallible::remaining = 4;
    EXPECT_THROW(world.CreateEntities(20, Signature<Fallible>(world)), std::runtime_error);
    EXPECT_EQ(world.GetEntityCount(), 0u); EXPECT_EQ(Fallible::alive, 0);
    Fallible::remaining = 3;
    const auto entities = world.CreateEntities(2, Signature<Fallible>(world));
    EXPECT_EQ(Fallible::alive, 2); world.DestroyEntities(entities); EXPECT_EQ(Fallible::alive, 0);
}
