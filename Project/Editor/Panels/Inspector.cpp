#include "Panels/Inspector.h"

#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Texts/Text.h>
#include <UI/Widgets/Texts/TextColored.h>
#include <UI/Widgets/Texts/TextDisabled.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/GroupCollapsable.h>
#include <UI/Widgets/Selection/ComboBox.h>
#include <UI/GUIDrawer.h>
#include <ServiceLocator.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>
#include <Reflection/Variant.h>
#include <imgui.h>
#include "Assets/PrefabUtilityFacade.h"
#include "Core/EditorActions.h"
#include "Components/TransformComponent.h"
#include "Panels/ComponentSearchPanel.h"
#include "Panels/ReflectedPropertyDrawer.h"
#include "GameObject.h"
#include "Settings/TagLayerSettings.h"
#include <algorithm>
#include <functional>
#include <map>

namespace
{
using namespace NLS;

std::string GetComponentDisplayName(const NLS::Engine::Components::Component& component)
{
    std::string displayName = NLS::Editor::Panels::ComponentSearchPanel::MakeDisplayName(component.GetType());
    if (displayName.empty())
        displayName = "Component";

    return displayName;
}

void DrawComponentFallback(NLS::UI::Internal::WidgetContainer &root, Engine::Components::Component &component)
{
    root.CreateWidget<NLS::UI::Widgets::Text>("No reflected fields");
    root.CreateWidget<NLS::UI::Widgets::Text>(component.GetType().GetName());
}

const char* ToPrefabConnectionText(NLS::Editor::Assets::PrefabEditorConnectionState state)
{
    using NLS::Editor::Assets::PrefabEditorConnectionState;
    switch (state)
    {
    case PrefabEditorConnectionState::Connected:
        return "Connected";
    case PrefabEditorConnectionState::MissingSource:
        return "Missing Source";
    case PrefabEditorConnectionState::Disconnected:
        return "Disconnected";
    case PrefabEditorConnectionState::Unpacked:
        return "Unpacked";
    case PrefabEditorConnectionState::Pending:
        return "Pending";
    case PrefabEditorConnectionState::Invalid:
        return "Invalid";
    case PrefabEditorConnectionState::NotAPrefab:
    default:
        return "Not a Prefab";
    }
}

const char* ToPrefabResourceText(NLS::Editor::Assets::PrefabEditorResourceState state)
{
    using NLS::Editor::Assets::PrefabEditorResourceState;
    switch (state)
    {
    case PrefabEditorResourceState::Pending:
        return "Pending";
    case PrefabEditorResourceState::Failed:
        return "Failed";
    case PrefabEditorResourceState::Cancelled:
        return "Cancelled";
    case PrefabEditorResourceState::Ready:
    default:
        return "Ready";
    }
}

NLS::Maths::Color PrefabStateColor(const NLS::Editor::Assets::PrefabEditorState& state)
{
    using NLS::Editor::Assets::PrefabEditorConnectionState;
    using NLS::Editor::Assets::PrefabEditorResourceState;
    if (state.connectionState == PrefabEditorConnectionState::MissingSource ||
        state.connectionState == PrefabEditorConnectionState::Invalid ||
        state.resourceState == PrefabEditorResourceState::Failed)
    {
        return {0.95f, 0.34f, 0.34f, 1.0f};
    }

    if (state.resourceState == PrefabEditorResourceState::Pending)
        return {0.68f, 0.67f, 0.86f, 1.0f};

    if (state.hasOverrides)
        return {0.92f, 0.70f, 0.31f, 1.0f};

    if (state.generatedReadOnly)
        return {0.54f, 0.82f, 0.78f, 1.0f};

    return {0.58f, 0.75f, 0.98f, 1.0f};
}

class HeaderComboBox final : public NLS::UI::Widgets::ComboBox
{
public:
    HeaderComboBox(
        std::map<int, std::string> p_choices,
        std::function<int()> p_gatherer,
        std::function<void(int)> p_provider)
        : HeaderComboBox(
              [choices = std::move(p_choices)] { return choices; },
              std::move(p_gatherer),
              std::move(p_provider))
    {
    }

    HeaderComboBox(
        std::function<std::map<int, std::string>()> p_choicesProvider,
        std::function<int()> p_gatherer,
        std::function<void(int)> p_provider)
        : NLS::UI::Widgets::ComboBox(0),
          m_choicesProvider(std::move(p_choicesProvider)),
          m_gatherer(std::move(p_gatherer)),
          m_provider(std::move(p_provider))
    {
        ValueChangedEvent += [this](int p_choice)
        {
            if (m_provider)
                m_provider(p_choice);
        };
    }

protected:
    void _Draw_Impl() override
    {
        if (m_choicesProvider)
            choices = m_choicesProvider();
        if (m_gatherer)
            currentChoice = m_gatherer();
        NLS::UI::Widgets::ComboBox::_Draw_Impl();
    }

private:
    std::function<std::map<int, std::string>()> m_choicesProvider;
    std::function<int()> m_gatherer;
    std::function<void(int)> m_provider;
};
} // namespace

namespace NLS::Editor::Panels
{
Inspector::Inspector(const std::string& p_title, bool p_opened, const NLS::UI::PanelWindowSettings& p_windowSettings)
    : NLS::UI::PanelWindow(p_title, p_opened, p_windowSettings)
{
    m_lifetimeToken = std::make_shared<bool>(true);
    m_inspectorHeader = &CreateWidget<UI::Widgets::Group>();
    m_inspectorHeader->enabled = false;
    m_gameObjectInfo = &CreateWidget<UI::Widgets::Group>();

    auto& headerColumns = m_inspectorHeader->CreateWidget<UI::Widgets::Columns>(2);
    headerColumns.widths[0] = 88;

    /* Name field */
    auto nameGatherer = [this]
    { return m_targetGameObject ? m_targetGameObject->GetName() : "%undef%"; };
    auto nameProvider = [this](const std::string& p_newName)
    {
        if (m_targetGameObject)
        {
            m_targetGameObject->SetName(p_newName);
            EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
        }
    };
    UI::GUIDrawer::DrawString(headerColumns, "Name", nameGatherer, nameProvider);

    /* Tag field */
    UI::GUIDrawer::CreateTitle(headerColumns, "Tag");
    headerColumns.CreateWidget<HeaderComboBox>(
        [this]()
        {
            auto choices = Settings::TagLayerSettings::BuildTagChoices();
            if (m_targetGameObject)
            {
                const auto tag = m_targetGameObject->GetTag();
                if (Settings::TagLayerSettings::FindTagIndex(tag) == 0 &&
                    tag != Settings::TagLayerSettings::GetTagAt(0))
                    choices.emplace(-1, tag);
            }
            return choices;
        },
        [this]()
        {
            if (!m_targetGameObject)
                return 0;

            const auto tag = m_targetGameObject->GetTag();
            if (Settings::TagLayerSettings::FindTagIndex(tag) == 0 &&
                tag != Settings::TagLayerSettings::GetTagAt(0))
                return -1;
            return Settings::TagLayerSettings::FindTagIndex(tag);
        },
        [this](int p_choice)
        {
            if (m_targetGameObject)
            {
                if (p_choice < 0)
                    return;
                m_targetGameObject->SetTag(Settings::TagLayerSettings::GetTagAt(p_choice));
                EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
            }
        });

    /* Layer field */
    UI::GUIDrawer::CreateTitle(headerColumns, "Layer");
    headerColumns.CreateWidget<HeaderComboBox>(
        Settings::TagLayerSettings::BuildLayerChoices(),
        [this]()
        {
            if (!m_targetGameObject)
                return 0;

            const int layer = m_targetGameObject->GetLayer();
            if (layer < 0 || layer >= static_cast<int>(Settings::TagLayerSettings::LayerCount))
                return 0;
            return layer;
        },
        [this](int p_choice)
        {
            if (m_targetGameObject)
            {
                m_targetGameObject->SetLayer(p_choice);
                EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
            }
        });

    /* Active field */
    auto activeGatherer = [this]
    { return m_targetGameObject ? m_targetGameObject->IsSelfActive() : false; };
    auto activeProvider = [this](bool p_active)
    {
        if (m_targetGameObject)
        {
            m_targetGameObject->SetActive(p_active);
            EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
        }
    };
    UI::GUIDrawer::DrawBoolean(headerColumns, "Active", activeGatherer, activeProvider);

    auto& addComponentButton = m_inspectorHeader->CreateWidget<UI::Widgets::Button>("Add Component", Maths::Vector2{118.f, 0});
    addComponentButton.idleBackgroundColor = Maths::Color{0.23f, 0.49f, 0.82f};
    addComponentButton.hoveredBackgroundColor = Maths::Color{0.29f, 0.58f, 0.93f};
    addComponentButton.clickedBackgroundColor = Maths::Color{0.18f, 0.41f, 0.71f};
    addComponentButton.textColor = Maths::Color::White;
    addComponentButton.ClickedEvent += [this]
    {
        SyncComponentPicker();

        if (m_targetGameObject && m_componentPicker)
        {
            m_componentPicker->SetAnchorRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            m_componentPicker->OpenForGameObject(m_targetGameObject);
        }
    };

    m_inspectorHeader->CreateWidget<UI::Widgets::Separator>();
    m_inspectorHeader->CreateWidget<UI::Widgets::Spacing>(1);
    m_componentPicker = &m_inspectorHeader->CreateWidget<ComponentSearchPanel>();
    m_componentPicker->ComponentAddedEvent += [this]
    {
        EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
        Refresh();
    };

    m_destroyedListener = Engine::GameObject::DestroyedEvent += [this](Engine::GameObject& p_destroyed)
    {
        if (&p_destroyed == m_targetGameObject)
            UnFocus();
    };
}
Inspector::~Inspector()
{
    if (m_lifetimeToken)
        *m_lifetimeToken = false;
    Engine::GameObject::DestroyedEvent -= m_destroyedListener;

    UnFocus();
}

void Inspector::FocusGameObject(Engine::GameObject& p_target)
{
    if (m_targetGameObject)
        UnFocus();

    m_gameObjectInfo->RemoveAllWidgets();

    m_targetGameObject = &p_target;

    m_componentAddedListener = m_targetGameObject->ComponentAddedEvent += [this](auto useless)
    {
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken]
        {
            if (lifetimeToken && *lifetimeToken)
                Refresh();
        }));
    };

    m_componentRemovedListener = m_targetGameObject->ComponentRemovedEvent += [this](auto useless)
    {
        m_gameObjectInfo->RemoveAllWidgets();
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken]
        {
            if (lifetimeToken && *lifetimeToken)
                Refresh();
        }));
    };

    m_inspectorHeader->enabled = true;

    CreateGameObjectInspector(p_target);
    SyncComponentPicker();

    EDITOR_EVENT(GameObjectSelectedEvent).Invoke(*m_targetGameObject);
}

void Inspector::UnFocus()
{
    if (m_targetGameObject)
    {
        m_targetGameObject->ComponentAddedEvent -= m_componentAddedListener;
        m_targetGameObject->ComponentRemovedEvent -= m_componentRemovedListener;
    }

    SoftUnFocus();
}

void Inspector::SoftUnFocus()
{
    if (m_targetGameObject)
    {
        EDITOR_EVENT(GameObjectUnselectedEvent).Invoke(*m_targetGameObject);
        if (m_componentPicker)
            m_componentPicker->ClearTarget();
        m_inspectorHeader->enabled = false;
        m_targetGameObject = nullptr;
        m_gameObjectInfo->RemoveAllWidgets();
    }
}

Engine::GameObject* Inspector::GetTargetGameObject() const
{
    return m_targetGameObject;
}

void Inspector::CreateGameObjectInspector(Engine::GameObject& p_target)
{
    using namespace NLS::Engine::Components;
    std::vector<Component*> components;

    DrawPrefabState(p_target);

    for (const auto& component : p_target.GetComponents())
    {
        if (!component)
            continue;

        auto* rawComponent = component.get();
        if (rawComponent->GetType() == NLS_TYPEOF(TransformComponent))
            continue;

        components.push_back(rawComponent);
    }

    std::sort(
        components.begin(),
        components.end(),
        [](const Component* p_left, const Component* p_right)
        {
            if (p_left == nullptr || p_right == nullptr)
                return p_left < p_right;

            return GetComponentDisplayName(*p_left) < GetComponentDisplayName(*p_right);
        });

    if (auto* transform = p_target.GetComponent<TransformComponent>())
        DrawComponent(transform);

    for (auto* instance : components)
        DrawComponent(instance);
}

void Inspector::DrawPrefabState(Engine::GameObject& p_target)
{
    auto* instance = EDITOR_CONTEXT(prefabInstanceRegistry).FindInstance(p_target);
    if (!instance)
        return;

    NLS::Engine::Assets::PrefabArtifact sourcePrefab;
    sourcePrefab.assetId = instance->prefabAssetId;
    sourcePrefab.graph = instance->SourceGraph();
    sourcePrefab.generatedModelPrefab = instance->generatedReadOnly;

    const auto presentation = EDITOR_CONTEXT(prefabInstanceRegistry).GetPresentation(p_target);
    NLS::Editor::Assets::PrefabEditorStateQuery query;
    query.instance = instance;
    query.prefab = sourcePrefab.graph.objects.empty() ? nullptr : &sourcePrefab;
    query.sourceAssetExists = !presentation.missingAsset;
    query.resourcesPending = presentation.pendingResources;
    query.unpacked = instance->unpacked;
    query.editableSourceArtifactContext = false;
    query.includeDefaultOverrides = true;
    const auto state = NLS::Editor::Assets::PrefabUtilityFacade().GetPrefabEditorState(query);

    auto& prefabGroup = m_gameObjectInfo->CreateWidget<UI::Widgets::GroupCollapsable>("Prefab");
    prefabGroup.opened = true;
    prefabGroup.closable = false;
    prefabGroup.CreateWidget<UI::Widgets::TextColored>(
        std::string("State: ") + ToPrefabConnectionText(state.connectionState),
        PrefabStateColor(state));
    prefabGroup.CreateWidget<UI::Widgets::Text>(
        std::string("Source: ") +
        (instance->prefabSubAssetKey.empty() ? std::string("<unknown>") : instance->prefabSubAssetKey));
    prefabGroup.CreateWidget<UI::Widgets::Text>(
        std::string("Resources: ") + ToPrefabResourceText(state.resourceState));

    if (state.hasOverrides)
    {
        prefabGroup.CreateWidget<UI::Widgets::TextColored>(
            "Overrides: " + std::to_string(state.overrideCount),
            Maths::Color{0.92f, 0.70f, 0.31f, 1.0f});
    }
    else
    {
        prefabGroup.CreateWidget<UI::Widgets::TextDisabled>("Overrides: None");
    }

    if (state.generatedReadOnly)
        prefabGroup.CreateWidget<UI::Widgets::TextColored>(
            "Model prefab source is read-only",
            Maths::Color{0.54f, 0.82f, 0.78f, 1.0f});

    for (const auto& diagnostic : state.diagnostics)
        prefabGroup.CreateWidget<UI::Widgets::TextColored>(
            diagnostic.message.empty() ? diagnostic.code : diagnostic.message,
            Maths::Color{0.95f, 0.56f, 0.34f, 1.0f});

    auto& actions = prefabGroup.CreateWidget<UI::Widgets::Columns>(2);
    auto& applyButton = actions.CreateWidget<UI::Widgets::Button>("Apply", Maths::Vector2{76.f, 0.f});
    applyButton.disabled = true;
    if (applyButton.disabled)
        applyButton.textColor = Maths::Color{0.62f, 0.62f, 0.62f, 1.0f};
    prefabGroup.CreateWidget<UI::Widgets::TextDisabled>("Apply from Inspector is unavailable until an editable source asset context is open.");

    auto& revertButton = actions.CreateWidget<UI::Widgets::Button>("Revert", Maths::Vector2{76.f, 0.f});
    revertButton.disabled = !state.canRevert;
    if (revertButton.disabled)
        revertButton.textColor = Maths::Color{0.62f, 0.62f, 0.62f, 1.0f};
    revertButton.ClickedEvent += [this, instance]
    {
        if (!instance)
            return;

        auto result = NLS::Editor::Assets::PrefabUtilityFacade().RevertPrefabInstance(*instance);
        if (result.status == NLS::Editor::Assets::PrefabOperationStatus::Committed)
        {
            EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
            Refresh();
        }
    };

    m_gameObjectInfo->CreateWidget<UI::Widgets::Spacing>(1);
}

void Inspector::DrawComponent(Engine::Components::Component* p_component)
{
    using namespace NLS::Engine::Components;
    if (!p_component)
        return;

    const bool isTransform = p_component->GetType() == NLS_TYPEOF(TransformComponent);
    std::string title = GetComponentDisplayName(*p_component);
    if (title.empty())
        title = "Component";

    auto& header = m_gameObjectInfo->CreateWidget<UI::Widgets::GroupCollapsable>(title);
    header.closable = !isTransform;
    header.opened = true;
    header.CloseEvent += [this, p_component]
    {
        if (!p_component)
            return;

        auto* owner = p_component->gameobject();
        if (!owner)
            return;

        const auto ownerInstanceID = owner->GetInstanceID();
        const auto componentType = p_component->GetType();
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken, ownerInstanceID, componentType]
        {
            if (!lifetimeToken || !*lifetimeToken)
                return;

            auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene();
            if (!scene)
                return;

            auto* owner = NLS::Object::IDToPointer<NLS::Engine::GameObject>(ownerInstanceID);
            if (!owner)
                return;

            const auto& actors = scene->GetGameObjects();
            if (std::find(actors.begin(), actors.end(), owner) == actors.end())
                return;

            auto* component = owner->GetComponent(componentType, true);
            if (!component)
                return;

            if (m_targetGameObject == owner)
                m_gameObjectInfo->RemoveAllWidgets();

            if (owner->RemoveComponent(component))
            {
                EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
                SyncComponentPicker();
                Refresh();
            }
        }));
    };
    const auto componentType = p_component->GetType();
    const auto &fields = componentType.GetFields();
    if (fields.empty())
    {
        DrawComponentFallback(header, *p_component);
        m_gameObjectInfo->CreateWidget<UI::Widgets::Spacing>(1);
        return;
    }

    ReflectedPropertyDrawerOptions options;
    options.labelWidth = 104.0f;
    options.onFieldChanged = [](const meta::Field&)
    {
        EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
    };
    options.onFieldLayoutChanged = [this](const meta::Field&)
    {
        EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken]
        {
            if (lifetimeToken && *lifetimeToken)
                Refresh();
        }));
    };
    DrawReflectedObject(header, meta::Variant(p_component, meta::variant_policy::WrapObject {}), options);

    m_gameObjectInfo->CreateWidget<UI::Widgets::Spacing>(1);
}

void Inspector::Refresh()
{
    if (m_targetGameObject)
    {
        m_gameObjectInfo->RemoveAllWidgets();
        CreateGameObjectInspector(*m_targetGameObject);
        SyncComponentPicker();
    }
}

void Inspector::SyncComponentPicker()
{
    if (!m_componentPicker)
        return;

    if (m_targetGameObject)
    {
        if (m_componentPicker->GetTargetGameObject() != m_targetGameObject)
            m_componentPicker->SetTargetGameObject(m_targetGameObject);
        else
            m_componentPicker->NotifyGameObjectComponentsChanged();
    }
    else
    {
        m_componentPicker->ClearTarget();
    }
}
} // namespace NLS::Editor::Panels
