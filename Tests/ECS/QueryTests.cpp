#include "TestComponents.h"
using namespace NLS::ECS;
using namespace ECSTest;

TEST(ECSQuery, TypedReadWriteIteration)
{
    World world; Register(world);
    const auto entities = world.CreateEntities(300, Signature<Position, Velocity>(world));
    for (auto entity : entities) world.SetComponent<Velocity>(entity, {2, 3, 4});
    world.Query<Write<Position>, Read<Velocity>>().Each([](Entity, Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; });
    for (auto entity : entities) { EXPECT_EQ(world.ReadComponent<Position>(entity).x, 2); EXPECT_EQ(world.ReadComponent<Position>(entity).y, 3); }
}
TEST(ECSQuery, AllAnyNoneOptionalAndDynamicArchetypeCache)
{
    World world; Register(world);
    auto query = world.Query<Read<Position>, OptionalRead<Velocity>>();
    query.Any<Tag, Active>().None<Group>();
    auto first = world.CreateEntity(Signature<Position, Tag>(world));
    EXPECT_EQ(query.Count(), 1u);
    auto second = world.CreateEntity(Signature<Position, Active, Velocity>(world));
    auto third = world.CreateEntity(Signature<Position, Tag, Group>(world));
    size_t withVelocity = 0, withoutVelocity = 0;
    query.Each([&](Entity, const Position&, const Velocity* velocity) { velocity ? ++withVelocity : ++withoutVelocity; });
    EXPECT_EQ(withVelocity, 1u); EXPECT_EQ(withoutVelocity, 1u); EXPECT_EQ(query.GetCachedArchetypeCount(), 2u);
    world.DestroyEntity(second); world.AddComponent<Velocity>(first);
    EXPECT_EQ(query.Count(), 1u); EXPECT_EQ(world.Query<>().All<Group>().Count(), 1u);
    EXPECT_TRUE(world.IsAlive(third));
}
TEST(ECSQuery, EnableStatesDoNotChangeArchetypes)
{
    World world; Register(world);
    const auto entities = world.CreateEntities(3, Signature<Position, Active>(world));
    const auto archetype = world.GetEntityArchetype(entities[0]);
    world.SetEnabled<Active>(entities[0], false);
    EXPECT_EQ(world.GetEntityArchetype(entities[0]), archetype);
    EXPECT_EQ(world.Query<Read<Active>>().Count(), 2u);
    EXPECT_EQ(world.Query<Read<Active>>().Disabled<Active>().Count(), 1u);
    EXPECT_EQ(world.Query<Read<Active>>().Enabled<Active>().Count(), 2u);
    EXPECT_EQ(world.Query<Read<Active>>().IncludeDisabled().Count(), 3u);
    EXPECT_EQ(world.Query<Read<Position>>().Any<Active>().Count(), 2u);
}
TEST(ECSQuery, ReadOnlyAndCountingDoNotMarkChanged)
{
    World world; Register(world);
    const auto entity = world.CreateEntity(Signature<Position, Velocity>(world));
    const auto cursor = world.GetChangeToken();
    world.Query<Read<Position>>().Each([](Entity, const Position&) {});
    EXPECT_EQ(world.Query<Write<Position>>().Count(), 1u);
    EXPECT_EQ(world.Query<Read<Position>>().Changed<Position>(cursor).Count(), 0u);
    world.WriteComponent<Velocity>(entity).x = 4;
    EXPECT_EQ(world.Query<Read<Position>>().Changed<Position>(cursor).Count(), 0u);
    world.Query<Write<Position>>().Each([](Entity, Position& value) { value.x = 7; });
    EXPECT_EQ(world.Query<Read<Position>>().Changed<Position>(cursor).Count(), 1u);
    EXPECT_EQ(world.ReadComponent<Position>(entity).x, 7);
}
TEST(ECSQuery, ChangedIsConservativePerChunkAndRejectsForeignTokens)
{
    World world, other; Register(world);
    const auto entities = world.CreateEntities(2, Signature<Position>(world));
    const auto cursor = world.GetChangeToken();
    world.WriteComponent<Position>(entities[0]).x = 1;
    EXPECT_EQ(world.Query<Read<Position>>().Changed<Position>(cursor).Count(), 2u);
    EXPECT_THROW(world.Query<Read<Position>>().Changed<Position>(other.GetChangeToken()), std::invalid_argument);
}
TEST(ECSQuery, StructuralMutationAndUndeclaredAccessAreRejected)
{
    World world; Register(world);
    const auto entity = world.CreateEntity(Signature<Position, Velocity>(world));
    world.Query<Read<Position>>().Each([&](Entity e, const Position&)
    {
        EXPECT_THROW(world.DestroyEntity(e), std::logic_error);
        EXPECT_THROW(world.AddComponent<Tag>(e), std::logic_error);
        EXPECT_THROW(world.WriteComponent<Position>(e), std::logic_error);
        EXPECT_THROW(world.ReadComponent<Velocity>(e), std::logic_error);
        EXPECT_THROW(world.Query<Read<Velocity>>().Count(), std::logic_error);
        EXPECT_THROW(world.GetChangeToken(), std::logic_error);
    });
    EXPECT_TRUE(world.IsAlive(entity));
    EXPECT_NO_THROW(world.AddComponent<Tag>(entity));
}
TEST(ECSQuery, ReplacingChangedCursorPreservesCacheAndDoesNotGrowFilters)
{
    World world; Register(world); world.CreateEntity(Signature<Position>(world));
    auto query = world.Query<Read<Position>>(); query.Changed<Position>({world.GetId(), 0});
    EXPECT_EQ(query.Count(), 1u); EXPECT_EQ(query.GetCachedArchetypeCount(), 1u);
    for (int iteration = 0; iteration < 20; ++iteration)
    {
        query.Changed<Position>(world.GetChangeToken());
        EXPECT_EQ(query.GetCachedArchetypeCount(), 1u); EXPECT_EQ(query.Count(), 0u);
        EXPECT_EQ(query.Description().changed.size(), 1u); EXPECT_EQ(query.Description().access.size(), 1u);
    }
}
TEST(ECSQuery, EntityFiltersChunkSpansAndSharedAccess)
{
    World world; Register(world);
    const auto entities = world.CreateEntities(10, Signature<Position, Group>(world));
    std::array selected{entities[2], entities[7]};
    auto query = world.Query<Write<Position>, Read<Group>>(); query.WithEntities(selected);
    query.EachChunk([&](ChunkView& view)
    {
        auto positions = view.WriteSpan<Position>(); EXPECT_EQ(view.Shared<Group>().value, 0);
        EXPECT_THROW(view.WriteSpan<Velocity>(), std::logic_error);
        for (size_t row = 0; row < view.Size(); ++row) if (view.Matches(row)) positions[row].x = 5;
    });
    for (size_t row = 0; row < entities.size(); ++row)
        EXPECT_EQ(world.ReadComponent<Position>(entities[row]).x, row == 2 || row == 7 ? 5 : 0);
}
TEST(ECSQuery, QueryOutlivingWorldRejectsAccess)
{
    std::unique_ptr<EntityQuery<>> query;
    { auto world = std::make_unique<World>(); query = std::make_unique<EntityQuery<>>(*world); }
    EXPECT_THROW(query->Count(), std::logic_error);
    EXPECT_THROW(query->Each([](Entity) {}), std::logic_error);
    EXPECT_THROW(query->All<Tag>(), std::logic_error);
}
