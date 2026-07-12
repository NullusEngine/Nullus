#include "Panels/Hierarchy.h"
#include "Core/EditorActions.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetPathUtils.h"

#include <Math/Color.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Plugins/DDSource.h>
#include <UI/Plugins/DDTarget.h>

#include <Debug/Logger.h>

#include <ServiceLocator.h>
#include <Utils/PathParser.h>

#include <UI/Plugins/ContextualMenu.h>

#include "Utils/GameObjectCreationMenu.h"
#include "UI/Widgets/InputFields/InputText.h"

#include <filesystem>
#include <functional>
#include <mutex>

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

	(void)ScheduleHierarchyImportedPrefabPreloadOnce(payload);
	(void)EDITOR_EXEC(CreateGameObjectFromAssetNonBlocking(payload, true, parent));
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

	EDITOR_EXEC(CreateGameObjectFromAsset(path, true, parent));
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
            focusButton.ClickedEvent += [this]
            {
                EDITOR_EXEC(MoveToTarget(*m_target));
            };

            auto& duplicateButton = CreateWidget<UI::Widgets::MenuItem>("Duplicate");
            duplicateButton.ClickedEvent += [this]
            {
                EDITOR_EXEC(DelayAction(EDITOR_BIND(DuplicateGameObject, std::ref(*m_target), nullptr, true), 0));
            };

            auto& deleteButton = CreateWidget<UI::Widgets::MenuItem>("Delete");
            deleteButton.ClickedEvent += [this]
            {
                EDITOR_EXEC(DestroyGameObject(std::ref(*m_target)));
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
				m_target->SetName(p_newName);
				EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
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
		m_sceneRoot->ConsiderWidget(*p_element.second);

		p_element.first->DetachFromParent();
		EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
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

	m_gameObjectUnselectedListener = EDITOR_EVENT(GameObjectUnselectedEvent) += std::bind(&Hierarchy::UnselectGameObjectsWidgets, this);
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
	m_gameObjectSelectedListener = EDITOR_EVENT(GameObjectSelectedEvent) += std::bind(&Hierarchy::SelectGameObjectByInstance, this, std::placeholders::_1);
	m_gameObjectAttachedListener = Engine::GameObject::AttachEvent += std::bind(&Hierarchy::AttachGameObjectToParent, this, std::placeholders::_1);
	m_gameObjectDetachedListener = Engine::GameObject::DettachEvent += std::bind(&Hierarchy::DetachFromParent, this, std::placeholders::_1);
}

Editor::Panels::Hierarchy::~Hierarchy()
{
	EDITOR_EVENT(GameObjectUnselectedEvent) -= m_gameObjectUnselectedListener;
	EDITOR_CONTEXT(sceneManager).SceneUnloadEvent -= m_sceneUnloadListener;
	EDITOR_CONTEXT(sceneManager).SceneLoadEvent -= m_sceneLoadListener;
	EDITOR_CONTEXT(sceneManager).CurrentSceneSourcePathChangedEvent -= m_sceneSourcePathChangedListener;
	EDITOR_CONTEXT(sceneManager).CurrentSceneDirtyStateChangedEvent -= m_sceneDirtyStateChangedListener;
	Engine::GameObject::CreatedEvent -= m_gameObjectCreatedListener;
	Engine::GameObject::DestroyedEvent -= m_gameObjectDestroyedListener;
	EDITOR_EVENT(GameObjectSelectedEvent) -= m_gameObjectSelectedListener;
	Engine::GameObject::AttachEvent -= m_gameObjectAttachedListener;
	Engine::GameObject::DettachEvent -= m_gameObjectDetachedListener;
}

void Editor::Panels::Hierarchy::Clear()
{
	EDITOR_EXEC(UnselectGameObject());

	m_sceneRoot->RemoveAllWidgets();
	m_widgetGameObjectLink.clear();
	RefreshSceneRootName();
}

void Editor::Panels::Hierarchy::UnselectGameObjectsWidgets()
{
	for (auto& widget : m_widgetGameObjectLink)
		widget.second->selected = false;
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
	if (auto result = m_widgetGameObjectLink.find(&p_actor); result != m_widgetGameObjectLink.end())
	{
		if (result->second)
		{
			result->second->Destroy();
		}

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
	textSelectable.AddPlugin<UI::DDSource<std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>>>("GameObject", "Attach to...", std::make_pair(&p_actor, &textSelectable));
	textSelectable.AddPlugin<UI::DDTarget<std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>>>("GameObject").DataReceivedEvent += [&p_actor, &textSelectable](std::pair<Engine::GameObject*, UI::Widgets::TreeNode*> p_element)
	{
		if (p_actor.IsDescendantOf(p_element.first))
		{
			NLS_LOG_WARNING("Cannot attach \"" + p_element.first->GetName() + "\" to \"" + p_actor.GetName() + "\" because it is a descendant of the latter.");
			return;
		}

		p_element.first->SetParent(p_actor);
		EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
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

	textSelectable.ClickedEvent += EDITOR_BIND(SelectGameObject, std::ref(p_actor));
	textSelectable.DoubleClickedEvent += EDITOR_BIND(MoveToTarget, std::ref(p_actor));
}

void Editor::Panels::Hierarchy::RebuildFromCurrentScene()
{
	m_sceneRoot->RemoveAllWidgets();
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
