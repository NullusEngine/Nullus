#include <ECS/Core/World.h>
#include <ECS/Serialization/SceneStreaming.h>
#include <Base/Jobs/JobSystem.h>
#include <array>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{
using namespace NLS::ECS;
struct Position { float x = 0; Entity target; };
void Register(World& world)
{
    world.RegisterComponent<Position>("streaming-example.Position");
    RegisterComponentCodec<Position>(world.GetRegistry(),
        [](const Position& value, const EntityWriteContext& context)
        { return ComponentJson{{"x", value.x}, {"target", context.EntityId(value.target)}}; },
        [](Position& value, const ComponentJson& json, const EntityReadContext& context)
        { value = {json.at("x").get<float>(), context.Resolve(json.at("target").get<std::string>())}; });
}
}
int main()
{
    try
    {
        NLS::Base::Jobs::JobSystemConfig jobsConfig; jobsConfig.workerCount = 4; jobsConfig.backgroundWorkerCount = 1;
        if (!NLS::Base::Jobs::TryInitializeJobSystem(jobsConfig)) return 1;
        struct JobsLifetime { ~JobsLifetime() { NLS::Base::Jobs::ShutdownJobSystem(); } } jobs;
        struct Files
        {
            std::filesystem::path directory = std::filesystem::path("tmp") /
                ("ecs-streaming-example-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ~Files() { std::error_code error; std::filesystem::remove_all(directory, error); }
        } files;
        std::filesystem::create_directories(files.directory);
        const auto path = files.directory / "chunk.json";
        {
            World authoring; Register(authoring); const auto a = authoring.CreateEntity(), b = authoring.CreateEntity();
            authoring.AddComponent<Position>(a, {7, b}); authoring.AddComponent<Position>(b, {9, a});
            const std::array records{PersistentEntity{a, "a", {}}, PersistentEntity{b, "b", {}}};
            WorldArchive::WriteFile(path, WorldArchive::Capture(authoring, records));
        }
        World runtime; Register(runtime); SceneStreaming streaming(runtime);
        ArchiveLoadOptions first; first.sceneOwner = 1001;
        ArchiveLoadOptions second; second.sceneOwner = 1002;
        const auto a = streaming.RequestLoad(1, path, first), b = streaming.RequestLoad(2, path, second);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        size_t frames = 0;
        while (streaming.GetInfo(a).status == SceneLoadStatus::Loading || streaming.GetInfo(b).status == SceneLoadStatus::Loading)
        {
            streaming.Pump(1); // Application's owner-thread safe point.
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Streaming example timed out");
            ++frames; std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (streaming.GetInfo(a).status != SceneLoadStatus::Loaded || streaming.GetInfo(b).status != SceneLoadStatus::Loaded)
            throw std::runtime_error("Streaming example failed: " + streaming.GetInfo(a).error + streaming.GetInfo(b).error);
        const auto firstEntity = streaming.Find(1, "a"), secondEntity = streaming.Find(2, "a");
        if (runtime.ReadComponent<Position>(firstEntity).target != streaming.Find(1, "b") ||
            runtime.ReadComponent<Position>(secondEntity).target != streaming.Find(2, "b") || firstEntity == secondEntity)
            throw std::runtime_error("Streaming example reference validation failed");
        const auto count = runtime.GetEntityCount(), unloaded = streaming.Unload(1);
        std::cout << "loaded_chunks=2 entities=" << count << " background=" <<
            (streaming.GetInfo(a).parsedOnBackgroundWorker && streaming.GetInfo(b).parsedOnBackgroundWorker) <<
            " owner_pumps=" << frames << " unloaded=" << unloaded << " remaining=" << runtime.GetEntityCount() << '\n';
        streaming.UnloadAll(); streaming.Forget(a); streaming.Forget(b);
        return runtime.GetEntityCount() == 0 && unloaded == 2 ? 0 : 1;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
