#include <ECS/Systems/SystemScheduler.h>
#include <Base/Jobs/JobSystem.h>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>

namespace
{
struct Position { float x = 0, y = 0, z = 0; };
struct Velocity { float x = 1, y = 0, z = 0; };
class Movement final : public NLS::ECS::System
{
public:
    void OnCreate(NLS::ECS::World& world) override
    { query = std::make_unique<NLS::ECS::EntityQuery<NLS::ECS::Write<Position>, NLS::ECS::Read<Velocity>>>(world); }
    void OnUpdate(NLS::ECS::SystemContext& context) override
    {
        const auto move = [&](NLS::ECS::Entity, Position& position, const Velocity& velocity)
        { position.x += velocity.x * static_cast<float>(context.time.deltaTime); };
        if (context.jobsEnabled) query->ParallelEach(move); else query->Each(move);
    }
private:
    std::unique_ptr<NLS::ECS::EntityQuery<NLS::ECS::Write<Position>, NLS::ECS::Read<Velocity>>> query;
};
}
int main(int argc, char** argv)
{
    try
    {
        size_t count = 100000; bool single = false;
        for (int index = 1; index < argc; ++index)
        {
            std::string_view argument(argv[index]);
            if (argument == "--single") single = true;
            else
            {
                const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), count);
                if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || count > 10000000)
                    throw std::invalid_argument("Usage: NullusECSMassSimulation [entity-count <= 10000000] [--single]");
            }
        }
        NLS::ECS::WorldConfig config; config.singleThreaded = single;
        NLS::ECS::World world(config);
        const auto p = world.RegisterComponent<Position>("sample.Position").id;
        const auto v = world.RegisterComponent<Velocity>("sample.Velocity").id;
        const std::array signature{p, v}; world.CreateEntities(count, signature);
        NLS::ECS::SystemDescription description{"Movement"}; description.mainThread = false;
        description.Writes<Position>(world).Reads<Velocity>(world);
        auto& systems = world.Systems(); systems.RegisterSystem<Movement>(description);
        struct JobsLifetime
        {
            bool owned;
            ~JobsLifetime() { if (owned) NLS::Base::Jobs::ShutdownJobSystem(); }
        } jobs{!single};
        NLS::Base::Jobs::JobSystemConfig jobConfig; jobConfig.workerCount = 4;
        if (!single && !NLS::Base::Jobs::TryInitializeJobSystem(jobConfig)) throw std::runtime_error("JobSystem startup failed");
        NLS::ECS::SystemTime time; time.deltaTime = 0.01;
        const auto started = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 100; ++frame) { time.frameIndex = frame + 1; systems.RunPhase(NLS::ECS::SystemPhase::Update, time); }
        const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        double checksum = 0; bool valid = true;
        world.Query<NLS::ECS::Read<Position>>().Each([&](NLS::ECS::Entity, const Position& position)
        { checksum += position.x; valid = valid && std::abs(position.x - 1.0f) < 1e-5f; });
        const auto stats = world.GetStatistics();
        std::cout << "entities=" << count << " frames=100 mode=" << (single ? "single" : "parallel")
            << " elapsed_ms=" << milliseconds << " chunks=" << stats.chunks << " jobs=" << stats.jobs << " checksum=" << checksum << '\n';
        return valid ? 0 : 1;
    }
    catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}
