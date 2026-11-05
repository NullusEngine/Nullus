#include "Panels/Hierarchy.h"
#include "Shortcuts/EditorCommandInvocation.h"
namespace Shortcuts = NLS::Editor::Shortcuts;
#include "ImGui/imgui_internal.h"
#include "Panels/SceneView.h"

#include "Inspector/EditorSelectionService.h"
#include "Core/EditorActions.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "Profiling/Profiler.h"
#include "Panels/HierarchyUndo.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetPathUtils.h"
#include "Assets/ScriptAssetUtility.h"
#include <Scripting/ScriptComponent.h>

#include <Math/Color.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Plugins/DDSource.h>
#include <UI/Plugins/DDTarget.h>

#include <Debug/Logger.h>

#include <ServiceLocator.h>
#include <SceneSystem/Scene.h>
#include <Utils/PathParser.h>

#include <UI/Plugins/ContextualMenu.h>

#include "Utils/GameObjectCreationMenu.h"
#include "UI/Widgets/InputFields/InputText.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <algorithm>
#include <ImGui/imgui.h>
#include "Inspector/NativeReflectionAdapter.h"
#include "Core/SceneSelection.h"

using namespace NLS;

namespace
{
constexpr size_t kHierarchyImportedPrefabPreloadGateCapacity = 256u;
constexpr auto kHierarchyImportedPrefabPreloadGateTtl = std::chrono::seconds(3);

std::mutex& HierarchyImportedPrefabPreloadMutex()
{
	static std::mutex mutex;
	return mutex;
}

NLS::Editor::Core::RecentBackgroundWorkGate& HierarchyImportedPrefabPreloadGate()
{
	static NLS::Editor::Core::RecentBackgroundWorkGate gate(
		kHierarchyImportedPrefabPreloadGateCapacity,
		kHierarchyImportedPrefabPreloadGateTtl);
	return gate;
}

std::string BuildHierarchyImportedPrefabPreloadKey(
	const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
	return NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload) + "|" +
		NLS::Editor::Assets::GetEditorAssetDragPayloadGuid(payload) + "|" +
		NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
}

bool IsHierarchyImportedPrefabPreloadInFlight(
	const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
	const auto key = BuildHierarchyImportedPrefabPreloadKey(payload);
	if (key.empty())
		return false;
	std::lock_guard lock(HierarchyImportedPrefabPreloadMutex());
	return HierarchyImportedPrefabPreloadGate().IsInFlight(key);
}

bool ScheduleHierarchyImportedPrefabPreloadOnce(
	const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
	const auto key = BuildHierarchyImportedPrefabPreloadKey(payload);
	if (key.empty())
		return false;
	{
		std::lock_guard lock(HierarchyImportedPrefabPreloadMutex());
		if (!HierarchyImportedPrefabPreloadGate().TryBegin(
				key,
				NLS::Editor::Core::RecentBackgroundWorkGate::Clock::now()))
			return false;
	}

	const auto projectAssetsPath = std::filesystem::path(EDITOR_CONTEXT(projectAssetsPath));
	const bool scheduled = EDITOR_EXEC(TrackOpportunisticBackgroundTask(
		[payload, projectAssetsPath, key]
		{
			auto completion = HierarchyImportedPrefabPreloadGate().CompleteOnScopeExit(key);
			try
			{
				NLS::Editor::Assets::EditorAssetDragDropBridge bridge(projectAssetsPath);
				(void)bridge.PreloadImportedAssetHandlePrefabHotCache(payload);
			}
			catch (const std::exception& exception)
			{
				NLS_LOG_WARNING(std::string("Imported prefab hot-cache preload failed: ") + exception.what());
			}
			catch (...)
			{
				NLS_LOG_WARNING("Imported prefab hot-cache preload failed with an unknown exception.");
			}
		}));
	if (!scheduled)
	{
		HierarchyImportedPrefabPreloadGate().End(key);
	}
	return scheduled;
}

NLS::Editor::Undo::UndoScopeId BuildHierarchyUndoScope(const Engine::SceneSystem::Scene& scene)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
        return {};
    return NLS::Core::ServiceLocator::Get<NLS::Editor::Core::EditorActions>().GetUndoScopeForScene(scene);
}

NLS::Editor::Undo::UndoScopeId BuildHierarchyUndoScope(const Engine::GameObject& gameObject)
{
    if (const auto* scene = gameObject.GetScene())
        return BuildHierarchyUndoScope(*scene);
    return {};
}

void ReportHierarchyUndoFailure(const NLS::Editor::Undo::UndoRequestResult& result)
{
    if (!result.Succeeded())
    {
        NLS_LOG_WARNING(
            "Hierarchy change was not applied through Undo: " +
            (result.message.empty() ? std::string("Undo service rejected the command.") : result.message));
    }
}

void RenameGameObjectThroughUndo(Engine::GameObject& gameObject, std::string newName)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
    {
        NLS_LOG_WARNING("Hierarchy rename was rejected because the global Undo service is unavailable.");
        return;
    }

    auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
    ReportHierarchyUndoFailure(NLS::Editor::Panels::HierarchyUndo::RenameGameObject(
        undoService,
        gameObject,
        std::move(newName),
        BuildHierarchyUndoScope(gameObject)));
}

struct HierarchyDragSelection
{
    NLS::Editor::Inspector::InspectorTargetHandle source;
    NLS::Editor::Inspector::InspectorTargetSet selection;
    NLS::Editor::Undo::UndoScopeId scope;
};
HierarchyDragSelection& GetHierarchyDragSelection()
{
    static HierarchyDragSelection snapshot;
    return snapshot;
}

std::vector<Engine::GameObject*> GetHierarchyCommandSelection(Engine::GameObject& object)
{
    if (EDITOR_EXEC(IsGameObjectSelected(object))) return EDITOR_EXEC(GetSelectedGameObjects());
    EDITOR_EXEC(SelectGameObject(object));
    return {&object};
}

void ReparentGameObjectThroughUndo(Engine::GameObject& gameObject, Engine::GameObject* newParent)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
    {
        NLS_LOG_WARNING("Hierarchy reparent was rejected because the global Undo service is unavailable.");
        return;
    }

    auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
    std::vector<Engine::GameObject*> objects;
    const auto& drag = GetHierarchyDragSelection();
    if (NLS::Editor::Inspector::NativeReflectionAdapter::ResolveObject(drag.source) == &gameObject &&
        drag.scope == BuildHierarchyUndoScope(gameObject))
    {
        for (const auto& target : drag.selection.targets)
            if (auto* object = dynamic_cast<Engine::GameObject*>(NLS::Editor::Inspector::NativeReflectionAdapter::ResolveObject(target)))
                objects.push_back(object);
    }
    else objects = GetHierarchyCommandSelection(gameObject);
    ReportHierarchyUndoFailure(NLS::Editor::Panels::HierarchyUndo::ReparentGameObjects(
        undoService, objects, newParent, BuildHierarchyUndoScope(gameObject)));
}

void DuplicateGameObjectThroughUndo(Engine::GameObject& gameObject)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
    {
        NLS_LOG_WARNING("Hierarchy duplicate was rejected because the global Undo service is unavailable.");
        return;
    }

    auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
    const auto objects = GetHierarchyCommandSelection(gameObject);
    ReportHierarchyUndoFailure(NLS::Editor::Panels::HierarchyUndo::DuplicateGameObjects(
        undoService, objects, BuildHierarchyUndoScope(gameObject)));
}

void DeleteGameObjectThroughUndo(Engine::GameObject& gameObject)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
    {
        NLS_LOG_WARNING("Hierarchy delete was rejected because the global Undo service is unavailable.");
        return;
    }

    auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
    const auto objects = GetHierarchyCommandSelection(gameObject);
    ReportHierarchyUndoFailure(NLS::Editor::Panels::HierarchyUndo::DeleteGameObjects(
        undoService, objects, BuildHierarchyUndoScope(gameObject)));
}

void DropAssetIntoHierarchy(
	const NLS::Editor::Assets::EditorAssetDragPayload& payload,
	Engine::GameObject* parent)
{
	auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene();
	if (scene == nullptr)
	{
		NLS_LOG_WARNING("Skipped prefab drop because there is no active scene.");
		return;
	}

	if (NLS::Editor::Assets::IsEditorAssetDragPayloadScript(payload))
	{
		if (parent == nullptr)
			return;
		const auto asset = NLS::Editor::Assets::LoadScriptAsset(
			std::filesystem::path(EDITOR_CONTEXT(projectAssetsPath)), payload);
		if (!asset.has_value())
		{
			NLS_LOG_WARNING("Skipped script drop because the ScriptAsset could not be loaded: " +
				NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload));
			return;
		}
		if (!asset->isComponent)
		{
			NLS_LOG_WARNING("Skipped script drop because the script is not a Behaviour component: " +
				NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload));
			return;
		}
		if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
		{
			NLS_LOG_WARNING("Skipped script drop because the global Undo service is unavailable.");
			return;
		}
		auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
		ReportHierarchyUndoFailure(NLS::Editor::Panels::HierarchyUndo::AddScriptComponent(
			undoService,
			*parent,
			*asset,
			BuildHierarchyUndoScope(*parent)));
		return;
	}

	(void)ScheduleHierarchyImportedPrefabPreloadOnce(payload);
	if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
	{
		NLS_LOG_WARNING("Skipped asset drop because the global Undo service is unavailable.");
		return;
	}
	auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
	ReportHierarchyUndoFailure(NLS::Editor::Panels::HierarchyUndo::CreateGameObjectFromAsset(
		undoService,
		payload,
		*scene,
		parent,
		BuildHierarchyUndoScope(*scene)));
}

void DropModelFileIntoHierarchy(
	const std::pair<std::string, UI::Widgets::Group*>& data,
	Engine::GameObject* parent)
{
	const auto& path = data.first;
	const auto fileType = Utils::PathParser::GetFileType(path);
	if (fileType != Utils::PathParser::EFileType::MODEL &&
		fileType != Utils::PathParser::EFileType::PREFAB)
	{
		return;
	}

	auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene();
	if (scene == nullptr)
	{
		NLS_LOG_WARNING("Skipped model drop because there is no active scene.");
		return;
	}
	if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
	{
		NLS_LOG_WARNING("Skipped model drop because the global Undo service is unavailable.");
		return;
	}
	auto& undoService = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
	ReportHierarchyUndoFailure(NLS::Editor::Panels::HierarchyUndo::CreateGameObjectFromAsset(
		undoService,
		path,
		*scene,
		parent,
		BuildHierarchyUndoScope(*scene)));
}
}

class GameObjectContextualMenu : public UI::ContextualMenu
{
public:
    GameObjectContextualMenu(Engine::GameObject* p_target, UI::Widgets::TreeNode& p_treeNode, bool p_panelMenu = false) :
        m_target(p_target),
        m_treeNode(p_treeNode)
    {
        using namespace UI::Widgets;
        using namespace UI::Widgets::Menu;
        using namespace Engine::Components;

        if (m_target)
        {
            auto& focusButton = CreateWidget<UI::Widgets::MenuItem>("Focus");
            focusButton.ClickedEvent += [target = m_target->GetInstanceID()]
            {
                Shortcuts::InvocationContext invocation; invocation.source = Shortcuts::CommandSource::Menu; invocation.targetInstance = target;
                Shortcuts::InvokeEditorCommand("edit.frame-selection-hierarchy", invocation);
            };

            auto& visibilityMenu = CreateWidget<UI::Widgets::MenuList>("Scene Visibility");
            visibilityMenu.CreateWidget<UI::Widgets::MenuItem>("Hide (with children)").ClickedEvent += [this]
            { EDITOR_PANEL(Editor::Panels::SceneView, "Scene View").SetObjectVisibility(*m_target, false); };
            visibilityMenu.CreateWidget<UI::Widgets::MenuItem>("Show (with children)").ClickedEvent += [this]
            { EDITOR_PANEL(Editor::Panels::SceneView, "Scene View").SetObjectVisibility(*m_target, true); };
            visibilityMenu.CreateWidget<UI::Widgets::MenuItem>("Hide this object").ClickedEvent += [this]
            { EDITOR_PANEL(Editor::Panels::SceneView, "Scene View").SetObjectVisibility(*m_target, false, false); };
            visibilityMenu.CreateWidget<UI::Widgets::MenuItem>("Show this object").ClickedEvent += [this]
            { EDITOR_PANEL(Editor::Panels::SceneView, "Scene View").SetObjectVisibility(*m_target, true, false); };

            auto& duplicateButton = CreateWidget<UI::Widgets::MenuItem>("Duplicate");
            duplicateButton.ClickedEvent += [target = m_target->GetInstanceID()]
            {
                Shortcuts::InvocationContext invocation; invocation.source = Shortcuts::CommandSource::Menu; invocation.targetInstance = target;
                Shortcuts::InvokeEditorCommand("edit.duplicate-selection-hierarchy", invocation);
            };

            auto& deleteButton = CreateWidget<UI::Widgets::MenuItem>("Delete");
            deleteButton.ClickedEvent += [target = m_target->GetInstanceID()]
            {
                Shortcuts::InvocationContext invocation; invocation.source = Shortcuts::CommandSource::Menu; invocation.targetInstance = target;
                Shortcuts::InvokeEditorCommand("edit.delete-selected-gameobject-hierarchy", invocation);
            };

			auto& renameMenu = CreateWidget<UI::Widgets::MenuList>("Rename to...");

			auto& nameEditor = renameMenu.CreateWidget<NLS::UI::Widgets::InputText>("");
			nameEditor.selectAllOnClick = true;

			renameMenu.ClickedEvent += [this, &nameEditor]
			{
				nameEditor.content = m_target->GetName();
			};

			nameEditor.EnterPressedEvent += [this](std::string p_newName)
			{
				RenameGameObjectThroughUndo(*m_target, std::move(p_newName));
			};
        }

		auto& createGameObject = CreateWidget<UI::Widgets::MenuList>("Create...");
        Editor::Utils::GameObjectCreationMenu::GenerateGameObjectCreationMenu(createGameObject, m_target, std::bind(&UI::Widgets::TreeNode::Open, &m_treeNode));
	}

	virtual void Execute() override
	{
		if (m_widgets.size() > 0)
			UI::ContextualMenu::Execute();
	}

private:
	Engine::GameObject* m_target;
	UI::Widgets::TreeNode& m_treeNode;
};

void ExpandTreeNode(UI::Widgets::TreeNode& p_toExpand, const UI::Widgets::TreeNode* p_root)
{
	p_toExpand.Open();

	if (&p_toExpand != p_root && p_toExpand.HasParent())
	{
		ExpandTreeNode(*static_cast<UI::Widgets::TreeNode*>(p_toExpand.GetParent()), p_root);
	}
}

std::vector<UI::Widgets::TreeNode*> nodesToCollapse;
std::vector<UI::Widgets::TreeNode*> founds;

std::string GetCurrentHierarchySceneName()
{
	if (EDITOR_CONTEXT(activePrefabStage).has_value())
	{
		const auto& stage = *EDITOR_CONTEXT(activePrefabStage);
		std::string name = stage.prefabAssetPath.empty()
			? stage.prefabSubAssetKey
			: Utils::PathParser::GetElementName(stage.prefabAssetPath);
		if (name.empty())
			name = "Prefab";
		return stage.dirty ? name + "*" : name;
	}

	const auto path = EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath();
	std::string sceneName = path.empty() ? "Untitled Scene" : Utils::PathParser::GetElementName(path);
	if (EDITOR_CONTEXT(sceneManager).HasUnsavedSceneChanges())
		sceneName += "*";
	return sceneName;
}

NLS::Maths::Color PrefabColorForToken(NLS::Editor::Assets::PrefabHierarchyColorToken token)
{
	switch (token)
	{
	case NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot:
		return {0.36f, 0.62f, 0.96f, 1.0f};
	case NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedChild:
		return {0.58f, 0.75f, 0.98f, 1.0f};
	case NLS::Editor::Assets::PrefabHierarchyColorToken::Override:
		return {0.36f, 0.62f, 0.96f, 1.0f};
	case NLS::Editor::Assets::PrefabHierarchyColorToken::Missing:
		return {0.95f, 0.34f, 0.34f, 1.0f};
	case NLS::Editor::Assets::PrefabHierarchyColorToken::GeneratedReadOnly:
		return {0.54f, 0.82f, 0.78f, 1.0f};
	case NLS::Editor::Assets::PrefabHierarchyColorToken::Pending:
		return {0.68f, 0.67f, 0.86f, 1.0f};
	case NLS::Editor::Assets::PrefabHierarchyColorToken::Unpacked:
		return {0.72f, 0.72f, 0.72f, 1.0f};
	case NLS::Editor::Assets::PrefabHierarchyColorToken::Default:
	default:
		return {1.0f, 1.0f, 1.0f, 1.0f};
	}
}

void ExpandTreeNodeAndEnable(UI::Widgets::TreeNode& p_toExpand, const UI::Widgets::TreeNode* p_root)
{
	if (!p_toExpand.IsOpened())
	{
		p_toExpand.Open();
		nodesToCollapse.push_back(&p_toExpand);
	}

	p_toExpand.enabled = true;

	if (&p_toExpand != p_root && p_toExpand.HasParent())
	{
		ExpandTreeNodeAndEnable(*static_cast<UI::Widgets::TreeNode*>(p_toExpand.GetParent()), p_root);
	}
}

Editor::Panels::Hierarchy::Hierarchy
(
	const std::string & p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings)
{
	m_prefabInstanceRegistry = &EDITOR_CONTEXT(prefabInstanceRegistry);
	auto& searchBar = CreateWidget<UI::Widgets::InputText>();
	searchBar.ContentChangedEvent += [this](const std::string& p_content)
	{
		founds.clear();
		auto content = p_content;
		std::transform(content.begin(), content.end(), content.begin(), ::tolower);

		for (auto& [actor, item] : m_widgetGameObjectLink)
		{
			if (!p_content.empty())
			{
				auto itemName = item->name;
				std::transform(itemName.begin(), itemName.end(), itemName.begin(), ::tolower);

				if (itemName.find(content) != std::string::npos)
				{
					founds.push_back(item);
				}

				item->enabled = false;
			}
			else
			{
				item->enabled = true;
			}
		}

		for (auto node : founds)
		{
			node->enabled = true;

			if (node->HasParent())
			{
				ExpandTreeNodeAndEnable(*static_cast<UI::Widgets::TreeNode*>(node->GetParent()), m_sceneRoot);
			}
		}

		if (p_content.empty())
		{
			for (auto node : nodesToCollapse)
			{
				node->Close();
			}

			nodesToCollapse.clear();
		}
	};

    CreateWidget<UI::Widgets::Spacing>(1);

	m_sceneRoot = &CreateWidget<UI::Widgets::TreeNode>("Scene", true);
	static_cast<UI::Widgets::TreeNode*>(m_sceneRoot)->Open();
	RefreshSceneRootName();
	m_sceneRoot->AddPlugin<UI::DDTarget<std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>>>("GameObject").DataReceivedEvent += [this](std::pair<Engine::GameObject*, UI::Widgets::TreeNode*> p_element)
	{
		if (p_element.first != nullptr)
			ReparentGameObjectThroughUndo(*p_element.first, nullptr);
	};
	m_sceneRoot->AddPlugin<UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
		NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent += [](NLS::Editor::Assets::EditorAssetDragPayload p_data)
	{
		DropAssetIntoHierarchy(p_data, nullptr);
	};
	m_sceneRoot->AddPlugin<UI::DDTarget<std::pair<std::string, UI::Widgets::Group*>>>("File").DataReceivedEvent += [](std::pair<std::string, UI::Widgets::Group*> p_data)
	{
		DropModelFileIntoHierarchy(p_data, nullptr);
	};
    m_sceneRoot->AddPlugin<GameObjectContextualMenu>(nullptr, *m_sceneRoot);

	m_selectionChangedListener = NLS::Editor::Inspector::EditorSelectionService::Get().SelectionChangedEvent +=
		[this](const NLS::Editor::Inspector::InspectorTargetSet& selection)
	{
		NLS_PROFILE_NAMED_SCOPE("Hierarchy::ApplyEditorSelection");
		UnselectGameObjectsWidgets();
        for (const auto& target : selection.targets)
        {
            if (target.kind != NLS::Editor::Inspector::InspectorTargetKind::GameObject) continue;
            auto* object = NLS::Object::IDToPointer<Engine::GameObject>(target.instanceId);
            const auto found = m_widgetGameObjectLink.find(object);
            if (found == m_widgetGameObjectLink.end() || !found->second) continue;
            auto* widget = found->second;
            widget->selected = true;
            widget->activeSelection = selection.Active() && target == *selection.Active();
            m_selectedWidgets.push_back(widget);
            if (widget->activeSelection)
            {
                m_selectedWidget = widget;
                if (widget->HasParent()) ExpandTreeNode(*static_cast<UI::Widgets::TreeNode*>(widget->GetParent()), m_sceneRoot);
            }
        }
	};
	m_sceneUnloadListener = EDITOR_CONTEXT(sceneManager).SceneUnloadEvent += std::bind(&Hierarchy::Clear, this);
	m_sceneLoadListener = EDITOR_CONTEXT(sceneManager).SceneLoadEvent += std::bind(&Hierarchy::RebuildFromCurrentScene, this);
	m_sceneSourcePathChangedListener = EDITOR_CONTEXT(sceneManager).CurrentSceneSourcePathChangedEvent += [this](const std::string&)
	{
		RefreshSceneRootName();
	};
	m_sceneDirtyStateChangedListener = EDITOR_CONTEXT(sceneManager).CurrentSceneDirtyStateChangedEvent += [this](bool)
	{
		RefreshSceneRootName();
	};
	m_gameObjectCreatedListener = Engine::GameObject::CreatedEvent += std::bind(&Hierarchy::AddGameObjectByInstance, this, std::placeholders::_1);
	m_gameObjectDestroyedListener = Engine::GameObject::DestroyedEvent += std::bind(&Hierarchy::DeleteGameObjectByInstance, this, std::placeholders::_1);
	m_gameObjectAttachedListener = Engine::GameObject::AttachEvent += std::bind(&Hierarchy::AttachGameObjectToParent, this, std::placeholders::_1);
	m_gameObjectDetachedListener = Engine::GameObject::DettachEvent += std::bind(&Hierarchy::DetachFromParent, this, std::placeholders::_1);
}

Editor::Panels::Hierarchy::~Hierarchy()
{
	NLS::Editor::Inspector::EditorSelectionService::Get().SelectionChangedEvent -= m_selectionChangedListener;
	EDITOR_CONTEXT(sceneManager).SceneUnloadEvent -= m_sceneUnloadListener;
	EDITOR_CONTEXT(sceneManager).SceneLoadEvent -= m_sceneLoadListener;
	EDITOR_CONTEXT(sceneManager).CurrentSceneSourcePathChangedEvent -= m_sceneSourcePathChangedListener;
	EDITOR_CONTEXT(sceneManager).CurrentSceneDirtyStateChangedEvent -= m_sceneDirtyStateChangedListener;
	Engine::GameObject::CreatedEvent -= m_gameObjectCreatedListener;
	Engine::GameObject::DestroyedEvent -= m_gameObjectDestroyedListener;
	Engine::GameObject::AttachEvent -= m_gameObjectAttachedListener;
	Engine::GameObject::DettachEvent -= m_gameObjectDetachedListener;
}

void Editor::Panels::Hierarchy::Clear()
{
	EDITOR_EXEC(UnselectGameObject());
    m_pendingSelectionClick.reset();
    m_rangeAnchor = NLS::InstanceID_None;

	m_sceneRoot->RemoveAllWidgets();
	m_selectedWidget = nullptr;
	m_widgetGameObjectLink.clear();
	RefreshSceneRootName();
}

void Editor::Panels::Hierarchy::UnselectGameObjectsWidgets()
{
    for (auto* widget : m_selectedWidgets)
        if (widget) { widget->selected = false; widget->activeSelection = false; }
    m_selectedWidgets.clear();
    m_selectedWidget = nullptr;
}

void Editor::Panels::Hierarchy::SelectGameObjectByInstance(Engine::GameObject& p_actor)
{
	if (auto result = m_widgetGameObjectLink.find(&p_actor); result != m_widgetGameObjectLink.end())
		if (result->second)
			SelectGameObjectByWidget(*result->second);
}

void Editor::Panels::Hierarchy::SelectGameObjectByWidget(UI::Widgets::TreeNode & p_widget)
{
	UnselectGameObjectsWidgets();

	p_widget.selected = true;
	p_widget.activeSelection = true;
	m_selectedWidgets.push_back(&p_widget);
	m_selectedWidget = &p_widget;

	if (p_widget.HasParent())
	{
		ExpandTreeNode(*static_cast<UI::Widgets::TreeNode*>(p_widget.GetParent()), m_sceneRoot);
	}
}

void Editor::Panels::Hierarchy::AttachGameObjectToParent(Engine::GameObject & p_actor)
{
	auto actorWidget = m_widgetGameObjectLink.find(&p_actor);

	if (actorWidget != m_widgetGameObjectLink.end())
	{
		auto widget = actorWidget->second;

		if (p_actor.HasParent())
		{
			auto parentWidget = m_widgetGameObjectLink.at(p_actor.GetParent());
			parentWidget->leaf = false;
			parentWidget->ConsiderWidget(*widget);
		}
	}
}

void Editor::Panels::Hierarchy::DetachFromParent(Engine::GameObject & p_actor)
{
	if (auto actorWidget = m_widgetGameObjectLink.find(&p_actor); actorWidget != m_widgetGameObjectLink.end())
	{
		if (p_actor.HasParent() && p_actor.GetParent()->GetChildren().size() == 1)
		{
			if (auto parentWidget = m_widgetGameObjectLink.find(p_actor.GetParent()); parentWidget != m_widgetGameObjectLink.end())
			{
				parentWidget->second->leaf = true;
			}
		}

		auto widget = actorWidget->second;

		m_sceneRoot->ConsiderWidget(*widget);
	}
}

void Editor::Panels::Hierarchy::DeleteGameObjectByInstance(Engine::GameObject& p_actor)
{
    EDITOR_EXEC(RemoveGameObjectFromSelection(p_actor));
	if (auto result = m_widgetGameObjectLink.find(&p_actor); result != m_widgetGameObjectLink.end())
	{
		if (result->second)
		{
			std::erase(m_selectedWidgets, result->second);
			result->second->Destroy();
		}

		if (m_selectedWidget == result->second)
			m_selectedWidget = nullptr;

		if (p_actor.HasParent() && p_actor.GetParent()->GetChildren().size() == 1)
		{
			if (auto parentWidget = m_widgetGameObjectLink.find(p_actor.GetParent()); parentWidget != m_widgetGameObjectLink.end())
			{
				parentWidget->second->leaf = true;
			}
		}

		m_widgetGameObjectLink.erase(result);
	}
}

void Editor::Panels::Hierarchy::AddGameObjectByInstance(Engine::GameObject & p_actor)
{
	if (p_actor.IsEditorTransient())
		return;

	if (m_widgetGameObjectLink.find(&p_actor) != m_widgetGameObjectLink.end())
		return;

	auto& textSelectable = m_sceneRoot->CreateWidget<UI::Widgets::TreeNode>(p_actor.GetName(), true);
	textSelectable.leaf = true;
	textSelectable.AddPlugin<GameObjectContextualMenu>(&p_actor, textSelectable);
    auto& dragSource = textSelectable.AddPlugin<UI::DDSource<std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>>>("GameObject", "Attach selection to...", std::make_pair(&p_actor, &textSelectable));
    dragSource.DragStartEvent += [this, &p_actor]
    {
        m_pendingSelectionClick.reset();
        if (!EDITOR_EXEC(IsGameObjectSelected(p_actor))) EDITOR_EXEC(SelectGameObject(p_actor));
        auto& drag = GetHierarchyDragSelection();
        drag.source = NLS::Editor::Inspector::InspectorTargetHandle::FromObject(p_actor, NLS::Editor::Inspector::InspectorTargetKind::GameObject);
        drag.selection = NLS::Editor::Inspector::EditorSelectionService::Get().GetSelection();
        drag.scope = BuildHierarchyUndoScope(p_actor);
    };
	textSelectable.AddPlugin<UI::DDTarget<std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>>>("GameObject").DataReceivedEvent += [&p_actor](std::pair<Engine::GameObject*, UI::Widgets::TreeNode*> p_element)
	{
		if (p_element.first == nullptr)
			return;

		if (p_actor.IsDescendantOf(p_element.first))
		{
			NLS_LOG_WARNING("Cannot attach \"" + p_element.first->GetName() + "\" to \"" + p_actor.GetName() + "\" because it is a descendant of the latter.");
			return;
		}

		ReparentGameObjectThroughUndo(*p_element.first, &p_actor);
	};
	textSelectable.AddPlugin<UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
		NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent += [&p_actor](NLS::Editor::Assets::EditorAssetDragPayload p_data)
	{
		DropAssetIntoHierarchy(p_data, &p_actor);
	};
	textSelectable.AddPlugin<UI::DDTarget<std::pair<std::string, UI::Widgets::Group*>>>("File").DataReceivedEvent += [&p_actor](std::pair<std::string, UI::Widgets::Group*> p_data)
	{
		DropModelFileIntoHierarchy(p_data, &p_actor);
	};
	auto& dispatcher = textSelectable.AddPlugin<UI::DataDispatcher<std::string>>();

	Engine::GameObject* targetPtr = &p_actor;
	dispatcher.RegisterGatherer([targetPtr] { return targetPtr->GetName(); });

	m_widgetGameObjectLink[targetPtr] = &textSelectable;
	RefreshPrefabPresentation(*targetPtr);

    textSelectable.ClickedEvent += [this, &p_actor] { HandleSelectionClick(p_actor); };
	textSelectable.DoubleClickedEvent += EDITOR_BIND(MoveToTarget, std::ref(p_actor));
}

std::vector<Engine::GameObject*> Editor::Panels::Hierarchy::CaptureVisibleObjects() const
{
    std::unordered_map<const UI::Widgets::TreeNode*, Engine::GameObject*> objects;
    for (const auto& [object, widget] : m_widgetGameObjectLink) objects.emplace(widget, object);
    std::vector<Engine::GameObject*> visible;
    const auto collect = [&](auto&& self, const UI::Widgets::TreeNode& parent) -> void
    {
        if (!parent.IsOpened()) return;
        for (const auto& [item, mode] : parent.GetWidgets())
            if (const auto* node = dynamic_cast<const UI::Widgets::TreeNode*>(item);
                node && node->enabled && !node->IsDestroyed())
            {
                if (const auto found = objects.find(node); found != objects.end()) visible.push_back(found->second);
                self(self, *node);
            }
    };
    if (m_sceneRoot) collect(collect, *m_sceneRoot);
    return visible;
}

void Editor::Panels::Hierarchy::SelectAll()
{
    m_pendingSelectionClick.reset();
    const auto visible = CaptureVisibleObjects();
    EDITOR_EXEC(SelectGameObjects(visible));
    if (!visible.empty()) m_rangeAnchor = visible.back()->GetInstanceID();
}

void Editor::Panels::Hierarchy::HandleSelectionClick(Engine::GameObject& object)
{
    using namespace NLS::Editor::Inspector;
    auto& service = EditorSelectionService::Get();
    const auto handle = InspectorTargetHandle::FromObject(object, InspectorTargetKind::GameObject);
    const auto& io = ImGui::GetIO();
    const bool action = io.KeyCtrl || io.KeySuper;
    const auto before = object.GetScene() ? Core::FilterSceneSelection(service.GetSelection(), *object.GetScene()) : InspectorTargetSet{};
    m_pendingSelectionClick.reset();
    if (io.KeyShift)
    {
        const auto visible = CaptureVisibleObjects();
        const auto begin = std::find_if(visible.begin(), visible.end(), [this](const auto* candidate) { return candidate->GetInstanceID() == m_rangeAnchor; });
        const auto end = std::find(visible.begin(), visible.end(), &object);
        InspectorTargetSet range;
        if (begin != visible.end() && end != visible.end())
            for (auto it = (std::min)(begin, end); it != (std::max)(begin, end) + 1; ++it)
                range.targets.push_back(InspectorTargetHandle::FromObject(**it, InspectorTargetKind::GameObject));
        else { range.targets.push_back(handle); m_rangeAnchor = object.GetInstanceID(); }
        range.activeTarget = handle;
        service.SetSelection(ComposeSelection(before, std::move(range), action ? SelectionOperation::Add : SelectionOperation::Replace));
    }
    else if (action)
    {
        service.SetSelection(ComposeSelection(before, {{handle}}, SelectionOperation::Toggle));
        m_rangeAnchor = object.GetInstanceID();
    }
    else
    {
        m_rangeAnchor = object.GetInstanceID();
        if (service.Contains(handle) && !service.GetSelection().IsSingleTarget())
        {
            m_pendingSelectionClick = handle;
            m_selectionPressPosition = {io.MousePos.x, io.MousePos.y};
        }
        else EDITOR_EXEC(SelectGameObject(object));
    }
}

void Editor::Panels::Hierarchy::OnBeforeDrawWidgets()
{
    if (ImGui::GetIO().AppFocusLost) m_pendingSelectionClick.reset();
    const auto revision = HierarchyUndo::GetHierarchyOrderRevision();
    if (m_hierarchyOrderRevision != revision)
    {
        SynchronizeHierarchyOrder();
        m_hierarchyOrderRevision = revision;
    }
}

void Editor::Panels::Hierarchy::SynchronizeHierarchyOrder()
{
    auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene();
    if (EDITOR_CONTEXT(activePrefabStage).has_value() && EDITOR_CONTEXT(activePrefabStage)->stageScene)
        scene = EDITOR_CONTEXT(activePrefabStage)->stageScene.get();
    if (!scene) return;

    const auto reorder = [this](UI::Widgets::TreeNode& parent, const auto& objects)
    {
        parent.CollectGarbages();
        std::vector<UI::Widgets::AWidget*> ordered;
        for (auto* object : objects)
        {
            if (!object || object->IsEditorTransient()) continue;
            const auto found = m_widgetGameObjectLink.find(object);
            if (found == m_widgetGameObjectLink.end() || found->second->GetParent() != &parent) return;
            ordered.push_back(found->second);
        }
        (void)parent.ReorderChildren(ordered);
    };
    std::vector<Engine::GameObject*> roots;
    for (auto* object : scene->GetGameObjects())
        if (object && !object->HasParent()) roots.push_back(object);
    reorder(*m_sceneRoot, roots);
    for (auto* object : scene->GetGameObjects())
    {
        if (!object || object->IsEditorTransient()) continue;
        const auto found = m_widgetGameObjectLink.find(object);
        if (found != m_widgetGameObjectLink.end()) reorder(*found->second, object->GetChildren());
    }
}

void Editor::Panels::Hierarchy::OnAfterDrawWidgets()
{
    const auto& io = ImGui::GetIO();
    if (m_pendingSelectionClick && !io.MouseDown[0])
    {
        const float dx = io.MousePos.x - m_selectionPressPosition.x, dy = io.MousePos.y - m_selectionPressPosition.y;
        if (ImGui::IsMouseReleased(0) && dx * dx + dy * dy <= io.MouseDragThreshold * io.MouseDragThreshold)
            if (auto* object = dynamic_cast<Engine::GameObject*>(NLS::Editor::Inspector::NativeReflectionAdapter::ResolveObject(*m_pendingSelectionClick)))
                EDITOR_EXEC(SelectGameObject(*object));
        m_pendingSelectionClick.reset();
    }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered() && !ImGui::IsDragDropActive())
    {
        m_pendingSelectionClick.reset(); m_rangeAnchor = NLS::InstanceID_None;
        EDITOR_EXEC(UnselectGameObject());
    }
}

std::vector<NLS::InstanceID> Editor::Panels::Hierarchy::CaptureExpandedObjects() const
{
    std::vector<NLS::InstanceID> ids;
    if(m_sceneRoot && m_sceneRoot->IsOpened())ids.push_back(NLS::InstanceID_None);
    for (const auto& [object, widget] : m_widgetGameObjectLink)
        if (object && widget && widget->IsOpened()) ids.push_back(object->GetInstanceID());
    return ids;
}
void Editor::Panels::Hierarchy::RestoreExpandedObjects(const std::vector<NLS::InstanceID>& ids)
{
    if(m_sceneRoot)
    {
        if(std::find(ids.begin(),ids.end(),NLS::InstanceID_None)!=ids.end())m_sceneRoot->Open();
        else m_sceneRoot->Close();
    }
    for (const auto& [object, widget] : m_widgetGameObjectLink)
        if (object && widget)
        {
            if (std::find(ids.begin(),ids.end(),object->GetInstanceID()) != ids.end()) widget->Open();
            else widget->Close();
        }
}

void Editor::Panels::Hierarchy::RebuildFromCurrentScene()
{
	UnselectGameObjectsWidgets();
	m_pendingSelectionClick.reset();
	m_rangeAnchor = NLS::InstanceID_None;
	m_sceneRoot->RemoveAllWidgets();
	m_selectedWidget = nullptr;
	m_widgetGameObjectLink.clear();
	RefreshSceneRootName();

	auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene();
	if (EDITOR_CONTEXT(activePrefabStage).has_value() && EDITOR_CONTEXT(activePrefabStage)->stageScene)
		scene = EDITOR_CONTEXT(activePrefabStage)->stageScene.get();
	if (!scene)
		return;

	for (auto* actor : scene->GetGameObjects())
	{
		if (actor && !actor->IsEditorTransient())
			AddGameObjectByInstance(*actor);
	}

	for (auto* actor : scene->GetGameObjects())
	{
		if (actor && !actor->IsEditorTransient() && actor->HasParent())
			AttachGameObjectToParent(*actor);
	}
}

void Editor::Panels::Hierarchy::RefreshSceneRootName()
{
	if (m_sceneRoot)
		m_sceneRoot->name = GetCurrentHierarchySceneName();
}

void Editor::Panels::Hierarchy::RefreshPrefabPresentation(Engine::GameObject& p_GameObject)
{
	if (!m_prefabInstanceRegistry)
		return;

	auto found = m_widgetGameObjectLink.find(&p_GameObject);
	if (found == m_widgetGameObjectLink.end() || !found->second)
		return;

	const auto presentation = m_prefabInstanceRegistry->GetPresentation(p_GameObject);
	found->second->useTextColor = presentation.state != NLS::Editor::Assets::PrefabHierarchyState::None;
	found->second->textColor = PrefabColorForToken(presentation.color);
}
