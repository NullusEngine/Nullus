#include <UI/Widgets/Menu/MenuList.h>
#include <UI/Widgets/Menu/MenuItem.h>

#include <Debug/Logger.h>
#include <Object/Object.h>
#include <Reflection/Type.h>
#include <SceneSystem/Scene.h>
#include <SceneSystem/SceneManager.h>
#include <ServiceLocator.h>

#include <utility>

#include "Core/EditorActions.h"
#include "Core/Context.h"
#include "Panels/HierarchyUndo.h"
#include "Undo/EditorUndoService.h"
#include "Utils/GameObjectCreationMenu.h"
#include "GameObject.h"
#include "Components/LightComponent.h"
#include "Components/CameraComponent.h"
#include "Components/MeshFilter.h"
#include "Engine/PrimitiveType.h"
using namespace NLS;

namespace
{
NLS::Editor::Undo::UndoScopeId BuildCreationUndoScope(const NLS::Engine::SceneSystem::Scene& scene)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
        return {};
    return NLS::Core::ServiceLocator::Get<NLS::Editor::Core::EditorActions>().GetUndoScopeForScene(scene);
}

NLS::Engine::GameObject* ResolveCreationParent(const InstanceID parentId)
{
    return parentId == InstanceID_None
        ? nullptr
        : NLS::Object::IDToPointer<NLS::Engine::GameObject>(parentId);
}

NLS::Engine::SceneSystem::Scene* ResolveCreationScene(NLS::Engine::GameObject* parent)
{
    if (parent != nullptr)
        return parent->GetScene();

    const auto& context = NLS::Core::ServiceLocator::Get<NLS::Editor::Core::EditorActions>().GetContext();
    if (context.activePrefabStage.has_value() && context.activePrefabStage->stageScene)
        return context.activePrefabStage->stageScene.get();
    return context.sceneManager.GetCurrentScene();
}

template<typename TFactory>
void ExecuteCreationThroughUndo(const InstanceID parentId, TFactory&& factory)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>() ||
        !NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        NLS_LOG_WARNING("GameObject creation was rejected because the global Undo service is unavailable.");
        return;
    }

    auto* parent = ResolveCreationParent(parentId);
    if (parentId != InstanceID_None && parent == nullptr)
    {
        NLS_LOG_WARNING("GameObject creation was rejected because its parent no longer exists.");
        return;
    }

    auto* scene = ResolveCreationScene(parent);
    if (scene == nullptr)
    {
        NLS_LOG_WARNING("GameObject creation was rejected because there is no editable Scene.");
        return;
    }

    auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
    const auto result = factory(undoService, *scene, parent, BuildCreationUndoScope(*scene));
    if (!result.Succeeded())
    {
        NLS_LOG_WARNING(
            "GameObject creation was not applied through Undo: " +
            (result.message.empty() ? std::string("Undo service rejected the command.") : result.message));
    }
}

template<class T>
void CreateGameObjectWithComponentThroughUndo(const InstanceID parentId)
{
    ExecuteCreationThroughUndo(parentId, [](
        NLS::Editor::Undo::EditorUndoService& undoService,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Engine::GameObject* parent,
        NLS::Editor::Undo::UndoScopeId scope)
    {
        return NLS::Editor::Panels::HierarchyUndo::CreateGameObjectWithComponent(
            undoService,
            scene,
            parent,
            NLS_TYPEOF(T).GetName(),
            std::move(scope));
    });
}

void CreatePrimitiveThroughUndo(
    const InstanceID parentId,
    const NLS::Engine::PrimitiveType primitiveType)
{
    ExecuteCreationThroughUndo(parentId, [primitiveType](
        NLS::Editor::Undo::EditorUndoService& undoService,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Engine::GameObject* parent,
        NLS::Editor::Undo::UndoScopeId scope)
    {
        return NLS::Editor::Panels::HierarchyUndo::CreatePrimitiveGameObject(
            undoService,
            scene,
            parent,
            primitiveType,
            std::move(scope));
    });
}

void CreateEmptyThroughUndo(const InstanceID parentId)
{
    ExecuteCreationThroughUndo(parentId, [](
        NLS::Editor::Undo::EditorUndoService& undoService,
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Engine::GameObject* parent,
        NLS::Editor::Undo::UndoScopeId scope)
    {
        return NLS::Editor::Panels::HierarchyUndo::CreateEmptyGameObject(
            undoService,
            scene,
            parent,
            std::move(scope));
    });
}
} // namespace

std::function<void()> Combine(std::function<void()> p_a, std::optional<std::function<void()>> p_b)
{
    if (p_b.has_value())
    {
        return [=]()
        {
            p_a();
            p_b.value()();
        };
    }

    return p_a;
}

template<class T>
std::function<void()> GameObjectWithComponentCreationHandler(Engine::GameObject* p_parent, std::optional<std::function<void()>> p_onItemClicked)
{
    const auto parentId = p_parent != nullptr ? p_parent->GetInstanceID() : InstanceID_None;
    return Combine([parentId] { CreateGameObjectWithComponentThroughUndo<T>(parentId); }, p_onItemClicked);
}

std::function<void()> PrimitiveCreationHandler(Engine::GameObject* p_parent, Engine::PrimitiveType p_type, std::optional<std::function<void()>> p_onItemClicked)
{
    const auto parentId = p_parent != nullptr ? p_parent->GetInstanceID() : InstanceID_None;
    return Combine([parentId, p_type] { CreatePrimitiveThroughUndo(parentId, p_type); }, p_onItemClicked);
}

void Editor::Utils::GameObjectCreationMenu::GenerateGameObjectCreationMenu(UI::Widgets::MenuList& p_menuList, Engine::GameObject* p_parent, std::optional<std::function<void()>> p_onItemClicked)
{
    using namespace NLS::UI;
    using namespace NLS::Engine::Components;

    const auto parentId = p_parent != nullptr ? p_parent->GetInstanceID() : InstanceID_None;
    p_menuList.CreateWidget<Widgets::MenuItem>("Create Empty").ClickedEvent +=
        Combine([parentId] { CreateEmptyThroughUndo(parentId); }, p_onItemClicked);

    auto& primitives = p_menuList.CreateWidget<Widgets::MenuList>("Primitives");
    auto& physicals = p_menuList.CreateWidget<Widgets::MenuList>("Physicals");
    auto& lights = p_menuList.CreateWidget<Widgets::MenuList>("Lights");
    auto& audio = p_menuList.CreateWidget<Widgets::MenuList>("Audio");
    auto& others = p_menuList.CreateWidget<Widgets::MenuList>("Others");

    primitives.CreateWidget<Widgets::MenuItem>("Cube").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Cube, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Sphere").ClickedEvent            += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Sphere, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Cone").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Cone, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Cylinder").ClickedEvent          += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Cylinder, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Plane").ClickedEvent             += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Plane, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Gear").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Gear, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Helix").ClickedEvent             += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Helix, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Pipe").ClickedEvent              += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Pipe, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Pyramid").ClickedEvent           += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Pyramid, p_onItemClicked);
    primitives.CreateWidget<Widgets::MenuItem>("Torus").ClickedEvent             += PrimitiveCreationHandler(p_parent, Engine::PrimitiveType::Torus, p_onItemClicked);
    lights.CreateWidget<Widgets::MenuItem>("Light").ClickedEvent                 += GameObjectWithComponentCreationHandler<LightComponent>(p_parent, p_onItemClicked);
    others.CreateWidget<Widgets::MenuItem>("Camera").ClickedEvent                += GameObjectWithComponentCreationHandler<CameraComponent>(p_parent, p_onItemClicked);
}
