#include "Panels/Inspector.h"

#include <UI/Widgets/Texts/Text.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Drags/DragMultipleFloats.h>
#include <UI/Widgets/Drags/DragFloat.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Visual/Image.h>
#include <UI/Widgets/InputFields/InputFloat.h>
#include <UI/Widgets/Selection/ColorEdit.h>
#include <UI/Plugins/DDTarget.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/GroupCollapsable.h>
#include <UI/GUIDrawer.h>
#include <ServiceLocator.h>
#include <ResourceManagement/ModelManager.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>
#include <Reflection/Field.h>
#include <Reflection/Type.h>
#include <Reflection/Variant.h>
#include <Reflection/Array.h>
#include <imgui.h>
#include "Core/EditorActions.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/LightComponent.h"
#include "Components/CameraComponent.h"
#include "Components/TransformComponent.h"
#include "Panels/ComponentSearchPanel.h"
#include "Panels/InspectorReflectionUtils.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Settings/EProjectionMode.h"
#include "Rendering/Settings/ELightType.h"
#include "GameObject.h"

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

std::string FormatFieldLabel(const std::string &name)
{
    if (name.empty())
        return {};

    std::string result;
    result.reserve(name.size() + 4);

    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        if (i > 0 && std::isupper(static_cast<unsigned char>(c)) != 0)
            result.push_back(' ');
        result.push_back(c);
    }

    if (!result.empty())
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));

    return result;
}

template <typename TValue>
TValue GetFieldValue(meta::Variant &instance, const meta::Field &field)
{
    return field.GetValue(instance).GetValue<TValue>();
}

template <typename TValue>
void SetFieldValue(meta::Variant &instance, const meta::Field &field, const TValue &value)
{
    meta::Variant updatedValue(const_cast<TValue &>(value), meta::variant_policy::NoCopy{});
    field.SetValue(instance, updatedValue);
}

void DrawFloatArrayField(NLS::UI::Internal::WidgetContainer &root, meta::Variant &instance, const meta::Field &field)
{
    const auto label = FormatFieldLabel(field.GetName());
    auto values = GetFieldValue<NLS::Array<float>>(instance, field);
    if (values.empty())
        values.resize(16, 0.0f);

    for (size_t row = 0; row < 4; ++row)
    {
        NLS::UI::GUIDrawer::DrawVec4(
            root,
            label + " " + std::to_string(row),
            [instance, field, row]() mutable
            {
                auto current = GetFieldValue<NLS::Array<float>>(instance, field);
                Maths::Vector4 value {};
                const size_t base = row * 4;
                for (size_t column = 0; column < 4; ++column)
                    value[column] = base + column < current.size() ? current[base + column] : 0.0f;
                return value;
            },
            [instance, field, row](Maths::Vector4 value) mutable
            {
                auto current = GetFieldValue<NLS::Array<float>>(instance, field);
                if (current.size() < 16)
                    current.resize(16, 0.0f);

                const size_t base = row * 4;
                for (size_t column = 0; column < 4; ++column)
                    current[base + column] = value[column];

                SetFieldValue(instance, field, current);
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT
        );
    }
}

void DrawStringArrayField(NLS::UI::Internal::WidgetContainer &root, meta::Variant &instance, const meta::Field &field)
{
    const auto label = FormatFieldLabel(field.GetName());
    auto values = GetFieldValue<NLS::Array<std::string>>(instance, field);
    if (values.empty())
        values.resize(1);

    for (size_t index = 0; index < values.size(); ++index)
    {
        NLS::UI::GUIDrawer::DrawDDString(
            root,
            label + " " + std::to_string(index),
            [instance, field, index]() mutable
            {
                auto current = GetFieldValue<NLS::Array<std::string>>(instance, field);
                return index < current.size() ? current[index] : std::string {};
            },
            [instance, field, index](std::string value) mutable
            {
                auto current = GetFieldValue<NLS::Array<std::string>>(instance, field);
                if (current.size() <= index)
                    current.resize(index + 1);
                current[index] = std::move(value);
                SetFieldValue(instance, field, current);
            },
            "File"
        );
    }
}

void DrawBoundingSphereField(NLS::UI::Internal::WidgetContainer &root, meta::Variant &instance, const meta::Field &field)
{
    const auto label = FormatFieldLabel(field.GetName());

    NLS::UI::GUIDrawer::DrawVec3(
        root,
        label + " Position",
        [instance, field]() mutable
        {
            return GetFieldValue<Render::Geometry::BoundingSphere>(instance, field).position;
        },
        [instance, field](Maths::Vector3 value) mutable
        {
            auto sphere = GetFieldValue<Render::Geometry::BoundingSphere>(instance, field);
            sphere.position = value;
            SetFieldValue(instance, field, sphere);
        },
        0.01f,
        NLS::UI::GUIDrawer::_MIN_FLOAT,
        NLS::UI::GUIDrawer::_MAX_FLOAT
    );

    NLS::UI::GUIDrawer::DrawScalar<float>(
        root,
        label + " Radius",
        [instance, field]() mutable
        {
            return GetFieldValue<Render::Geometry::BoundingSphere>(instance, field).radius;
        },
        [instance, field](float value) mutable
        {
            auto sphere = GetFieldValue<Render::Geometry::BoundingSphere>(instance, field);
            sphere.radius = value;
            SetFieldValue(instance, field, sphere);
        },
        0.01f,
        0.0f,
        NLS::UI::GUIDrawer::_MAX_FLOAT
    );
}

void DrawEnumField(NLS::UI::Internal::WidgetContainer &root, meta::Variant &instance, const meta::Field &field, const std::map<int, std::string> &choices)
{
    const auto label = FormatFieldLabel(field.GetName());
    NLS::UI::GUIDrawer::CreateTitle(root, label);
    auto &combo = root.CreateWidget<NLS::UI::Widgets::ComboBox>(GetFieldValue<int>(instance, field));
    combo.choices = choices;
    combo.ValueChangedEvent += [instance, field](int value) mutable
    {
        SetFieldValue(instance, field, value);
    };
}

void DrawEnumValue(NLS::UI::Internal::WidgetContainer &root, const std::string &label, int currentValue, const std::map<int, std::string> &choices, const std::function<void(int)> &setter)
{
    NLS::UI::GUIDrawer::CreateTitle(root, label);
    auto &combo = root.CreateWidget<NLS::UI::Widgets::ComboBox>(currentValue);
    combo.choices = choices;
    combo.ValueChangedEvent += [setter](int value)
    {
        setter(value);
    };
}

void DrawReflectedField(NLS::UI::Internal::WidgetContainer &root, meta::Variant &instance, const meta::Field &field)
{
    const auto fieldType = field.GetType();
    const auto label = FormatFieldLabel(field.GetName());

    if (fieldType.IsEnum())
    {
        const auto choices = NLS::Editor::Panels::BuildEnumChoices(fieldType);
        if (!choices.empty())
        {
            DrawEnumField(root, instance, field, choices);
            return;
        }
    }

    if (fieldType == NLS_TYPEOF(bool))
    {
        NLS::UI::GUIDrawer::DrawBoolean(
            root,
            label,
            [instance, field]() mutable { return GetFieldValue<bool>(instance, field); },
            [instance, field](bool value) mutable { SetFieldValue(instance, field, value); }
        );
        return;
    }

    if (fieldType == NLS_TYPEOF(int))
    {
        NLS::UI::GUIDrawer::DrawScalar<int>(
            root,
            label,
            [instance, field]() mutable { return GetFieldValue<int>(instance, field); },
            [instance, field](int value) mutable { SetFieldValue(instance, field, value); }
        );
        return;
    }

    if (fieldType == NLS_TYPEOF(float))
    {
        NLS::UI::GUIDrawer::DrawScalar<float>(
            root,
            label,
            [instance, field]() mutable { return GetFieldValue<float>(instance, field); },
            [instance, field](float value) mutable { SetFieldValue(instance, field, value); },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT
        );
        return;
    }

    if (fieldType == NLS_TYPEOF(std::string))
    {
        NLS::UI::GUIDrawer::DrawDDString(
            root,
            label,
            [instance, field]() mutable { return GetFieldValue<std::string>(instance, field); },
            [instance, field](std::string value) mutable { SetFieldValue(instance, field, value); },
            "File"
        );
        return;
    }

    if (fieldType == NLS_TYPEOF(Maths::Vector3))
    {
        NLS::UI::GUIDrawer::DrawVec3(
            root,
            label,
            [instance, field]() mutable { return GetFieldValue<Maths::Vector3>(instance, field); },
            [instance, field](Maths::Vector3 value) mutable { SetFieldValue(instance, field, value); },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT
        );
        return;
    }

    if (fieldType == NLS_TYPEOF(Maths::Vector4))
    {
        NLS::UI::GUIDrawer::DrawVec4(
            root,
            label,
            [instance, field]() mutable { return GetFieldValue<Maths::Vector4>(instance, field); },
            [instance, field](Maths::Vector4 value) mutable { SetFieldValue(instance, field, value); },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT
        );
        return;
    }

    if (fieldType == NLS_TYPEOF(Maths::Quaternion))
    {
        NLS::UI::GUIDrawer::DrawQuat(
            root,
            label,
            [instance, field]() mutable { return GetFieldValue<Maths::Quaternion>(instance, field); },
            [instance, field](Maths::Quaternion value) mutable { SetFieldValue(instance, field, value); },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT
        );
        return;
    }

    if (fieldType == NLS_TYPEOF(Render::Geometry::BoundingSphere))
    {
        DrawBoundingSphereField(root, instance, field);
        return;
    }

    if (fieldType == NLS_TYPEOF(NLS::Array<std::string>))
    {
        DrawStringArrayField(root, instance, field);
        return;
    }

    if (fieldType == NLS_TYPEOF(NLS::Array<float>))
    {
        DrawFloatArrayField(root, instance, field);
        return;
    }

    root.CreateWidget<NLS::UI::Widgets::Text>(label + " (unsupported)");
    root.CreateWidget<NLS::UI::Widgets::Text>(fieldType.GetName());
}

void DrawComponentFallback(NLS::UI::Internal::WidgetContainer &root, Engine::Components::Component &component)
{
    root.CreateWidget<NLS::UI::Widgets::Text>("No reflected fields");
    root.CreateWidget<NLS::UI::Widgets::Text>(component.GetType().GetName());
}
} // namespace

namespace NLS::Editor::Panels
{
Inspector::Inspector(const std::string& p_title, bool p_opened, const NLS::UI::PanelWindowSettings& p_windowSettings)
    : NLS::UI::PanelWindow(p_title, p_opened, p_windowSettings)
{
    m_inspectorHeader = &CreateWidget<UI::Widgets::Group>();
    m_inspectorHeader->enabled = false;
    m_actorInfo = &CreateWidget<UI::Widgets::Group>();

    auto& headerColumns = m_inspectorHeader->CreateWidget<UI::Widgets::Columns>(2);
    headerColumns.widths[0] = 88;

    /* Name field */
    auto nameGatherer = [this]
    { return m_targetActor ? m_targetActor->GetName() : "%undef%"; };
    auto nameProvider = [this](const std::string& p_newName)
    { if (m_targetActor) m_targetActor->SetName(p_newName); };
    UI::GUIDrawer::DrawString(headerColumns, "Name", nameGatherer, nameProvider);

    /* Tag field */
    auto tagGatherer = [this]
    { return m_targetActor ? m_targetActor->GetTag() : "%undef%"; };
    auto tagProvider = [this](const std::string& p_newName)
    { if (m_targetActor) m_targetActor->SetTag(p_newName); };
    UI::GUIDrawer::DrawString(headerColumns, "Tag", tagGatherer, tagProvider);

    /* Active field */
    auto activeGatherer = [this]
    { return m_targetActor ? m_targetActor->IsSelfActive() : false; };
    auto activeProvider = [this](bool p_active)
    { if (m_targetActor) m_targetActor->SetActive(p_active); };
    UI::GUIDrawer::DrawBoolean(headerColumns, "Active", activeGatherer, activeProvider);

    auto& addComponentButton = m_inspectorHeader->CreateWidget<UI::Widgets::Button>("Add Component", Maths::Vector2{118.f, 0});
    addComponentButton.idleBackgroundColor = Maths::Color{0.23f, 0.49f, 0.82f};
    addComponentButton.hoveredBackgroundColor = Maths::Color{0.29f, 0.58f, 0.93f};
    addComponentButton.clickedBackgroundColor = Maths::Color{0.18f, 0.41f, 0.71f};
    addComponentButton.textColor = Maths::Color::White;
    addComponentButton.ClickedEvent += [this]
    {
        SyncComponentPicker();

        if (m_targetActor && m_componentPicker)
        {
            m_componentPicker->SetAnchorRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            m_componentPicker->OpenForActor(m_targetActor);
        }
    };

    m_inspectorHeader->CreateWidget<UI::Widgets::Separator>();
    m_inspectorHeader->CreateWidget<UI::Widgets::Spacing>(1);
    m_componentPicker = &m_inspectorHeader->CreateWidget<ComponentSearchPanel>();
    m_componentPicker->ComponentAddedEvent += [this]
    {
        Refresh();
    };

    m_destroyedListener = Engine::GameObject::DestroyedEvent += [this](Engine::GameObject& p_destroyed)
    {
        if (&p_destroyed == m_targetActor)
            UnFocus();
    };
}
Inspector::~Inspector()
{
    Engine::GameObject::DestroyedEvent -= m_destroyedListener;

    UnFocus();
}

void Inspector::FocusActor(Engine::GameObject& p_target)
{
    if (m_targetActor)
        UnFocus();

    m_actorInfo->RemoveAllWidgets();

    m_targetActor = &p_target;

    m_componentAddedListener = m_targetActor->ComponentAddedEvent += [this](auto useless)
    { EDITOR_EXEC(DelayAction([this]
                              { Refresh(); })); };

    m_componentRemovedListener = m_targetActor->ComponentRemovedEvent += [this](auto useless)
    { EDITOR_EXEC(DelayAction([this]
                              { Refresh(); })); };

    m_inspectorHeader->enabled = true;

    CreateActorInspector(p_target);
    SyncComponentPicker();

    EDITOR_EVENT(ActorSelectedEvent).Invoke(*m_targetActor);
}

void Inspector::UnFocus()
{
    if (m_targetActor)
    {
        m_targetActor->ComponentAddedEvent -= m_componentAddedListener;
        m_targetActor->ComponentRemovedEvent -= m_componentRemovedListener;
    }

    SoftUnFocus();
}

void Inspector::SoftUnFocus()
{
    if (m_targetActor)
    {
        EDITOR_EVENT(ActorUnselectedEvent).Invoke(*m_targetActor);
        if (m_componentPicker)
            m_componentPicker->ClearTarget();
        m_inspectorHeader->enabled = false;
        m_targetActor = nullptr;
        m_actorInfo->RemoveAllWidgets();
    }
}

Engine::GameObject* Inspector::GetTargetActor() const
{
    return m_targetActor;
}

void Inspector::CreateActorInspector(Engine::GameObject& p_target)
{
    using namespace NLS::Engine::Components;
    std::vector<Component*> components;

    for (const auto& component : p_target.GetComponents())
    {
        if (!component)
            continue;

        auto* rawComponent = component.get();
        if (dynamic_cast<TransformComponent*>(rawComponent))
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

void Inspector::DrawComponent(Engine::Components::Component* p_component)
{
    using namespace NLS::Engine::Components;
    if (!p_component)
        return;

    const bool isTransform = dynamic_cast<TransformComponent*>(p_component) != nullptr;
    std::string title = GetComponentDisplayName(*p_component);
    if (title.empty())
        title = "Component";

    auto& header = m_actorInfo->CreateWidget<UI::Widgets::GroupCollapsable>(title);
    header.closable = !isTransform;
    header.opened = true;
    header.CloseEvent += [this, p_component]
    {
        if (!p_component)
            return;

        auto* owner = p_component->gameobject();
        if (!owner)
            return;

        const auto actorId = owner->GetWorldID();
        const auto componentType = p_component->GetType();
        EDITOR_EXEC(DelayAction([this, actorId, componentType]
        {
            auto* scene = EDITOR_CONTEXT(sceneManager).GetCurrentScene();
            if (!scene)
                return;

            auto* actor = scene->FindActorByID(actorId);
            if (!actor)
                return;

            auto* component = actor->GetComponent(componentType, true);
            if (!component)
                return;

            if (actor->RemoveComponent(component))
                SyncComponentPicker();
        }));
    };
    auto& columns = header.CreateWidget<UI::Widgets::Columns>(2);
    columns.widths[0] = 104;

    meta::Variant componentInstance(p_component, meta::variant_policy::WrapObject {});
    const auto componentType = p_component->GetType();
    const auto &fields = componentType.GetFields();
    if (fields.empty())
    {
        DrawComponentFallback(columns, *p_component);
        m_actorInfo->CreateWidget<UI::Widgets::Spacing>(1);
        return;
    }

    for (const auto &field : fields)
    {
        if (!field.IsValid())
            continue;

        DrawReflectedField(columns, componentInstance, field);
    }

    m_actorInfo->CreateWidget<UI::Widgets::Spacing>(1);
}

void Inspector::Refresh()
{
    if (m_targetActor)
    {
        m_actorInfo->RemoveAllWidgets();
        CreateActorInspector(*m_targetActor);
        SyncComponentPicker();
    }
}

void Inspector::SyncComponentPicker()
{
    if (!m_componentPicker)
        return;

    if (m_targetActor)
    {
        if (m_componentPicker->GetTargetActor() != m_targetActor)
            m_componentPicker->SetTargetActor(m_targetActor);
        else
            m_componentPicker->NotifyActorComponentsChanged();
    }
    else
    {
        m_componentPicker->ClearTarget();
    }
}
} // namespace NLS::Editor::Panels
