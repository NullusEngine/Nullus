#include "TestComponents.h"
#include <ECS/Commands/EntityCommandBuffer.h>
#include <thread>
using namespace NLS::ECS;
using namespace ECSTest;

TEST(ECSCommands, DeferredCreationComponentOperationsAndReset)
{
    World world; Register(world); EntityCommandBuffer commands(world);
    auto writer = commands.GetWriter();
    const auto deferred = writer.CreateEntity();
    writer.AddComponent<Position>(deferred, {1, 2, 3}); writer.SetComponent<Position>(deferred, {7, 8, 9});
    writer.AddComponent<Active>(deferred); writer.SetEnabled<Active>(deferred, false);
    writer.AddComponent<Tag>(deferred); writer.RemoveComponent<Tag>(deferred);
    EXPECT_EQ(world.GetEntityCount(), 0u); EXPECT_EQ(commands.GetCommandCount(), 7u);
    const auto result = commands.Playback();
    ASSERT_TRUE(result.Succeeded()); ASSERT_TRUE(result.Resolve(deferred));
    const auto entity = *result.Resolve(deferred);
    EXPECT_EQ(world.ReadComponent<Position>(entity).x, 7); EXPECT_FALSE(world.IsEnabled<Active>(entity));
    EXPECT_FALSE(world.HasComponent<Tag>(entity)); EXPECT_EQ(result.applied, 7u);
    EXPECT_THROW(commands.Playback(), std::logic_error);
    EXPECT_THROW(writer.DestroyEntity(entity), std::logic_error);
    commands.Reset(); auto replacement = commands.GetWriter();
    EXPECT_THROW(replacement.DestroyEntity(deferred), std::invalid_argument);
    replacement.DestroyEntity(entity); EXPECT_TRUE(commands.Playback().Succeeded()); EXPECT_FALSE(world.IsAlive(entity));
}
TEST(ECSCommands, PlaybackWaitsUntilIterationEnds)
{
    World world; Register(world);
    auto entities = world.CreateEntities(50, Signature<Position>(world));
    EntityCommandBuffer commands(world); auto writer = commands.GetWriter();
    world.Query<Read<Position>>().Each([&](Entity entity, const Position&)
    { writer.DestroyEntity(entity); EXPECT_THROW(commands.Playback(), std::logic_error); });
    EXPECT_EQ(world.GetEntityCount(), 50u);
    EXPECT_TRUE(commands.Playback().Succeeded()); EXPECT_EQ(world.GetEntityCount(), 0u);
}
TEST(ECSCommands, DestroyThenSetIsDiagnosedAndNeverResurrects)
{
    World world; Register(world); auto entity = world.CreateEntity(Signature<Position>(world));
    EntityCommandBuffer commands(world);
    auto last = commands.GetWriter(9); last.SetComponent<Position>(entity, {10, 0, 0});
    auto first = commands.GetWriter(1); first.DestroyEntity(entity);
    auto result = commands.Playback();
    ASSERT_EQ(result.errors.size(), 1u); EXPECT_EQ(result.errors[0].writer, 9u);
    EXPECT_EQ(result.applied, 1u); EXPECT_FALSE(world.IsAlive(entity));
}
TEST(ECSCommands, IndependentWritersMergeInExplicitKeyOrder)
{
    World world; Register(world); EntityCommandBuffer commands(world);
    std::vector<std::thread> workers;
    for (uint64_t key = 0; key < 4; ++key)
    {
        auto writer = commands.GetWriter(key);
        workers.emplace_back([writer, key]() mutable
        {
            for (int i = 0; i < 80; ++i)
            { auto entity = writer.CreateEntity(); writer.AddComponent<Position>(entity, {float(key * 1000 + i), 0, 0}); }
        });
    }
    for (auto& worker : workers) worker.join();
    auto result = commands.Playback(); ASSERT_TRUE(result.Succeeded()); ASSERT_EQ(result.created.size(), 320u);
    for (size_t index = 0; index < result.created.size(); ++index)
        EXPECT_EQ(world.ReadComponent<Position>(result.created[index].second).x, float(index / 80 * 1000 + index % 80));
}
TEST(ECSCommands, RejectsForeignTemporaryAndStaleWorldLifetimes)
{
    World world; Register(world);
    EntityCommandBuffer first(world), second(world);
    auto writer = first.GetWriter(); auto temporary = writer.CreateEntity();
    auto other = second.GetWriter(); EXPECT_THROW(other.DestroyEntity(temporary), std::invalid_argument);
    first.Reset(); EXPECT_THROW(writer.CreateEntity(), std::logic_error);
    world.Close();
    EXPECT_THROW(second.Playback(), std::logic_error);
    EXPECT_THROW(other.CreateEntity(), std::logic_error);
}
TEST(ECSCommands, CrossWriterForwardReferencesHaveDefinedErrors)
{
    World world; Register(world); EntityCommandBuffer commands(world);
    auto creator = commands.GetWriter(5); auto future = creator.CreateEntity();
    auto earlier = commands.GetWriter(1); earlier.AddComponent<Position>(future);
    auto result = commands.Playback(); EXPECT_EQ(result.errors.size(), 1u); EXPECT_EQ(world.GetEntityCount(), 1u);
    ASSERT_TRUE(result.Resolve(future)); EXPECT_FALSE(world.HasComponent<Position>(*result.Resolve(future)));
}
TEST(ECSCommands, PayloadsAndWritersDoNotLeakComponentMemory)
{
    ASSERT_EQ(Tracked::alive, 0);
    {
        World world; Register(world);
        {
            EntityCommandBuffer commands(world); auto writer = commands.GetWriter();
            auto target = writer.CreateEntity(); writer.AddComponent<Tracked>(target, Tracked("queued"));
            auto result = commands.Playback(); ASSERT_TRUE(result.Succeeded());
            EXPECT_EQ(Tracked::alive, 1); EXPECT_EQ(world.ReadComponent<Tracked>(*result.Resolve(target)).text, "queued");
            commands.Reset(); writer = commands.GetWriter(); writer.AddComponent<Tracked>(writer.CreateEntity(), Tracked("discarded"));
        }
        EXPECT_EQ(Tracked::alive, 1);
    }
    EXPECT_EQ(Tracked::alive, 0);
}
