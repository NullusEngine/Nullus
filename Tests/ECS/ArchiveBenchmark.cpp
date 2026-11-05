#include <ECS/Core/World.h>
#include <ECS/Serialization/WorldArchive.h>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{
using namespace NLS::ECS;
using Clock = std::chrono::steady_clock;
struct Position { float x = 1, y = 2, z = 3; };
template<class Function> double Measure(Function&& function)
{ const auto begin = Clock::now(); function(); return std::chrono::duration<double, std::milli>(Clock::now() - begin).count(); }
uint64_t AvailableMemory()
{
#if defined(_WIN32)
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) return memory.ullAvailPhys;
#endif
    return std::numeric_limits<uint64_t>::max();
}
void Row(const char* operation, size_t entities, double ms, size_t bytes, uint64_t memory, const char* status = "ok")
{ std::cout << operation << ',' << entities << ',' << ms << ',' << bytes << ',' << memory << ',' << status << '\n' << std::flush; }
void Run(size_t count, uint64_t available)
{
    std::cerr << "begin_entities=" << count << '\n';
    ArchiveLimits limits; limits.maxBytes = size_t(2) * 1024 * 1024 * 1024;
    WorldConfig config; config.enableStatistics = false;
    auto registry = std::make_shared<ComponentRegistry>(); WorldDocument document;
    double ms;
    {
        World source(config, registry); const auto type = source.RegisterComponent<Position>("archive-benchmark.Position").id;
        RegisterComponentCodec<Position>(*registry,
            [](const Position& value, const auto&) { return ComponentJson{{"x", value.x}, {"y", value.y}, {"z", value.z}}; },
            [](Position& value, const ComponentJson& json, const auto&) { value = {json.at("x").get<float>(), json.at("y").get<float>(), json.at("z").get<float>()}; });
        const std::array signature{type}; source.CreateEntities(count, signature);
        ms = Measure([&] { document = WorldArchive::CaptureAll(source, DocumentKind::Scene, limits); });
        Row("scene_capture", count, ms, 0, available);
    }
    std::string text;
    ms = Measure([&] { text = WorldArchive::Encode(document, limits); }); Row("scene_encode", count, ms, text.size(), available);
    // Release the capture's JSON values before the largest parse allocation.
    document = {};
    ms = Measure([&] { document = WorldArchive::Parse(text, limits); }); Row("scene_parse", count, ms, text.size(), available);
    const auto file = std::filesystem::path("tmp") / ("ecs-archive-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()) + ".json");
    struct Cleanup { std::filesystem::path file; ~Cleanup() { std::error_code error; std::filesystem::remove(file, error); } } cleanup{file};
    ms = Measure([&] { WorldArchive::WriteFile(file, document, limits); }); Row("scene_file_write", count, ms, text.size(), available);
    document = {};
    ms = Measure([&] { document = WorldArchive::ReadFile(file, limits); }); Row("scene_file_read", count, ms, text.size(), available);
    ArchiveLoadOptions options; options.sceneOwner = 1; options.limits = limits;
    {
        World target(config, registry); ArchiveInstance instance;
        ms = Measure([&] { instance = WorldArchive::Instantiate(target, document, options); }); Row("scene_instantiate", count, ms, text.size(), available);
        double checksum = 0; target.Query<Read<Position>>().Each([&](Entity, const Position& value) { checksum += value.x; });
        if (checksum != double(count) || target.GetEntityCount() != count) throw std::runtime_error("Scene benchmark data mismatch");
        ms = Measure([&] { target.UnloadScene(1); }); Row("scene_unload", count, ms, text.size(), available);
        if (target.GetEntityCount() != 0) throw std::runtime_error("Scene benchmark unload mismatch");
    }
    WorldDocument prefab; prefab.kind = DocumentKind::Prefab;
    prefab.entities.assign(document.entities.begin(), document.entities.begin() + 2);
    document = {}; text.clear(); text.shrink_to_fit();
    World target(config, registry); std::vector<ArchiveInstance> batch;
    ms = Measure([&] { batch = WorldArchive::InstantiatePrefabs(target, prefab, count / 2, options); }); Row("prefab_batch_instantiate", count, ms, 0, available);
    if (target.GetEntityCount() != count || batch.size() != count / 2) throw std::runtime_error("Prefab benchmark batch count mismatch");
    double checksum = 0; target.Query<Read<Position>>().Each([&](Entity, const Position& value) { checksum += value.x; });
    if (checksum != double(count)) throw std::runtime_error("Prefab benchmark data mismatch");
    ms = Measure([&] { for (auto& instance : batch) instance.Destroy(target); }); Row("prefab_batch_destroy", count, ms, 0, available);
    if (target.GetEntityCount() != 0) throw std::runtime_error("Prefab benchmark destruction mismatch");
}
}
int main(int argc, char** argv)
{
    size_t maximum = 1'000'000;
    if (argc > 1)
    {
        const std::string_view text(argv[1]); const auto parsed = std::from_chars(text.data(), text.data() + text.size(), maximum);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return 2;
    }
    std::cout << std::fixed << std::setprecision(6) << "operation,entities,total_ms,encoded_bytes,available_memory_before,status\n";
    for (const size_t count : {1'000u, 10'000u, 100'000u, 1'000'000u})
    {
        const auto memory = AvailableMemory();
        if (count > maximum) { Row("scale", count, 0, 0, memory, "not_run_configured_limit"); continue; }
        // JSON object maps have much greater overhead than runtime chunks. Keep
        // headroom for other applications rather than forcing a large allocation.
        if (count == 1'000'000 && memory < uint64_t(8) * 1024 * 1024 * 1024)
        { Row("scale", count, 0, 0, memory, "not_run_available_memory"); continue; }
        try { Run(count, memory); }
        catch (const std::bad_alloc&) { Row("allocation", count, 0, 0, memory, "out_of_memory"); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    return 0;
}
