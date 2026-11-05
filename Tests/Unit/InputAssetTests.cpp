#include "EditorInputTestSupport.h"
#include <gtest/gtest.h>
#include <Assets/AssetDatabaseFacade.h>
#include <Assets/NativeArtifactContainer.h>
#include <Assets/ContentSource.h>
#include <Assets/NpakArchive.h>
#include <Core/Inputs/InputSerialization.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include <Engine/Assets/InputActionsAsset.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <Engine/Components/PlayerInputComponent.h>
#include <SceneSystem/SceneManager.h>
#include <Scripting/RuntimePlaySession.h>
#include <filesystem>
#include <fstream>
#include "Inspector/InspectorView.h"
#include "Shortcuts/EditorShortcutService.h"
#include <Components/CameraComponent.h>

namespace
{
using namespace NLS;
namespace A = Core::Assets; namespace E = Engine; namespace S = E::Serialize;
Guid Id(const char* name) { return Guid::NewDeterministic(name); }
struct InputAssetFixture : testing::Test
{
    // The managed Windows runner permits writes to its virtual TEMP directory
    // but denies MoveFileEx there. Exercise the real atomic-save path in the
    // configured build workspace, which also matches editor project storage.
    std::filesystem::path root = std::filesystem::path(NLS_BUILD_DIR) / "input-test-artifacts" / Guid::New().ToString();
    Input::InputDefinition definition;
    void SetUp() override
    {
        std::filesystem::create_directories(root / "Assets");
        definition.id = Id("integration-input"); definition.defaultContext = Id("integration-gameplay");
        Input::ActionDefinition action; action.id = Id("integration-jump"); action.name = "Jump"; definition.actions.push_back(action);
        Input::ContextDefinition context; context.id = definition.defaultContext; context.name = "Gameplay";
        Input::BindingDefinition binding; binding.id = Id("integration-space"); binding.action = action.id;
        binding.parts = {{{Input::DeviceKind::Keyboard, Input::ControlKind::Button, 32}, {1}}}; context.bindings.push_back(binding);
        definition.contexts.push_back(context);
    }
    void TearDown() override { std::error_code error; std::filesystem::remove_all(root, error); }
    void Save()
    {
        std::string error; ASSERT_TRUE(Core::Inputs::SaveInputDefinition(root / "Assets/Player.inputactions", definition, error)) << error;
    }
};
}

TEST_F(InputAssetFixture, ImportManifestPlayerIsolationAndActiveSubscriptionReload)
{
    Save(); Editor::Assets::AssetDatabaseFacade database({root}); ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Player.inputactions"));
    auto records = database.LoadAllAssetsAtPath("Assets/Player.inputactions"); ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].artifactType, A::ArtifactType::InputActions);
    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Player.inputactions"); ASSERT_TRUE(manifest);
    E::Assets::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest);
    auto built = builder.BuildAssetPacks({{"input", "default", {records[0].assetId, records[0].subAssetKey}}}, "win64-dx12");
    ASSERT_EQ(built.manifest.entries.size(), 1u); ASSERT_EQ(built.manifest.assetPacks.size(), 1u);
    built.manifest.entries[0].artifactPath = database.ResolveArtifactPathAtPath("Assets/Player.inputactions", records[0].subAssetKey).string();
    E::Assets::RuntimeAssetDatabase runtimeDatabase(built.manifest);
    auto* previous = Core::ServiceLocator::Contains<E::Assets::RuntimeAssetDatabase>() ? &Core::ServiceLocator::Get<E::Assets::RuntimeAssetDatabase>() : nullptr;
    Core::ServiceLocator::Provide(runtimeDatabase);
    struct Restore { E::Assets::RuntimeAssetDatabase* previous; ~Restore() { if (previous) Core::ServiceLocator::Provide(*previous); else Core::ServiceLocator::Remove<E::Assets::RuntimeAssetDatabase>(); } } restore{previous};
    const auto identifier = S::ObjectIdentifier::Asset(S::AssetId(records[0].assetId.GetGuid()),
        S::MakeLocalIdentifierInFile(records[0].assetId.GetGuid(), records[0].subAssetKey), "Assets/Player.inputactions");
    std::string error; auto asset = E::Assets::LoadInputActionsAsset(identifier, error); ASSERT_TRUE(asset) << error;
    Input::InputSystem system; Input::InputDomain domain(system, "Runtime");
    auto first = domain.CreateUser(), second = domain.CreateUser(); asset->ApplyTo(*first); asset->ApplyTo(*second);
    first->PushContext(definition.defaultContext); second->PushContext(definition.defaultContext);
    first->SetBindingOverride({Id("integration-space"), Input::OverrideState::Override,
        {{{Input::DeviceKind::Keyboard, Input::ControlKind::Button, 65}, {1}}}});
    int performed = 0, canceled = 0;
    auto subscription = first->BindAction(Id("integration-jump"), Input::ActionPhase::Performed, [&](const auto&) { ++performed; });
    auto cancellation = first->BindAction(Id("integration-jump"), Input::ActionPhase::Canceled, [&](const auto&) { ++canceled; });
    Input::InputEvent event; event.device = system.GetKeyboard(); event.control = 65; event.type = Input::InputEventType::ButtonDown;
    ASSERT_TRUE(system.SubmitEvent(event)); domain.Update(system.Now()); EXPECT_EQ(performed, 1);
    EXPECT_FALSE(second->GetActionState(second->FindAction(Id("integration-jump"))).actuated);
    const auto oldHandle = first->FindAction(Id("integration-jump"));
    definition.actions[0].name = "Jump renamed"; Save(); ASSERT_TRUE(database.ImportAsset("Assets/Player.inputactions"));
    auto updated = database.GetArtifactManifestForAssetPath("Assets/Player.inputactions"); ASSERT_TRUE(updated);
    E::Assets::RuntimeManifestBuilder reloadBuilder; reloadBuilder.AddArtifactManifest(*updated);
    auto next = reloadBuilder.Build({{records[0].assetId, records[0].subAssetKey}}, "win64-dx12"); ASSERT_EQ(next.manifest.entries.size(), 1u);
    next.manifest.entries[0].artifactPath = database.ResolveArtifactPathAtPath("Assets/Player.inputactions", records[0].subAssetKey).string();
    runtimeDatabase = E::Assets::RuntimeAssetDatabase(std::move(next.manifest));
    auto reloaded = E::Assets::LoadInputActionsAsset(identifier, error); ASSERT_TRUE(reloaded) << error; EXPECT_EQ(reloaded.get(), asset.get());
    reloaded->ApplyTo(*first); first->DispatchDynamicEvents(); EXPECT_EQ(canceled, 1);
    EXPECT_FALSE(first->GetActionState(oldHandle).actuated); EXPECT_TRUE(first->GetUnappliedOverrides().empty());
    system.EndFrame(); event.type = Input::InputEventType::ButtonUp; ASSERT_TRUE(system.SubmitEvent(event)); first->Update(system.Now());
    system.EndFrame(); event.type = Input::InputEventType::ButtonDown; ASSERT_TRUE(system.SubmitEvent(event)); first->Update(system.Now()); EXPECT_EQ(performed, 2);
}

TEST_F(InputAssetFixture, PackagedInputLoadsWithoutLooseArtifactOrSourceAsset)
{
    Save(); Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/Player.inputactions"));
    const auto records = database.LoadAllAssetsAtPath("Assets/Player.inputactions"); ASSERT_EQ(records.size(), 1u);
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Player.inputactions"); ASSERT_TRUE(manifest);
    E::Assets::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest);
    auto built = builder.Build({{records[0].assetId, records[0].subAssetKey}}, "win64-dx12"); ASSERT_EQ(built.manifest.entries.size(), 1u);
    std::ifstream artifact(database.ResolveArtifactPathAtPath("Assets/Player.inputactions", records[0].subAssetKey), std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(artifact)), {}); ASSERT_FALSE(bytes.empty());
    const std::string logicalPath = "Library/Artifacts/Input.native";
    A::NpakWriter writer; std::string error; ASSERT_TRUE(writer.AddFile(logicalPath, bytes, &error)) << error;
    ASSERT_TRUE(writer.Write(root / "input.npak", {}, &error)) << error;
    const auto install = root / "Install"; ASSERT_FALSE(std::filesystem::exists(install / logicalPath));
    built.manifest.entries[0].artifactPath = logicalPath;
    E::Assets::RuntimeAssetDatabase runtimeDatabase(std::move(built.manifest));
    auto* previous = Core::ServiceLocator::Contains<E::Assets::RuntimeAssetDatabase>() ? &Core::ServiceLocator::Get<E::Assets::RuntimeAssetDatabase>() : nullptr;
    const auto assets = Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
    const auto engine = Core::ResourceManagement::ShaderManager::EngineAssetsRoot();
    struct Restore
    {
        E::Assets::RuntimeAssetDatabase* previous; std::string assets, engine;
        ~Restore()
        {
            A::ClearContentMounts(); Core::ResourceManagement::ShaderManager::ProvideAssetPaths(assets, engine);
            if (previous) Core::ServiceLocator::Provide(*previous); else Core::ServiceLocator::Remove<E::Assets::RuntimeAssetDatabase>();
        }
    } restore{previous, assets, engine};
    ASSERT_FALSE(A::HasContentMounts());
    ASSERT_TRUE(A::MountContentPackage(root / "input.npak", install, &error)) << error;
    Core::ResourceManagement::ShaderManager::ProvideAssetPaths((install / "Assets").string(), engine);
    Core::ServiceLocator::Provide(runtimeDatabase);
    const auto identifier = S::ObjectIdentifier::Asset(S::AssetId(records[0].assetId.GetGuid()),
        S::MakeLocalIdentifierInFile(records[0].assetId.GetGuid(), records[0].subAssetKey), "Assets/Player.inputactions");
    auto loaded = E::Assets::LoadInputActionsAsset(identifier, error); ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->GetDefinition()->GetDefinition().actions[0].id, definition.actions[0].id);
    EXPECT_FALSE(std::filesystem::exists(install / "Assets/Player.inputactions"));
    EXPECT_FALSE(std::filesystem::exists(install / logicalPath));
}

TEST_F(InputAssetFixture, PlayerComponentReferenceSurvivesTwoPlaySandboxes)
{
    Save(); Editor::Assets::AssetDatabaseFacade database({root}); ASSERT_TRUE(database.Refresh()); ASSERT_TRUE(database.ImportAsset("Assets/Player.inputactions"));
    const auto records = database.LoadAllAssetsAtPath("Assets/Player.inputactions"); ASSERT_EQ(records.size(), 1u);
    auto manifest = database.GetArtifactManifestForAssetPath("Assets/Player.inputactions"); ASSERT_TRUE(manifest);
    E::Assets::RuntimeManifestBuilder builder; builder.AddArtifactManifest(*manifest);
    auto built = builder.Build({{records[0].assetId, records[0].subAssetKey}}, "editor"); ASSERT_EQ(built.manifest.entries.size(), 1u);
    built.manifest.entries[0].artifactPath = database.ResolveArtifactPathAtPath("Assets/Player.inputactions", records[0].subAssetKey).string();
    E::Assets::RuntimeAssetDatabase runtimeDatabase(built.manifest);
    auto* previous = Core::ServiceLocator::Contains<E::Assets::RuntimeAssetDatabase>() ? &Core::ServiceLocator::Get<E::Assets::RuntimeAssetDatabase>() : nullptr;
    Core::ServiceLocator::Provide(runtimeDatabase);
    struct Restore { E::Assets::RuntimeAssetDatabase* old; ~Restore() { if (old) Core::ServiceLocator::Provide(*old); else Core::ServiceLocator::Remove<E::Assets::RuntimeAssetDatabase>(); } } restore{previous};
    auto id = S::ObjectIdentifier::Asset(S::AssetId(records[0].assetId.GetGuid()), S::MakeLocalIdentifierInFile(records[0].assetId.GetGuid(), records[0].subAssetKey), "Assets/Player.inputactions");
    std::string error; auto asset = E::Assets::LoadInputActionsAsset(id, error); ASSERT_TRUE(asset) << error;
    E::SceneSystem::SceneManager scenes(""); scenes.LoadEmptyScene();
    auto* authored = scenes.GetCurrentScene()->CreateGameObject("Player").AddComponent<E::Components::PlayerInputComponent>();
    authored->SetActions(S::PPtr<E::Assets::InputActionsAsset>(asset.get()));
    NLS::Scripting::ScriptRuntime scripts; ASSERT_TRUE(scripts.Initialize({}).Succeeded()); Input::InputSystem system;
    uint64_t previousUser = 0;
    for (uint64_t sessionId = 800; sessionId < 802; ++sessionId)
    {
        ASSERT_TRUE(scenes.BeginPlaySandbox()); NLS::Scripting::RuntimePlaySession session; session.ConfigureInput(system);
        ASSERT_TRUE(session.Begin(scripts, scenes, sessionId).Succeeded()); scenes.GetCurrentScene()->Play();
        auto* component = scenes.GetCurrentScene()->GetGameObjects().front()->GetComponent<E::Components::PlayerInputComponent>(); ASSERT_NE(component, nullptr);
        auto user = component->GetInputUser(); ASSERT_TRUE(user) << component->GetLastError(); EXPECT_NE(user->GetId(), previousUser); previousUser = user->GetId();
        EXPECT_EQ(user->GetDefinition()->GetDefinition().defaultContext, definition.defaultContext);
        Input::InputEvent event; event.device = system.GetKeyboard(); event.control = 32; event.type = Input::InputEventType::ButtonDown;
        ASSERT_TRUE(system.SubmitEvent(event)); session.NotifyFocus(true); session.Tick(.02f); EXPECT_TRUE(user->WasActionPressed(user->FindAction(Id("integration-jump"))));
        if (sessionId == 800)
        {
            const auto oldDefinition = user->GetDefinition(); int canceled = 0;
            auto subscription = user->BindAction(Id("integration-jump"), Input::ActionPhase::Canceled, [&](const auto&) { ++canceled; });
            definition.actions[0].name = "Jump Reloaded"; Save(); ASSERT_TRUE(database.ImportAsset("Assets/Player.inputactions"));
            auto manifest = database.GetArtifactManifestForAssetPath("Assets/Player.inputactions"); ASSERT_TRUE(manifest);
            E::Assets::RuntimeManifestBuilder updated; updated.AddArtifactManifest(*manifest);
            auto next = updated.Build({{records[0].assetId, records[0].subAssetKey}}, "editor"); ASSERT_EQ(next.manifest.entries.size(), 1u);
            next.manifest.entries[0].artifactPath = database.ResolveArtifactPathAtPath("Assets/Player.inputactions", records[0].subAssetKey).string();
            runtimeDatabase = E::Assets::RuntimeAssetDatabase(std::move(next.manifest));
            component->OnUpdate(0); EXPECT_EQ(user->GetDefinition(), oldDefinition);
            system.EndFrame(); session.NotifyFocus(true); session.Tick(.001f);
            EXPECT_NE(user->GetDefinition(), oldDefinition); EXPECT_EQ(canceled, 1);
            EXPECT_EQ(user->GetDefinition()->GetDefinition().actions[0].name, "Jump Reloaded");
        }
        session.End(); EXPECT_TRUE(user->IsSuspended()); ASSERT_TRUE(scenes.DiscardPlaySandbox());
        EXPECT_EQ(authored->GetActions().Get(), asset.get());
        event.type = Input::InputEventType::DeviceReset; ASSERT_TRUE(system.SubmitEvent(event)); system.EndFrame();
    }
}

TEST(InspectorInputCommands, MenuAndActionShareTargetValidationAfterComponentDestruction)
{
    using namespace NLS; using namespace NLS::Editor::Shortcuts;
    Input::InputSystem input; EditorShortcutService service; Editor::Inspector::InspectorView view(false);
    int32_t componentId = 0; int calls = 0, fallback = 0;
    service.RegisterContext({"inspector", "Inspector", 50, "focused-panel", [] { return true; }});
    Tests::EditorInputMappingBuilder mappings;
    Tests::TestShortcutCommand command; command.id = "inspector.component-copy"; command.context = "inspector";
    command.defaultBinding = ShortcutBinding::FromKey(Windowing::Inputs::EKey::KEY_C, EShortcutModifier::Ctrl);
    command.canExecute = [&](const InvocationContext&) { return view.CanExecuteComponentCommand("copy", componentId); };
    command.invoke = [&](const InvocationContext&) { if (!view.ExecuteComponentCommand("copy", componentId)) return CommandResult::TargetExpired; ++calls; return CommandResult::Completed; };
    mappings.Add(command); ASSERT_TRUE(service.RegisterCommand(command));
    command.id = "global.copy"; command.context = ShortcutContexts::Global; command.canExecute = {}; command.invoke = [&](const InvocationContext&) { ++fallback; return CommandResult::Completed; };
    mappings.Add(command); ASSERT_TRUE(service.RegisterCommand(command)); service.InitializeInput(input); mappings.Apply(service);
    const auto press = [&]
    {
        Input::InputEvent event; event.device = input.GetKeyboard(); event.control = 341; event.type = Input::InputEventType::ButtonDown; EXPECT_TRUE(input.SubmitEvent(event));
        event.control = 67; EXPECT_TRUE(input.SubmitEvent(event)); event.type = Input::InputEventType::ButtonUp; EXPECT_TRUE(input.SubmitEvent(event));
        service.UpdateInput({}); input.EndFrame();
    };
    {
        Engine::SceneSystem::Scene scene; auto* camera = scene.CreateGameObject("Camera").AddComponent<Engine::Components::CameraComponent>(); componentId = camera->GetInstanceID();
        InvocationContext menu; menu.source = CommandSource::Menu;
        EXPECT_EQ(service.TryExecuteCommand("inspector.component-copy", menu), CommandResult::Completed);
        press(); EXPECT_EQ(calls, 2); EXPECT_EQ(fallback, 0);
    }
    EXPECT_FALSE(view.CanExecuteComponentCommand("copy", componentId)); press(); EXPECT_EQ(calls, 2); EXPECT_EQ(fallback, 0);
}
