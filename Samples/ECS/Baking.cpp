#include <Engine/ECS/Baking.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/GameObject.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Components/TransformComponent.h>
#include <iostream>

int main(int argc, char** argv)
{
    using namespace NLS::ECS;
    using namespace NLS::Engine::ECSIntegration;
    try
    {
        const std::filesystem::path path = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("tmp/ECSBakedScene.json");
        World schemas; RegisterTransformComponents(schemas); BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
        const NLS::Engine::Serialize::ObjectId rootId{NLS::Guid::NewDeterministic("ECS.Baking.Demo.Root")};
        const NLS::Engine::Serialize::ObjectId childId{NLS::Guid::NewDeterministic("ECS.Baking.Demo.Child")};
        {
            NLS::Engine::SceneSystem::Scene authoring;
            auto& root = authoring.CreateGameObject("authoring root"); auto& child = authoring.CreateGameObject("authoring child");
            root.GetTransform()->SetLocalPosition({7, 0, 0}); child.SetParent(root); child.GetTransform()->SetLocalPosition({4, 0, 0});
            const std::array sources{BakingSource{rootId, root.GetInstanceID()}, BakingSource{childId, child.GetInstanceID()}};
            BakingOptions options; options.outputPath = path;
            const auto full = pipeline.Bake(sources, options);
            if (!full.succeeded) throw std::runtime_error(full.diagnostics.at(0));
            root.GetTransform()->SetLocalPosition({9, 0, 0}); const auto incremental = pipeline.Bake(sources, options);
            if (!incremental.succeeded) throw std::runtime_error(incremental.diagnostics.at(0));
            std::cout << "full_rebuilt=" << full.statistics.rebuilt << " incremental_rebuilt=" << incremental.statistics.rebuilt
                << " incremental_reused=" << incremental.statistics.reused << '\n';
        }
        // Only the baked file and component schemas survive authoring teardown.
        World runtime; RegisterTransformSystem(runtime);
        const auto loaded = WorldArchive::Instantiate(runtime, WorldArchive::ReadFile(path), {.sceneOwner = 17});
        runtime.Systems().RunPhase(SystemPhase::LateUpdate, {});
        const auto child = loaded.byAuthoringId.at(childId.GetGuid().ToString());
        const auto x = runtime.ReadComponent<WorldTransform>(child).matrix[3];
        std::cout << "loaded_entities=" << runtime.GetEntityCount() << " child_world_x=" << x << " file=" << path.generic_string() << '\n';
        if (runtime.GetEntityCount() != 2 || x != 13) return 1;
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
