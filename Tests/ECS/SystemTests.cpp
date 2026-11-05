#include "TestComponents.h"
#include <ECS/Systems/SystemScheduler.h>
using namespace NLS::ECS;
using namespace ECSTest;

namespace
{
class FunctionSystem final : public System
{
public:
    explicit FunctionSystem(std::function<void(SystemContext&)> function) : callback(std::move(function)) {}
    void OnUpdate(SystemContext& context) override { callback(context); }
    std::function<void(SystemContext&)> callback;
};
struct Lifecycle { int create = 0, start = 0, update = 0, stop = 0, destroy = 0; };
class LifecycleSystem final : public System
{
public:
    explicit LifecycleSystem(Lifecycle& value) : counts(value) {}
    void OnCreate(World&) override { ++counts.create; }
    void OnStart(World&) override { ++counts.start; }
    void OnUpdate(SystemContext&) override { ++counts.update; }
    void OnStop(World&) noexcept override { ++counts.stop; }
    void OnDestroy(World&) noexcept override { ++counts.destroy; }
    Lifecycle& counts;
};
class TeardownSystem final : public System
{
public:
    explicit TeardownSystem(bool& destroyed) : m_destroyed(destroyed) {}
    void OnUpdate(SystemContext&) override {}
    void OnDestroy(World& world) noexcept override
    {
        // A teardown callback must see the existing scheduler, not instantiate
        // a new scheduler after unique_ptr::reset cleared the owning pointer.
        EXPECT_EQ(world.Systems().Find("teardown"), this);
        EXPECT_THROW(world.Systems().SetEnabled("teardown", false), std::logic_error);
        world.Close(); // Idempotent reentrant request, outer close owns cleanup.
        m_destroyed = true;
    }
private:
    bool& m_destroyed;
};
}
TEST(ECSSystems, LifecycleEnableDisableStopAndWorldDestruction)
{
    Lifecycle counts;
    {
        World world; auto& systems = world.Systems();
        systems.RegisterSystem<LifecycleSystem>({"life"}, counts);
        EXPECT_EQ(counts.create, 1); systems.RunPhase(SystemPhase::Update, {});
        EXPECT_EQ(counts.start, 1); EXPECT_EQ(counts.update, 1);
        EXPECT_EQ(systems.Find<LifecycleSystem>(), systems.Find("life"));
        systems.SetEnabled("life", false); systems.RunPhase(SystemPhase::Update, {});
        EXPECT_EQ(counts.stop, 1); EXPECT_EQ(counts.update, 1);
        systems.SetEnabled("life", true); systems.RunPhase(SystemPhase::Update, {});
        EXPECT_EQ(counts.start, 2); EXPECT_EQ(counts.update, 2);
        systems.Stop(); systems.Stop(); EXPECT_EQ(counts.stop, 2);
    }
    EXPECT_EQ(counts.destroy, 1);
}
TEST(ECSSystems, ExplicitReverseOrderWinsOverAutomaticHazards)
{
    World world; Register(world); world.SetResource<Position>({}); auto& systems = world.Systems();
    std::vector<int> order;
    SystemDescription first{"first"}; first.WritesResource<Position>(world); first.after = {"second"};
    SystemDescription second{"second"}; second.WritesResource<Position>(world);
    systems.RegisterSystem<FunctionSystem>(first, [&](SystemContext& c) { order.push_back(1); c.world.WriteResource<Position>().x += 1; });
    systems.RegisterSystem<FunctionSystem>(second, [&](SystemContext& c) { order.push_back(2); c.world.WriteResource<Position>().x = 10; });
    systems.RunPhase(SystemPhase::Update, {});
    EXPECT_EQ(order, (std::vector<int>{2, 1})); EXPECT_EQ(world.ReadResource<Position>().x, 11);
    EXPECT_EQ(systems.GetExecutionOrder(SystemPhase::Update), (std::vector<std::string>{"second", "first"}));
}
TEST(ECSSystems, ReadWriteAndResourceConflictsProduceDependencies)
{
    World world; Register(world); auto& systems = world.Systems();
    SystemDescription writer{"writer"}; writer.Writes<Position>(world);
    SystemDescription reader{"reader"}; reader.Reads<Position>(world);
    SystemDescription other{"resource"}; other.WritesResource<Position>(world);
    const auto empty = [](SystemContext&) {};
    systems.RegisterSystem<FunctionSystem>(writer, empty); systems.RegisterSystem<FunctionSystem>(reader, empty);
    systems.RegisterSystem<FunctionSystem>(other, empty);
    auto edges = systems.GetDependencies(); ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].from, "writer"); EXPECT_EQ(edges[0].to, "reader"); EXPECT_EQ(edges[0].reason, "Read/write conflict");
    systems.SetEnabled("writer", false); EXPECT_TRUE(systems.GetDependencies().empty());
}
TEST(ECSSystems, CyclesUnknownDependenciesAndContradictoryPhasesFailBeforeUpdate)
{
    World world; auto& systems = world.Systems(); int updates = 0;
    SystemDescription a{"a"}, b{"b"}; a.after = {"b"}; b.after = {"a"};
    systems.RegisterSystem<FunctionSystem>(a, [&](SystemContext&) { ++updates; });
    systems.RegisterSystem<FunctionSystem>(b, [&](SystemContext&) { ++updates; });
    EXPECT_THROW(systems.RunPhase(SystemPhase::Update, {}), std::logic_error); EXPECT_EQ(updates, 0);
    systems.SetEnabled("b", false); EXPECT_NO_THROW(systems.ValidateDependencies());
    systems.Remove("b"); EXPECT_THROW(systems.ValidateDependencies(), std::invalid_argument);
    systems.Remove("a"); a.after = {"late"}; a.phase = SystemPhase::Update;
    b.name = "late"; b.after.clear(); b.phase = SystemPhase::LateUpdate;
    systems.RegisterSystem<FunctionSystem>(a, [](SystemContext&) {}); systems.RegisterSystem<FunctionSystem>(b, [](SystemContext&) {});
    EXPECT_THROW(systems.ValidateDependencies(), std::logic_error);
}
TEST(ECSSystems, PhaseExecutionAndTimeAreExplicit)
{
    World world; auto& systems = world.Systems(); std::vector<SystemPhase> phases;
    for (int index = 0; index < int(SystemPhase::Count); ++index)
    {
        SystemDescription description{std::to_string(index)}; description.phase = static_cast<SystemPhase>(index);
        systems.RegisterSystem<FunctionSystem>(description, [&](SystemContext& context)
        { phases.push_back(context.phase); EXPECT_DOUBLE_EQ(context.time.deltaTime, 0.125); });
    }
    SystemTime time; time.deltaTime = 0.125;
    systems.RunPhase(SystemPhase::PreFixedUpdate, time); systems.RunPhase(SystemPhase::FixedSimulation, time);
    systems.RunPhase(SystemPhase::PostPhysics, time); systems.RunPhase(SystemPhase::Update, time);
    EXPECT_EQ(phases, (std::vector<SystemPhase>{SystemPhase::PreFixedUpdate, SystemPhase::FixedSimulation, SystemPhase::PostPhysics, SystemPhase::Update}));
}
TEST(ECSSystems, SystemQueriesMustFitDeclaredPermissions)
{
    World world; Register(world); world.CreateEntity(Signature<Position, Velocity>(world));
    auto& systems = world.Systems(); SystemDescription description{"bad"}; description.Reads<Position>(world);
    systems.RegisterSystem<FunctionSystem>(description, [](SystemContext& context)
    { context.world.Query<Write<Position>>().Each([](Entity, Position& p) { p.x = 1; }); });
    EXPECT_THROW(systems.RunPhase(SystemPhase::Update, {}), std::logic_error);
    EXPECT_EQ(world.Query<Read<Position>>().Count(), 1u);
    EXPECT_FALSE(systems.GetStatistics()[0].lastError.empty());
}
TEST(ECSSystems, CommandsPlayBackAfterEverySystemInThePhase)
{
    World world; Register(world); world.CreateEntities(10, Signature<Position>(world)); auto& systems = world.Systems();
    SystemDescription destroy{"destroy"}; destroy.Reads<Position>(world);
    systems.RegisterSystem<FunctionSystem>(destroy, [](SystemContext& context)
    { context.world.Query<Read<Position>>().Each([&](Entity e, const Position&) { context.commands.DestroyEntity(e); }); });
    SystemDescription observer{"observer"}; observer.after = {"destroy"}; observer.Reads<Position>(world);
    systems.RegisterSystem<FunctionSystem>(observer, [](SystemContext& context) { EXPECT_EQ(context.world.Query<Read<Position>>().Count(), 10u); });
    systems.RunPhase(SystemPhase::Update, {});
    EXPECT_EQ(world.GetEntityCount(), 0u); EXPECT_EQ(systems.GetLastPlayback().applied, 10u);
}
TEST(ECSSystems, WorldResourcesRequireSeparateDeclarations)
{
    World world; Register(world); world.SetResource<Position>({}); auto& systems = world.Systems();
    SystemDescription description{"resource"}; description.WritesResource<Position>(world);
    systems.RegisterSystem<FunctionSystem>(description, [](SystemContext& context)
    {
        context.world.WriteResource<Position>().x = 9;
        EXPECT_THROW(context.world.CreateEntity(), std::logic_error);
        EXPECT_THROW(context.world.ReadResource<Velocity>(), std::logic_error);
    });
    systems.RunPhase(SystemPhase::Update, {}); EXPECT_EQ(world.ReadResource<Position>().x, 9);
}
TEST(ECSSystems, ChangedQueriesUseTheLastSuccessfulSystemUpdate)
{
    World world; Register(world); const auto entity = world.CreateEntity(Signature<Position>(world));
    auto& systems = world.Systems(); SystemDescription description{"changes"}; description.Reads<Position>(world);
    std::vector<size_t> observed;
    systems.RegisterSystem<FunctionSystem>(description, [&](SystemContext& context)
    { observed.push_back(context.world.Query<Read<Position>>().Changed<Position>(context.lastUpdate).Count()); });
    systems.RunPhase(SystemPhase::Update, {}); systems.RunPhase(SystemPhase::Update, {});
    world.WriteComponent<Position>(entity).x = 7;
    systems.RunPhase(SystemPhase::Update, {}); systems.RunPhase(SystemPhase::Update, {});
    EXPECT_EQ(observed, (std::vector<size_t>{1, 0, 1, 0}));
    systems.SetEnabled("changes", false); systems.SetEnabled("changes", true);
    systems.RunPhase(SystemPhase::Update, {}); EXPECT_EQ(observed.back(), 1u);
}
TEST(ECSSystems, WorldTeardownKeepsSchedulerIdentityUntilCallbacksFinish)
{
    bool destroyed = false; World world;
    world.Systems().RegisterSystem<TeardownSystem>({"teardown"}, destroyed);
    world.Close(); EXPECT_TRUE(destroyed); EXPECT_TRUE(world.IsClosed());
}
