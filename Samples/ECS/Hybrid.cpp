#include <Engine/ECS/RuntimeWorld.h>
#include <Engine/Components/CameraComponent.h>
#include <Engine/Components/TransformComponent.h>
#include <Scripting/RuntimePlaySession.h>
#include <Scripting/LuaScriptBackend.h>
#include <Scripting/ScriptComponent.h>
#include <cmath>
#include <iostream>

namespace
{
class EnemyMovement final : public NLS::ECS::System
{
public:
    void OnUpdate(NLS::ECS::SystemContext& context) override
    {
        context.world.Query<NLS::ECS::Write<NLS::Engine::ECSIntegration::WorldPose>>()
            .Each([&](NLS::ECS::Entity, NLS::Engine::ECSIntegration::WorldPose& pose)
            { pose.position[0] += static_cast<float>(context.time.deltaTime) * 10; });
    }
};
}
int main()
{
#if NLS_HAS_LUA_VM
    try
    {
        using namespace NLS::Scripting;
        using namespace NLS::Engine;
        using namespace NLS::Engine::ECSIntegration;
        ScriptApiDatabase api; ScriptTypeDescriptor type; type.name = "HybridPlayer"; type.id = MakeStableScriptId(type.name);
        type.fields.push_back({1, "updates", {0, ScriptValueKind::Int32, "int", 4, 4, true}});
        if (!api.RegisterType(type)) throw std::runtime_error("Script type registration failed");
        ScriptRuntime scripts;
        if (!scripts.RegisterBackend(std::make_unique<LuaScriptBackend>()).Succeeded() || !scripts.Initialize(api).Succeeded())
            throw std::runtime_error("Lua initialization failed");
        SceneSystem::SceneManager scenes;
        auto& player = scenes.GetCurrentScene()->CreateGameObject("Player");
        auto& camera = scenes.GetCurrentScene()->CreateGameObject("Camera"); camera.AddComponent<Components::CameraComponent>();
        auto* script = player.AddComponent<ScriptComponent>();
        ScriptAsset asset; asset.assetId = NLS::Core::Assets::AssetId::New(); asset.language = ScriptLanguage::Lua;
        asset.scriptType = type.id; asset.sourcePath = "HybridPlayer.lua";
        asset.sourceText = "return {updates=0, Update=function(self) self.updates=self.updates+1 end}";
        script->SetScriptAsset(asset);
        RuntimePlaySession session;
        session.EnableECS([](RuntimeWorld& runtime, SceneSystem::Scene& scene)
        {
            auto entity = runtime.GetWorld().CreateEntity();
            auto& enemy = scene.CreateGameObject("Enemy presentation");
            HybridLinkOptions options; options.authority = TransformAuthority::Entity; options.destroyObjectWithEntity = true;
            runtime.Link(entity, enemy, options);
            NLS::ECS::SystemDescription movement{"EnemyMovement"}; movement.Writes<WorldPose>(runtime.GetWorld());
            runtime.GetWorld().Systems().RegisterSystem<EnemyMovement>(movement);
        });
        std::string error;
        if (!session.ConfigurePhysicsTiming({0.02, 8, 0.25}, error)) throw std::runtime_error(error);
        const auto begin = session.Begin(scripts, scenes, 9001); if (!begin.Succeeded()) throw std::runtime_error(begin.message);
        scenes.GetCurrentScene()->Play();
        for (int frame = 0; frame < 3; ++frame) session.Tick(0.02f);
        ScriptValue value; if (!script->GetRuntimeSerializedField(1, value)) throw std::runtime_error("Lua field read failed");
        const int playerUpdates = std::get<int32_t>(value);
        const float enemyX = scenes.GetCurrentScene()->FindGameObjectByName("Enemy presentation")->GetTransform()->GetWorldPosition().x;
        std::cout << "legacy_player_updates=" << playerUpdates << " ecs_enemy_x=" << enemyX
            << " game_objects=" << scenes.GetCurrentScene()->GetGameObjects().size()
            << " entities=" << session.GetECS()->GetWorld().GetEntityCount() << '\n';
        const bool valid = playerUpdates == 3 && std::abs(enemyX - 0.6f) < 1e-5f;
        if (!session.End().Succeeded()) return 1;
        return valid ? 0 : 1;
    }
    catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
#else
    std::cerr << "This sample requires NLS_ENABLE_LUA_SCRIPTING.\n";
    return 2;
#endif
}
