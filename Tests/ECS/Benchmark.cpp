#include <ECS/Commands/EntityCommandBuffer.h>
#include <Base/Jobs/JobSystem.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>

namespace
{
using namespace NLS::ECS;
struct Position { float x = 0, y = 0, z = 0; };
struct Velocity { float x = 1, y = 2, z = 3; };
struct Health { int value = 100; };
using Clock = std::chrono::steady_clock;
template<class Function> double Measure(Function&& function)
{ const auto start = Clock::now(); function(); return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void Row(const char* operation, size_t count, size_t operations, double ms, const World& world)
{
    const auto stats = world.GetStatistics();
    const auto knownBytes = stats.chunkBytes + stats.metadataBytes + stats.sharedBytes;
    std::cout << operation << ',' << count << ',' << operations << ',' << ms << ','
        << (operations ? ms * 1e6 / double(operations) : 0) << ',' << (ms > 0 ? double(operations) * 1000 / ms : 0)
        << ',' << knownBytes << ',' << stats.chunks << ',' << stats.Occupancy() << ",ok\n";
}
void Run(size_t count, size_t chunkBytes)
{
    WorldConfig config; config.chunkBytes = chunkBytes; config.enableStatistics = false;
    World world(config);
    const auto position = world.RegisterComponent<Position>("benchmark.Position").id;
    const auto velocity = world.RegisterComponent<Velocity>("benchmark.Velocity").id;
    world.RegisterComponent<Health>("benchmark.Health");
    std::vector<Entity> entities;
    const std::array signature{position, velocity};
    auto ms = Measure([&] { entities = world.CreateEntities(count, signature); });
    Row("create", count, count, ms, world);
    auto move = world.Query<Write<Position>, Read<Velocity>>();
    constexpr size_t iterations = 32;
    ms = Measure([&] { for (size_t i = 0; i < iterations; ++i) move.Each([](Entity, Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; p.z += v.z; }); });
    Row("position_single", count, count * iterations, ms, world);
    ms = Measure([&] { for (size_t i = 0; i < iterations; ++i) move.ParallelEach([](Entity, Position& p, const Velocity& v) { p.x += v.x; p.y += v.y; p.z += v.z; }); });
    Row("position_parallel", count, count * iterations, ms, world);
    double checksum = 0;
    ms = Measure([&] { world.Query<Read<Position>>().Each([&](Entity, const Position& p) { checksum += p.x; }); });
    if (checksum != double(count) * iterations * 2) throw std::runtime_error("Benchmark data validation failed");
    Row("query_read", count, count, ms, world);
    ms = Measure([&] { for (auto entity : entities) world.AddComponent<Health>(entity); }); Row("component_add_migrate", count, count, ms, world);
    ms = Measure([&] { for (auto entity : entities) world.RemoveComponent<Health>(entity); }); Row("component_remove_migrate", count, count, ms, world);
    {
        EntityCommandBuffer commands(world); auto writer = commands.GetWriter();
        ms = Measure([&] { for (auto entity : entities) writer.SetComponent<Position>(entity, {1, 2, 3}); });
        Row("command_record", count, count, ms, world);
        CommandPlaybackResult result;
        ms = Measure([&] { result = commands.Playback(); });
        if (!result.Succeeded() || result.applied != count) throw std::runtime_error("Benchmark command playback failed");
        Row("command_playback", count, count, ms, world);
    }
    ms = Measure([&] { world.DestroyEntities(entities); }); Row("destroy", count, count, ms, world);
    ms = Measure([&] { world.TrimEmptyChunks(); }); Row("chunk_trim", count, 1, ms, world);
}
}
int main(int argc, char** argv)
{
    size_t chunkBytes = 16 * 1024;
    if (argc > 1)
    {
        const std::string_view argument(argv[1]); const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), chunkBytes);
        if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size()) return 2;
    }
    NLS::Base::Jobs::JobSystemConfig config; config.workerCount = 4;
    if (!NLS::Base::Jobs::TryInitializeJobSystem(config)) return 1;
    struct JobsLifetime { ~JobsLifetime() { NLS::Base::Jobs::ShutdownJobSystem(); } } jobs;
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "operation,entities,operations,total_ms,ns_per_operation,operations_per_second,known_world_bytes,chunks,occupancy,status\n";
    for (const size_t count : {1000u, 10000u, 100000u, 1000000u})
    {
        try { Run(count, chunkBytes); }
        catch (const std::bad_alloc&) { std::cout << "allocation," << count << ",0,0,0,0,0,0,0,out_of_memory\n"; }
        catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
    }
    return 0;
}
