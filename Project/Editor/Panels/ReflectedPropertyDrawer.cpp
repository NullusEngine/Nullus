#include "Panels/ReflectedPropertyDrawer.h"

#include <Reflection/Array.h>
#include <Reflection/ArrayWrapper.h>
#include <Reflection/Argument.h>
#include <Reflection/RuntimeMetaProperties.h>
#include <UI/GUIDrawer.h>
#include <UI/Plugins/DDTarget.h>
#include <UI/Widgets/AWidget.h>
#include <UI/Widgets/Buttons/ButtonSmall.h>
#include <UI/Widgets/InputFields/InputText.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/TreeNode.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Selection/ComboBox.h>
#include <UI/Widgets/Selection/ColorEdit.h>
#include <UI/Widgets/Sliders/SliderFloat.h>
#include <UI/Widgets/Sliders/SliderInt.h>
#include <UI/Widgets/Texts/Text.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "Assets/AssetBrowserPresentation.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Components/Component.h"
#include "GameObject.h"
#include "LayerMask.h"
#include "Math/ExternalReflection.h"
#include "Rendering/Geometry/BoundingSphere.h"
#include "Rendering/Geometry/Bounds.h"
#include "Rendering/ExternalReflection.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture2D.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/PPtr.h"
#include "Serialize/PPtrResourceTypes.h"
#include "Settings/TagLayerSettings.h"

namespace
{
using namespace NLS;
using ReflectedVariantHandle = std::shared_ptr<meta::Variant>;
constexpr int kMaxInspectorArraySize = 1024;
constexpr int kMaxReflectedValueDepth = 8;

enum class ReflectedElementSupport
{
    Unsupported,
    Bool,
    Int,
    Float,
    String,
    Vector2,
    Vector3,
    Vector4,
    Color,
    ObjectReference,
    ReflectedValue
};

struct ReflectedArrayFieldState
{
    ReflectedArrayFieldState(
        ReflectedVariantHandle p_instance,
        const meta::Field& p_field,
        meta::Type p_elementType,
        NLS::Editor::Panels::ReflectedPropertyDrawerOptions p_options,
        meta::Variant&& p_value,
        int p_depth = 0,
        std::vector<meta::Type> p_typeStack = {})
        : instance(std::move(p_instance)),
          field(p_field),
          elementType(p_elementType),
          options(std::move(p_options)),
          value(std::move(p_value)),
          array(value.GetArray()),
          depth(p_depth),
          typeStack(std::move(p_typeStack))
    {
    }

    ReflectedVariantHandle instance;
    const meta::Field field;
    const meta::Type elementType;
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions options;
    meta::Variant value;
    meta::ArrayWrapper array;
    int depth = 0;
    std::vector<meta::Type> typeStack;
};

class ReflectedBoolWidget final : public NLS::UI::Widgets::AWidget
{
public:
    ReflectedBoolWidget(std::function<bool()> p_gatherer, std::function<void(bool)> p_provider)
        : m_gatherer(std::move(p_gatherer)),
          m_provider(std::move(p_provider))
    {
    }

protected:
    void _Draw_Impl() override
    {
        bool value = m_gatherer ? m_gatherer() : false;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.42f, 0.45f, 0.48f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.16f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.20f, 0.22f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.25f, 0.28f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.78f, 0.82f, 0.88f, 1.0f));

        if (ImGui::Checkbox(("##ReflectedBool" + m_widgetID).c_str(), &value) && m_provider)
            m_provider(value);

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar();
    }

private:
    std::function<bool()> m_gatherer;
    std::function<void(bool)> m_provider;
};

bool IsSupportedIdentityObjectReferenceType(const meta::Type& p_type);
bool IsCompatibleArtifactType(const meta::Type& p_type, NLS::Core::Assets::ArtifactType p_artifactType);
void NotifyChanged(
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options,
    const meta::Field& p_field);
bool DrawReflectedValueField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_value,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options,
    int p_depth,
    std::vector<meta::Type> p_typeStack,
    std::function<void()> p_commit);
std::optional<NLS::Engine::Serialize::ObjectIdentifier> MakeObjectReferenceIdentifier(
    const meta::Type& p_type,
    const NLS::Editor::Assets::EditorAssetDragPayload& p_payload);
bool SetIdentityObjectReferenceField(
    meta::Variant& p_instance,
    const meta::Field& p_field,
    const NLS::Engine::Serialize::ObjectIdentifier& p_identifier);

class ObjectReferencePickerPlugin final : public NLS::UI::IPlugin
{
public:
    ObjectReferencePickerPlugin(
        ReflectedVariantHandle p_instance,
        meta::Field p_field,
        NLS::Editor::Panels::ReflectedPropertyDrawerOptions p_options)
        : m_instance(std::move(p_instance)),
          m_field(std::move(p_field)),
          m_options(std::move(p_options))
    {
    }

    void RequestOpen()
    {
        m_openRequested = true;
    }

    void Execute() override
    {
        if (m_openRequested)
        {
            ImGui::OpenPopup(GetPopupId().c_str());
            m_openRequested = false;
            RefreshEntries();
        }

        if (!ImGui::BeginPopup(GetPopupId().c_str()))
            return;

        DrawSearch();
        ImGui::Separator();
        DrawEntries();
        ImGui::EndPopup();
    }

private:
    std::string GetPopupId() const
    {
        return "##ObjectReferencePicker" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
    }

    void DrawSearch()
    {
        char buffer[128] {};
        m_query.copy(buffer, sizeof(buffer) - 1u);
        if (ImGui::InputText("##ObjectReferenceSearch", buffer, sizeof(buffer)))
        {
            m_query = buffer;
            RefreshEntries();
        }
    }

    void DrawEntries()
    {
        if (m_entries.empty())
        {
            ImGui::TextUnformatted("No matching assets");
            return;
        }

        for (const auto& entry : m_entries)
        {
            if (!ImGui::MenuItem(entry.displayName.c_str()))
                continue;

            const auto identifier = MakeObjectReferenceIdentifier(m_field.GetType(), entry.payload);
            if (identifier.has_value() && SetIdentityObjectReferenceField(*m_instance, m_field, *identifier))
                NotifyChanged(m_options, m_field);
            ImGui::CloseCurrentPopup();
            return;
        }
    }

    void RefreshEntries()
    {
        m_entries.clear();
        if (!IsSupportedIdentityObjectReferenceType(m_field.GetType()))
            return;

        const auto query = NLS::Editor::Panels::NormalizeReflectedSearchText(m_query);
        for (const auto& entry : NLS::Editor::Assets::GetObjectReferencePickerEntries())
        {
            if (!IsCompatibleArtifactType(
                    m_field.GetType(),
                    NLS::Editor::Assets::GetEditorAssetDragPayloadArtifactType(entry.payload)))
                continue;

            if (!query.empty() &&
                NLS::Editor::Panels::NormalizeReflectedSearchText(entry.displayName).find(query) == std::string::npos)
                continue;

            m_entries.push_back(entry);
        }

        std::sort(
            m_entries.begin(),
            m_entries.end(),
            [](const NLS::Editor::Assets::ObjectReferencePickerEntry& p_left,
               const NLS::Editor::Assets::ObjectReferencePickerEntry& p_right)
            {
                return p_left.displayName < p_right.displayName;
            });
        m_entries.erase(
            std::unique(
                m_entries.begin(),
                m_entries.end(),
                [](const NLS::Editor::Assets::ObjectReferencePickerEntry& p_left,
                   const NLS::Editor::Assets::ObjectReferencePickerEntry& p_right)
                {
                    return p_left.displayName == p_right.displayName;
                }),
            m_entries.end());
    }

    ReflectedVariantHandle m_instance;
    meta::Field m_field;
    NLS::Editor::Panels::ReflectedPropertyDrawerOptions m_options;
    std::string m_query;
    std::vector<NLS::Editor::Assets::ObjectReferencePickerEntry> m_entries;
    bool m_openRequested = false;
};

template <typename TValue>
TValue GetFieldValue(meta::Variant& p_instance, const meta::Field& p_field)
{
    return p_field.GetValue(p_instance).GetValue<TValue>();
}

template <typename TValue>
void SetFieldValue(meta::Variant& p_instance, const meta::Field& p_field, const TValue& p_value)
{
    meta::Variant updatedValue(const_cast<TValue&>(p_value), meta::variant_policy::NoCopy {});
    p_field.SetValue(p_instance, updatedValue);
}

int GetEnumFieldValue(meta::Variant& p_instance, const meta::Field& p_field)
{
    return p_field.GetValue(p_instance).ToInt();
}

void SetEnumFieldValue(meta::Variant& p_instance, const meta::Field& p_field, int p_value)
{
    const auto fieldType = p_field.GetType();
    if (!fieldType.IsValid() || !fieldType.IsEnum())
        return;

    const auto enumType = fieldType.GetEnum();
    for (const auto& key : enumType.GetKeys())
    {
        auto enumValue = enumType.GetValue(key);
        if (enumValue.IsValid() && enumValue.ToInt() == p_value)
        {
            p_field.SetValue(p_instance, enumValue);
            return;
        }
    }
}

void NotifyChanged(const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options, const meta::Field& p_field)
{
    if (p_options.onFieldChanged)
        p_options.onFieldChanged(p_field);
}

void NotifyLayoutChanged(const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options, const meta::Field& p_field)
{
    if (p_options.onFieldLayoutChanged)
        p_options.onFieldLayoutChanged(p_field);
}

std::string BuildFieldLabel(
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    std::string label = NLS::Editor::Panels::FormatReflectedFieldLabel(p_field.GetName());
    if (p_options.fieldBadgeProvider)
    {
        const std::string badge = p_options.fieldBadgeProvider(p_field);
        if (!badge.empty())
            label += " (" + badge + ")";
    }
    return label;
}

bool IsSinglePointerType(const meta::Type& p_type)
{
    const auto typeName = p_type.GetName();
    return p_type.IsPointer() &&
           !typeName.empty() &&
           typeName.back() == '*' &&
           (typeName.size() < 2u || typeName[typeName.size() - 2u] != '*');
}

bool IsGameObjectPointerType(const meta::Type& p_type)
{
    return IsSinglePointerType(p_type) &&
           p_type.GetDecayedType() == NLS_TYPEOF(NLS::Engine::GameObject);
}

bool IsComponentPointerType(const meta::Type& p_type)
{
    if (!IsSinglePointerType(p_type))
        return false;

    const auto objectType = p_type.GetDecayedType();
    const auto componentType = NLS_TYPEOF(NLS::Engine::Components::Component);
    return objectType.IsValid() &&
           (objectType == componentType || objectType.DerivesFrom(componentType));
}

bool IsSceneObjectReferenceType(const meta::Type& p_type)
{
    return IsGameObjectPointerType(p_type) || IsComponentPointerType(p_type);
}

bool IsResourceObjectReferenceType(const meta::Type& p_type)
{
    return p_type == NLS_TYPEOF(NLS::Render::Resources::Texture2D*) ||
           p_type == NLS_TYPEOF(NLS::Render::Resources::Mesh*) ||
           p_type == NLS_TYPEOF(NLS::Render::Resources::Shader*) ||
           p_type == NLS_TYPEOF(NLS::Render::Resources::Material*);
}

bool IsIdentityObjectReferenceType(const meta::Type& p_type)
{
    const auto typeName = p_type.GetName();
    return typeName.rfind("NLS::Engine::Serialize::PPtr<", 0) == 0;
}

bool IsSupportedIdentityObjectReferenceType(const meta::Type& p_type)
{
#define NLS_EDITOR_MATCH_PPTR_RESOURCE_TYPE(type, label, artifactType, subAssetPrefix) \
    if (p_type == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>)) return true;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_MATCH_PPTR_RESOURCE_TYPE)
#undef NLS_EDITOR_MATCH_PPTR_RESOURCE_TYPE
    return false;
}

bool IsReflectedValueElementType(const meta::Type& p_type)
{
    if (!p_type.IsValid() || p_type.IsArray() || p_type.IsPointer())
        return false;
    if (!p_type.IsClass() || p_type.GetFields().empty())
        return false;
    if (p_type == NLS_TYPEOF(NLS::Object) || p_type.DerivesFrom(NLS_TYPEOF(NLS::Object)))
        return false;
    return true;
}

ReflectedElementSupport GetArrayElementSupport(const meta::Type& p_type)
{
    if (p_type == NLS_TYPEOF(std::string))
        return ReflectedElementSupport::String;
    if (p_type == NLS_TYPEOF(float))
        return ReflectedElementSupport::Float;
    if (p_type == NLS_TYPEOF(int))
        return ReflectedElementSupport::Int;
    if (p_type == NLS_TYPEOF(bool))
        return ReflectedElementSupport::Bool;
    if (p_type == NLS_TYPEOF(Maths::Vector2))
        return ReflectedElementSupport::Vector2;
    if (p_type == NLS_TYPEOF(Maths::Vector3))
        return ReflectedElementSupport::Vector3;
    if (p_type == NLS_TYPEOF(Maths::Vector4))
        return ReflectedElementSupport::Vector4;
    if (p_type == NLS_TYPEOF(Maths::Color))
        return ReflectedElementSupport::Color;
    if (IsSupportedIdentityObjectReferenceType(p_type))
        return ReflectedElementSupport::ObjectReference;
    if (IsReflectedValueElementType(p_type))
        return ReflectedElementSupport::ReflectedValue;
    return ReflectedElementSupport::Unsupported;
}

bool IsSupportedArrayElementType(const meta::Type& p_type)
{
    return GetArrayElementSupport(p_type) != ReflectedElementSupport::Unsupported;
}

std::optional<std::pair<float, float>> GetRangeLimits(const meta::Field& p_field)
{
    const auto* range = p_field.GetMeta().GetProperty<NLS::meta::Range>();
    if (range == nullptr)
        return std::nullopt;

    return std::make_pair((std::min)(range->min, range->max), (std::max)(range->min, range->max));
}

template <typename TValue, typename TWidget, typename TMinMax>
void DrawRangeSliderField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const std::string& p_label,
    TMinMax p_min,
    TMinMax p_max,
    const char* p_format,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    NLS::UI::GUIDrawer::CreateTitle(p_root, p_label);
    auto& widget = p_root.CreateWidget<TWidget>(
        static_cast<TValue>(p_min),
        static_cast<TValue>(p_max),
        GetFieldValue<TValue>(*p_instance, p_field),
        NLS::UI::Widgets::ESliderOrientation::HORIZONTAL,
        "",
        p_format);
    auto& dispatcher = widget.template AddPlugin<NLS::UI::DataDispatcher<TValue>>();
    dispatcher.RegisterGatherer([p_instance, p_field]() mutable
    {
        return GetFieldValue<TValue>(*p_instance, p_field);
    });
    dispatcher.RegisterProvider([p_instance, p_field, p_options](TValue p_value) mutable
    {
        SetFieldValue(*p_instance, p_field, p_value);
        NotifyChanged(p_options, p_field);
    });
}

std::optional<NLS::Engine::Serialize::InstanceID> GetIdentityObjectReferenceInstanceID(
    meta::Variant& p_instance,
    const meta::Field& p_field)
{
    const auto fieldType = p_field.GetType();
#define NLS_EDITOR_GET_PPTR_RESOURCE_INSTANCE_ID(type, label, artifactType, subAssetPrefix)        \
    if (fieldType == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))                               \
        return GetFieldValue<NLS::Engine::Serialize::PPtr<type>>(p_instance, p_field).GetInstanceID();
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_GET_PPTR_RESOURCE_INSTANCE_ID)
#undef NLS_EDITOR_GET_PPTR_RESOURCE_INSTANCE_ID
    return std::nullopt;
}

bool ClearIdentityObjectReferenceField(meta::Variant& p_instance, const meta::Field& p_field)
{
    const auto fieldType = p_field.GetType();
#define NLS_EDITOR_CLEAR_PPTR_RESOURCE_FIELD(type, label, artifactType, subAssetPrefix) \
    if (fieldType == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))         \
    {                                                                        \
        NLS::Engine::Serialize::PPtr<type> empty;                            \
        SetFieldValue(p_instance, p_field, empty);                           \
        return true;                                                         \
    }
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_CLEAR_PPTR_RESOURCE_FIELD)
#undef NLS_EDITOR_CLEAR_PPTR_RESOURCE_FIELD
    return false;
}

bool IsCompatibleArtifactType(
    const meta::Type& p_type,
    const NLS::Core::Assets::ArtifactType p_artifactType)
{
#define NLS_EDITOR_MATCH_PPTR_RESOURCE_ARTIFACT(type, label, artifactType, subAssetPrefix) \
    if (p_type == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))                          \
        return p_artifactType == artifactType;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_MATCH_PPTR_RESOURCE_ARTIFACT)
#undef NLS_EDITOR_MATCH_PPTR_RESOURCE_ARTIFACT

    return false;
}

std::string DefaultSubAssetKeyForObjectReferenceType(
    const meta::Type& p_type,
    const std::string& p_path)
{
    std::string stem = std::filesystem::path(p_path).stem().generic_string();
    if (stem.empty())
        stem = "Main";

#define NLS_EDITOR_MATCH_PPTR_RESOURCE_SUBASSET(type, label, artifactType, subAssetPrefix) \
    if (p_type == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))                          \
        return std::string(subAssetPrefix) + ":" + stem;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_MATCH_PPTR_RESOURCE_SUBASSET)
#undef NLS_EDITOR_MATCH_PPTR_RESOURCE_SUBASSET

    return {};
}

std::optional<NLS::Engine::Serialize::ObjectIdentifier> MakeObjectReferenceIdentifier(
    const meta::Type& p_type,
    const std::string& p_assetPath,
    const NLS::Guid& p_assetGuid,
    std::string p_subAssetKey,
    NLS::Core::Assets::ArtifactType p_artifactType)
{
    if (p_assetPath.empty() || !p_assetGuid.IsValid())
        return std::nullopt;

    if (!IsCompatibleArtifactType(p_type, p_artifactType))
        return std::nullopt;

    if (p_subAssetKey.empty())
        p_subAssetKey = DefaultSubAssetKeyForObjectReferenceType(p_type, p_assetPath);
    if (p_subAssetKey.empty())
        return std::nullopt;

    return NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(p_assetGuid),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(p_assetGuid, p_subAssetKey),
        p_assetPath);
}

std::optional<NLS::Engine::Serialize::ObjectIdentifier> MakeObjectReferenceIdentifier(
    const meta::Type& p_type,
    const NLS::Editor::Assets::EditorAssetDragPayload& p_payload)
{
    return MakeObjectReferenceIdentifier(
        p_type,
        NLS::Editor::Assets::GetEditorAssetDragPayloadPath(p_payload),
        NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(p_payload).GetGuid(),
        NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(p_payload),
        NLS::Editor::Assets::GetEditorAssetDragPayloadArtifactType(p_payload));
}

bool SetIdentityObjectReferenceField(
    meta::Variant& p_instance,
    const meta::Field& p_field,
    const NLS::Engine::Serialize::ObjectIdentifier& p_identifier)
{
    const auto fieldType = p_field.GetType();
    const auto instanceID =
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(p_identifier);
    if (instanceID == NLS::Engine::Serialize::InstanceID_None)
        return false;

#define NLS_EDITOR_SET_PPTR_RESOURCE_FIELD(type, label, artifactType, subAssetPrefix) \
    if (fieldType == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))       \
    {                                                                      \
        NLS::Engine::Serialize::PPtr<type> value(instanceID);              \
        SetFieldValue(p_instance, p_field, value);                         \
        return true;                                                       \
    }
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_SET_PPTR_RESOURCE_FIELD)
#undef NLS_EDITOR_SET_PPTR_RESOURCE_FIELD
    return false;
}

bool SetIdentityObjectReferenceArrayElement(
    meta::ArrayWrapper& p_array,
    const meta::Type& p_elementType,
    size_t p_index,
    const NLS::Engine::Serialize::ObjectIdentifier& p_identifier)
{
    if (p_index >= p_array.Size())
        return false;

    const auto instanceID =
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(p_identifier);
    if (instanceID == NLS::Engine::Serialize::InstanceID_None)
        return false;

#define NLS_EDITOR_SET_PPTR_RESOURCE_ARRAY_ELEMENT(type, label, artifactType, subAssetPrefix) \
    if (p_elementType == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))           \
    {                                                                              \
        NLS::Engine::Serialize::PPtr<type> value(instanceID);                      \
        p_array.SetValue(p_index, value);                                          \
        return true;                                                               \
    }
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_SET_PPTR_RESOURCE_ARRAY_ELEMENT)
#undef NLS_EDITOR_SET_PPTR_RESOURCE_ARRAY_ELEMENT
    return false;
}

NLS::Engine::Serialize::InstanceID VariantToIdentityObjectReferenceInstanceID(
    const meta::Variant& p_value,
    const meta::Type& p_type)
{
#define NLS_EDITOR_VARIANT_TO_PPTR_INSTANCE_ID(type, label, artifactType, subAssetPrefix)        \
    if (p_type == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))                                \
        return p_value.GetValue<NLS::Engine::Serialize::PPtr<type>>().GetInstanceID();
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_VARIANT_TO_PPTR_INSTANCE_ID)
#undef NLS_EDITOR_VARIANT_TO_PPTR_INSTANCE_ID
    return NLS::Engine::Serialize::InstanceID_None;
}

std::string ObjectReferenceTypeLabel(const meta::Type& p_type)
{
#define NLS_EDITOR_LABEL_PPTR_RESOURCE_TYPE(type, label, artifactType, subAssetPrefix) \
    if (p_type == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>)) return label;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_LABEL_PPTR_RESOURCE_TYPE)
#undef NLS_EDITOR_LABEL_PPTR_RESOURCE_TYPE
    return p_type.GetName();
}

template <typename T>
std::string RawPointerObjectReferenceDisplay(T* p_value, const char* p_label)
{
    return p_value != nullptr
        ? std::string("Assigned (") + p_label + ")"
        : std::string("Empty (") + p_label + ")";
}

std::string RawObjectPointerReferenceDisplay(NLS::Object* p_value, const meta::Type& p_type)
{
    const auto labelType = p_type.GetDecayedType();
    auto label = labelType.IsValid() ? labelType.GetName() : p_type.GetName();
    const auto namespaceEnd = label.rfind("::");
    if (namespaceEnd != std::string::npos)
        label.erase(0, namespaceEnd + 2u);
    return p_value != nullptr
        ? "Assigned (" + label + ")"
        : "Empty (" + label + ")";
}

std::string BuildObjectReferenceDisplay(meta::Variant& p_instance, const meta::Field& p_field)
{
    const auto fieldType = p_field.GetType();
    if (IsIdentityObjectReferenceType(fieldType))
    {
        const auto typeName = ObjectReferenceTypeLabel(fieldType);
        const auto instanceID = GetIdentityObjectReferenceInstanceID(p_instance, p_field)
            .value_or(NLS::Engine::Serialize::InstanceID_None);

        NLS::Engine::Serialize::ObjectIdentifier value;
        if (instanceID == NLS::Engine::Serialize::InstanceID_None ||
            !NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(instanceID, value) ||
            !value.IsValid())
            return "Empty (" + typeName + ")";
        if (value.guid.IsValid())
        {
            const auto label = !value.filePath.empty() ? value.filePath : value.guid.ToString();
            return label + " (" + typeName + ")";
        }
        return "fileID " + std::to_string(value.localIdentifierInFile) + " (" + typeName + ")";
    }

    if (IsGameObjectPointerType(fieldType) || IsComponentPointerType(fieldType))
        return RawObjectPointerReferenceDisplay(GetFieldValue<NLS::Object*>(p_instance, p_field), fieldType);

    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Texture2D*))
        return RawPointerObjectReferenceDisplay(GetFieldValue<NLS::Render::Resources::Texture2D*>(p_instance, p_field), "Texture2D");
    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Mesh*))
        return RawPointerObjectReferenceDisplay(GetFieldValue<NLS::Render::Resources::Mesh*>(p_instance, p_field), "Mesh");
    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Shader*))
        return RawPointerObjectReferenceDisplay(GetFieldValue<NLS::Render::Resources::Shader*>(p_instance, p_field), "Shader");
    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Material*))
        return RawPointerObjectReferenceDisplay(GetFieldValue<NLS::Render::Resources::Material*>(p_instance, p_field), "Material");

    return "Empty (" + fieldType.GetName() + ")";
}

bool SetSceneObjectReferenceField(
    meta::Variant& p_instance,
    const meta::Field& p_field,
    NLS::Engine::GameObject* p_gameObject)
{
    if (p_gameObject == nullptr)
        return false;

    const auto fieldType = p_field.GetType();
    if (IsGameObjectPointerType(fieldType))
    {
        auto* object = static_cast<NLS::Object*>(p_gameObject);
        auto value = ObjectVariant(object);
        return p_field.SetValue(p_instance, value);
    }

    if (fieldType.GetDecayedType() == NLS_TYPEOF(NLS::Engine::Components::Component))
    {
        NLS::Engine::Components::Component* component = nullptr;
        for (const auto& entry : p_gameObject->GetComponents())
        {
            if (!entry || entry.get() == p_gameObject->GetTransform())
                continue;

            component = entry.get();
            break;
        }

        if (component == nullptr)
            component = p_gameObject->GetTransform();
        if (component == nullptr)
            return false;

        auto* object = static_cast<NLS::Object*>(component);
        auto value = ObjectVariant(object);
        return p_field.SetValue(p_instance, value);
    }

    if (IsComponentPointerType(fieldType))
    {
        const auto componentType = fieldType.GetDecayedType();

        auto* component = p_gameObject->GetComponent(componentType, true);
        if (component == nullptr)
            return false;

        auto* object = static_cast<NLS::Object*>(component);
        auto value = ObjectVariant(object);
        return p_field.SetValue(p_instance, value);
    }

    return false;
}

void ClearObjectReferenceField(meta::Variant& p_instance, const meta::Field& p_field)
{
    const auto fieldType = p_field.GetType();
    if (IsIdentityObjectReferenceType(fieldType))
    {
        ClearIdentityObjectReferenceField(p_instance, p_field);
        return;
    }

    if (IsSceneObjectReferenceType(fieldType))
    {
        NLS::Object* empty = nullptr;
        auto value = ObjectVariant(empty);
        p_field.SetValue(p_instance, value);
        return;
    }

    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Texture2D*))
    {
        NLS::Render::Resources::Texture2D* empty = nullptr;
        SetFieldValue(p_instance, p_field, empty);
        return;
    }

    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Mesh*))
    {
        NLS::Render::Resources::Mesh* empty = nullptr;
        SetFieldValue(p_instance, p_field, empty);
        return;
    }

    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Shader*))
    {
        NLS::Render::Resources::Shader* empty = nullptr;
        SetFieldValue(p_instance, p_field, empty);
        return;
    }

    if (fieldType == NLS_TYPEOF(NLS::Render::Resources::Material*))
    {
        NLS::Render::Resources::Material* empty = nullptr;
        SetFieldValue(p_instance, p_field, empty);
    }
}

void AddAssetObjectReferenceTarget(
    NLS::UI::Widgets::AWidget& p_widget,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    p_widget.AddPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
        NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent +=
        [p_instance, p_field, p_options](NLS::Editor::Assets::EditorAssetDragPayload p_payload) mutable
    {
        const auto identifier = MakeObjectReferenceIdentifier(p_field.GetType(), p_payload);
        if (!identifier.has_value() || !SetIdentityObjectReferenceField(*p_instance, p_field, *identifier))
            return;

        NotifyChanged(p_options, p_field);
    };
}

void AddSceneObjectReferenceTarget(
    NLS::UI::Widgets::AWidget& p_widget,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    p_widget.AddPlugin<NLS::UI::DDTarget<std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*>>>(
        "GameObject").DataReceivedEvent +=
        [p_instance, p_field, p_options](
            std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*> p_payload) mutable
    {
        if (!SetSceneObjectReferenceField(*p_instance, p_field, p_payload.first))
            return;

        NotifyChanged(p_options, p_field);
    };
}

void DrawObjectReferenceField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = BuildFieldLabel(p_field, p_options);
    NLS::UI::GUIDrawer::CreateTitle(p_root, label);

    auto& rightSide = p_root.CreateWidget<NLS::UI::Widgets::Group>();
    auto& display = rightSide.CreateWidget<NLS::UI::Widgets::Text>(
        BuildObjectReferenceDisplay(*p_instance, p_field));
    display.lineBreak = false;
    auto& displayDispatcher = display.AddPlugin<NLS::UI::DataDispatcher<std::string>>();
    displayDispatcher.RegisterGatherer([p_instance, p_field]() mutable
    {
        return BuildObjectReferenceDisplay(*p_instance, p_field);
    });

    if (IsSupportedIdentityObjectReferenceType(p_field.GetType()))
        AddAssetObjectReferenceTarget(display, p_instance, p_field, p_options);
    else if (IsSceneObjectReferenceType(p_field.GetType()))
        AddSceneObjectReferenceTarget(display, p_instance, p_field, p_options);

    auto& pickerButton = rightSide.CreateWidget<NLS::UI::Widgets::ButtonSmall>("o");
    pickerButton.lineBreak = false;
    pickerButton.idleBackgroundColor = {0.18f, 0.19f, 0.20f};
    pickerButton.hoveredBackgroundColor = {0.25f, 0.26f, 0.28f};
    pickerButton.clickedBackgroundColor = {0.31f, 0.33f, 0.36f};
    if (IsSupportedIdentityObjectReferenceType(p_field.GetType()))
    {
        auto& pickerPlugin = pickerButton.AddPlugin<ObjectReferencePickerPlugin>(p_instance, p_field, p_options);
        pickerButton.ClickedEvent += [&pickerPlugin]()
        {
            pickerPlugin.RequestOpen();
        };
        AddAssetObjectReferenceTarget(pickerButton, p_instance, p_field, p_options);
    }
    else if (IsSceneObjectReferenceType(p_field.GetType()))
        AddSceneObjectReferenceTarget(pickerButton, p_instance, p_field, p_options);

    auto& clearButton = rightSide.CreateWidget<NLS::UI::Widgets::ButtonSmall>("x");
    clearButton.idleBackgroundColor = NLS::UI::GUIDrawer::ClearButtonColor;
    clearButton.ClickedEvent += [p_instance, p_field, p_options]() mutable
    {
        ClearObjectReferenceField(*p_instance, p_field);
        NotifyChanged(p_options, p_field);
    };
}

std::string BuildObjectIdentifierDisplay(const NLS::Engine::Serialize::ObjectIdentifier& value)
{
    if (!value.IsValid())
        return "Empty (Object)";
    if (value.guid.IsValid())
    {
        const auto label = !value.filePath.empty() ? value.filePath : value.guid.ToString();
        return label + " (Object)";
    }
    return "fileID " + std::to_string(value.localIdentifierInFile) + " (Object)";
}

std::string BuildArrayObjectReferenceElementDisplay(
    const meta::ArrayWrapper& p_array,
    const meta::Type& p_elementType,
    size_t p_index)
{
    if (p_index >= p_array.Size())
        return "Empty (Object)";

    const auto value = p_array.GetValue(p_index);
    const auto instanceID = VariantToIdentityObjectReferenceInstanceID(value, p_elementType);
    NLS::Engine::Serialize::ObjectIdentifier identifier;
    NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(instanceID, identifier);
    return BuildObjectIdentifierDisplay(identifier);
}

meta::Variant MakeDefaultArrayElementValue(const meta::Type& p_elementType)
{
    if (p_elementType == NLS_TYPEOF(std::string))
        return std::string {};
    if (p_elementType == NLS_TYPEOF(float))
        return 0.0f;
    if (p_elementType == NLS_TYPEOF(int))
        return 0;
    if (p_elementType == NLS_TYPEOF(bool))
        return false;
    if (p_elementType == NLS_TYPEOF(Maths::Vector2))
        return Maths::Vector2 {};
    if (p_elementType == NLS_TYPEOF(Maths::Vector3))
        return Maths::Vector3 {};
    if (p_elementType == NLS_TYPEOF(Maths::Vector4))
        return Maths::Vector4 {};
    if (p_elementType == NLS_TYPEOF(Maths::Color))
        return Maths::Color {};
#define NLS_EDITOR_DEFAULT_PPTR_RESOURCE_ELEMENT(type, label, artifactType, subAssetPrefix) \
    if (p_elementType == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>)) \
        return NLS::Engine::Serialize::PPtr<type> {};
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_DEFAULT_PPTR_RESOURCE_ELEMENT)
#undef NLS_EDITOR_DEFAULT_PPTR_RESOURCE_ELEMENT
    return {};
}

void ResizeArrayWrapper(meta::ArrayWrapper& p_array, const meta::Type& p_elementType, size_t p_size)
{
    while (p_array.Size() > p_size && p_array.CanRemove())
        p_array.Remove(p_array.Size() - 1);

    if (p_array.CanResize())
    {
        p_array.Resize(p_size);
        return;
    }

    while (p_array.Size() < p_size && p_array.CanInsertDefault())
    {
        p_array.InsertDefault(p_array.Size());
    }

    while (p_array.Size() < p_size && p_array.CanInsert())
    {
        auto value = MakeDefaultArrayElementValue(p_elementType);
        if (!value.IsValid())
            break;
        p_array.Insert(p_array.Size(), value);
    }
}

meta::Variant ReadArrayFieldValue(
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field)
{
    return p_field.GetValue(*p_instance);
}

void WriteArrayFieldValue(
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    meta::Variant& p_value,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    if (p_field.SetValue(*p_instance, p_value))
        NotifyChanged(p_options, p_field);
}

std::shared_ptr<ReflectedArrayFieldState> MakeArrayFieldState(
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options,
    int p_depth = 0,
    std::vector<meta::Type> p_typeStack = {})
{
    return std::make_shared<ReflectedArrayFieldState>(
        p_instance,
        p_field,
        p_field.GetType().GetArrayType(),
        p_options,
        ReadArrayFieldValue(p_instance, p_field),
        p_depth,
        std::move(p_typeStack));
}

void WriteArrayFieldValue(const std::shared_ptr<ReflectedArrayFieldState>& p_state)
{
    if (p_state && p_state->field.SetValue(*p_state->instance, p_state->value))
        NotifyChanged(p_state->options, p_state->field);
}

void RefreshArrayFieldState(const std::shared_ptr<ReflectedArrayFieldState>& p_state)
{
    if (!p_state)
        return;

    p_state->value = ReadArrayFieldValue(p_state->instance, p_state->field);
    p_state->array = p_state->value.GetArray();
}

void DrawArrayObjectReferenceElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    auto& rightSide = p_root.CreateWidget<NLS::UI::Widgets::Group>();
    auto& display = rightSide.CreateWidget<NLS::UI::Widgets::Text>(
        BuildArrayObjectReferenceElementDisplay(p_state->array, p_state->elementType, p_index));
    display.lineBreak = false;
    auto& displayDispatcher = display.AddPlugin<NLS::UI::DataDispatcher<std::string>>();
    displayDispatcher.RegisterGatherer([p_state, p_index]() mutable
    {
        return BuildArrayObjectReferenceElementDisplay(p_state->array, p_state->elementType, p_index);
    });

    display.AddPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
        NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent +=
        [p_state, p_index](NLS::Editor::Assets::EditorAssetDragPayload p_payload) mutable
    {
        const auto identifier = MakeObjectReferenceIdentifier(p_state->elementType, p_payload);
        if (!identifier.has_value())
            return;

        if (!SetIdentityObjectReferenceArrayElement(p_state->array, p_state->elementType, p_index, *identifier))
            return;

        WriteArrayFieldValue(p_state);
    };

    auto& clearButton = rightSide.CreateWidget<NLS::UI::Widgets::ButtonSmall>("Clear");
    clearButton.idleBackgroundColor = NLS::UI::GUIDrawer::ClearButtonColor;
    clearButton.ClickedEvent += [p_state, p_index]() mutable
    {
        if (p_index >= p_state->array.Size())
            return;

#define NLS_EDITOR_CLEAR_PPTR_RESOURCE_ARRAY_ELEMENT(type, label, artifactType, subAssetPrefix) \
        if (p_state->elementType == NLS_TYPEOF(NLS::Engine::Serialize::PPtr<type>))             \
        {                                                                                       \
            NLS::Engine::Serialize::PPtr<type> empty;                                           \
            p_state->array.SetValue(p_index, empty);                                            \
            WriteArrayFieldValue(p_state);                                                      \
            return;                                                                             \
        }
        NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_EDITOR_CLEAR_PPTR_RESOURCE_ARRAY_ELEMENT)
#undef NLS_EDITOR_CLEAR_PPTR_RESOURCE_ARRAY_ELEMENT
    };
}

template <typename TValue>
TValue GetArrayValueOrDefault(
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index,
    TValue p_default = {})
{
    return p_state && p_index < p_state->array.Size()
        ? p_state->array.GetValue(p_index).GetValue<TValue>()
        : p_default;
}

template <typename TValue>
void SetArrayElementValue(
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index,
    const TValue& p_value)
{
    if (p_state && p_index < p_state->array.Size() && p_state->array.CanSetValue())
    {
        p_state->array.SetValue(p_index, p_value);
        WriteArrayFieldValue(p_state);
    }
}

void DrawStringArrayElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index)
{
    auto& widget = p_root.CreateWidget<NLS::UI::Widgets::InputText>("");
    auto& dispatcher = widget.AddPlugin<NLS::UI::DataDispatcher<std::string>>();
    dispatcher.RegisterGatherer([p_state, p_index]() mutable
    {
        return GetArrayValueOrDefault<std::string>(p_state, p_index);
    });
    dispatcher.RegisterProvider([p_state, p_index](std::string p_value) mutable
    {
        SetArrayElementValue(p_state, p_index, p_value);
    });
}

template <typename TValue>
void DrawScalarArrayElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index,
    float p_step = 1.0f,
    TValue p_min = std::numeric_limits<TValue>::min(),
    TValue p_max = std::numeric_limits<TValue>::max())
{
    NLS::UI::GUIDrawer::DrawScalar<TValue>(
        p_root,
        "",
        [p_state, p_index]() mutable
        {
            return GetArrayValueOrDefault<TValue>(p_state, p_index);
        },
        [p_state, p_index](TValue p_value) mutable
        {
            SetArrayElementValue(p_state, p_index, p_value);
        },
        p_step,
        p_min,
        p_max);
}

void DrawBoolArrayElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index)
{
    auto& widget = p_root.CreateWidget<NLS::UI::Widgets::CheckBox>(false);
    auto& dispatcher = widget.AddPlugin<NLS::UI::DataDispatcher<bool>>();
    dispatcher.RegisterGatherer([p_state, p_index]() mutable
    {
        return GetArrayValueOrDefault<bool>(p_state, p_index);
    });
    dispatcher.RegisterProvider([p_state, p_index](bool p_value) mutable
    {
        SetArrayElementValue(p_state, p_index, p_value);
    });
}

template <size_t Size, typename TValue, typename TToArray, typename TFromArray>
void DrawVectorArrayElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index,
    TToArray p_toArray,
    TFromArray p_fromArray)
{
    auto& widget = p_root.CreateWidget<NLS::UI::Widgets::DragMultipleScalars<float, Size>>(
        NLS::UI::GUIDrawer::GetDataType<float>(),
        NLS::UI::GUIDrawer::_MIN_FLOAT,
        NLS::UI::GUIDrawer::_MAX_FLOAT,
        0.0f,
        0.01f,
        "",
        NLS::UI::GUIDrawer::GetFormat<float>());
    auto& dispatcher = widget.template AddPlugin<NLS::UI::DataDispatcher<std::array<float, Size>>>();
    dispatcher.RegisterGatherer([p_state, p_index, p_toArray]() mutable
    {
        return p_toArray(GetArrayValueOrDefault<TValue>(p_state, p_index));
    });
    dispatcher.RegisterProvider([p_state, p_index, p_fromArray](std::array<float, Size> p_value) mutable
    {
        SetArrayElementValue(p_state, p_index, p_fromArray(p_value));
    });
}

void DrawColorArrayElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index)
{
    auto& widget = p_root.CreateWidget<NLS::UI::Widgets::ColorEdit>(true);
    auto& dispatcher = widget.AddPlugin<NLS::UI::DataDispatcher<Maths::Color>>();
    dispatcher.RegisterGatherer([p_state, p_index]() mutable
    {
        return GetArrayValueOrDefault<Maths::Color>(p_state, p_index);
    });
    dispatcher.RegisterProvider([p_state, p_index](Maths::Color p_value) mutable
    {
        SetArrayElementValue(p_state, p_index, p_value);
    });
}

bool CommitArrayElementValue(
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index,
    const ReflectedVariantHandle& p_elementValue)
{
    if (!p_state || !p_elementValue || p_index >= p_state->array.Size() || !p_state->array.CanSetValue())
        return false;

    p_state->array.SetValue(p_index, *p_elementValue);
    WriteArrayFieldValue(p_state);
    return true;
}

bool DrawReflectedValueField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_value,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options,
    int p_depth,
    std::vector<meta::Type> p_typeStack,
    std::function<void()> p_commit)
{
    if (!p_value || !p_field.IsValid() ||
        !NLS::Editor::Panels::ReflectedFieldMatchesSearch(p_field, p_options.searchText))
        return false;

    const auto fieldType = p_field.GetType();
    const auto label = NLS::Editor::Panels::FormatReflectedFieldLabel(p_field.GetName());
    if (fieldType == NLS_TYPEOF(int))
    {
        NLS::UI::GUIDrawer::DrawScalar<int>(
            p_root,
            label,
            [p_value, p_field]() mutable
            {
                return p_field.GetValue(*p_value).GetValue<int>();
            },
            [p_value, p_field, p_commit](int p_newValue) mutable
            {
                SetFieldValue(*p_value, p_field, p_newValue);
                if (p_commit)
                    p_commit();
            });
        return true;
    }

    if (fieldType == NLS_TYPEOF(float))
    {
        NLS::UI::GUIDrawer::DrawScalar<float>(
            p_root,
            label,
            [p_value, p_field]() mutable
            {
                return p_field.GetValue(*p_value).GetValue<float>();
            },
            [p_value, p_field, p_commit](float p_newValue) mutable
            {
                SetFieldValue(*p_value, p_field, p_newValue);
                if (p_commit)
                    p_commit();
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
        return true;
    }

    if (fieldType.IsArray())
    {
        p_root.CreateWidget<NLS::UI::Widgets::Text>(label + " (recursive unsupported)");
        p_root.CreateWidget<NLS::UI::Widgets::Text>(fieldType.GetName());
        return true;
    }

    if (IsReflectedValueElementType(fieldType))
    {
        if (p_depth >= kMaxReflectedValueDepth ||
            std::find(p_typeStack.begin(), p_typeStack.end(), fieldType) != p_typeStack.end())
        {
            p_root.CreateWidget<NLS::UI::Widgets::Text>(label + " (recursive unsupported)");
            p_root.CreateWidget<NLS::UI::Widgets::Text>(fieldType.GetName());
            return true;
        }

        auto nestedValue = std::make_shared<meta::Variant>(p_field.GetValue(*p_value));
        auto& foldout = p_root.CreateWidget<NLS::UI::Widgets::TreeNode>(label, true);
        foldout.Open();

        auto nestedStack = std::move(p_typeStack);
        nestedStack.push_back(fieldType);
        auto nestedCommit = [p_value, p_field, nestedValue, p_commit]() mutable
        {
            p_field.SetValue(*p_value, *nestedValue);
            if (p_commit)
                p_commit();
        };

        for (const auto& childField : fieldType.GetFields())
            DrawReflectedValueField(
                foldout,
                nestedValue,
                childField,
                p_options,
                p_depth + 1,
                nestedStack,
                nestedCommit);
        return true;
    }

    p_root.CreateWidget<NLS::UI::Widgets::Text>(label + " (unsupported)");
    p_root.CreateWidget<NLS::UI::Widgets::Text>(fieldType.GetName());
    return true;
}

void DrawReflectedValueArrayElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index)
{
    if (!p_state || p_index >= p_state->array.Size())
        return;

    const auto typeAlreadyInStack =
        std::find(p_state->typeStack.begin(), p_state->typeStack.end(), p_state->elementType) != p_state->typeStack.end();
    if (p_state->depth >= kMaxReflectedValueDepth || typeAlreadyInStack)
    {
        p_root.CreateWidget<NLS::UI::Widgets::Text>(p_state->elementType.GetName() + " (recursive unsupported)");
        return;
    }

    auto elementValue = std::make_shared<meta::Variant>(p_state->array.GetValue(p_index));
    auto& foldout = p_root.CreateWidget<NLS::UI::Widgets::TreeNode>(
        "Element " + std::to_string(p_index),
        true);
    foldout.Open();

    auto childStack = p_state->typeStack;
    childStack.push_back(p_state->elementType);
    auto commitElement = [p_state, p_index, elementValue]() mutable
    {
        CommitArrayElementValue(p_state, p_index, elementValue);
    };

    for (const auto& childField : p_state->elementType.GetFields())
        DrawReflectedValueField(
            foldout,
            elementValue,
            childField,
            p_state->options,
            p_state->depth + 1,
            childStack,
            commitElement);
}

void DrawArrayElement(
    NLS::UI::Internal::WidgetContainer& p_root,
    const std::shared_ptr<ReflectedArrayFieldState>& p_state,
    size_t p_index,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto& p_elementType = p_state->elementType;
    const auto elementSupport = GetArrayElementSupport(p_elementType);

    if (elementSupport == ReflectedElementSupport::ReflectedValue)
    {
        DrawReflectedValueArrayElement(p_root, p_state, p_index);
        return;
    }

    const auto elementLabel = "Element " + std::to_string(p_index);
    NLS::UI::GUIDrawer::CreateTitle(p_root, elementLabel);

    if (elementSupport == ReflectedElementSupport::String)
    {
        DrawStringArrayElement(p_root, p_state, p_index);
        return;
    }
    if (elementSupport == ReflectedElementSupport::Float)
    {
        DrawScalarArrayElement<float>(
            p_root,
            p_state,
            p_index,
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
        return;
    }
    if (elementSupport == ReflectedElementSupport::Int)
    {
        DrawScalarArrayElement<int>(p_root, p_state, p_index);
        return;
    }
    if (elementSupport == ReflectedElementSupport::Bool)
    {
        DrawBoolArrayElement(p_root, p_state, p_index);
        return;
    }
    if (elementSupport == ReflectedElementSupport::Vector2)
    {
        DrawVectorArrayElement<2, Maths::Vector2>(
            p_root,
            p_state,
            p_index,
            [](Maths::Vector2 p_value) { return std::array<float, 2> {p_value.x, p_value.y}; },
            [](const std::array<float, 2>& p_value) { return Maths::Vector2 {p_value[0], p_value[1]}; });
        return;
    }
    if (elementSupport == ReflectedElementSupport::Vector3)
    {
        DrawVectorArrayElement<3, Maths::Vector3>(
            p_root,
            p_state,
            p_index,
            [](Maths::Vector3 p_value) { return std::array<float, 3> {p_value.x, p_value.y, p_value.z}; },
            [](const std::array<float, 3>& p_value) { return Maths::Vector3 {p_value[0], p_value[1], p_value[2]}; });
        return;
    }
    if (elementSupport == ReflectedElementSupport::Vector4)
    {
        DrawVectorArrayElement<4, Maths::Vector4>(
            p_root,
            p_state,
            p_index,
            [](Maths::Vector4 p_value) { return std::array<float, 4> {p_value.x, p_value.y, p_value.z, p_value.w}; },
            [](const std::array<float, 4>& p_value) { return Maths::Vector4 {p_value[0], p_value[1], p_value[2], p_value[3]}; });
        return;
    }
    if (elementSupport == ReflectedElementSupport::Color)
    {
        DrawColorArrayElement(p_root, p_state, p_index);
        return;
    }
    if (elementSupport == ReflectedElementSupport::ObjectReference)
    {
        DrawArrayObjectReferenceElement(p_root, p_state, p_index, p_options);
        return;
    }

    p_root.CreateWidget<NLS::UI::Widgets::Text>(p_elementType.GetName() + " (unsupported)");
}

void DrawGenericArrayField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = BuildFieldLabel(p_field, p_options);
    const auto elementType = p_field.GetType().GetArrayType();
    NLS::UI::GUIDrawer::CreateTitle(p_root, label);

    auto& rightSide = p_root.CreateWidget<NLS::UI::Widgets::Group>();
    auto& foldout = rightSide.CreateWidget<NLS::UI::Widgets::TreeNode>("Array", true);
    foldout.Open();

    foldout.CreateWidget<NLS::UI::Widgets::Text>("Size");
    auto& sizeWidget = foldout.CreateWidget<NLS::UI::Widgets::DragSingleScalar<int>>(
        NLS::UI::GUIDrawer::GetDataType<int>(),
        0,
        kMaxInspectorArraySize,
        0,
        1.0f,
        "",
        "%d");
    auto& sizeDispatcher = sizeWidget.AddPlugin<NLS::UI::DataDispatcher<int>>();
    auto state = MakeArrayFieldState(p_instance, p_field, p_options);
    sizeDispatcher.RegisterGatherer([state]() mutable
    {
        return static_cast<int>(state->array.Size());
    });
    sizeDispatcher.RegisterProvider([state](int p_size) mutable
    {
        RefreshArrayFieldState(state);
        const auto previousSize = state->array.Size();
        const auto clampedSize = static_cast<size_t>(std::clamp(p_size, 0, kMaxInspectorArraySize));
        ResizeArrayWrapper(state->array, state->elementType, clampedSize);
        WriteArrayFieldValue(state);
        if (state->array.Size() != previousSize)
            NotifyLayoutChanged(state->options, state->field);
    });

    const auto size = state->array.Size();
    for (size_t index = 0; index < size; ++index)
        DrawArrayElement(foldout, state, index, p_options);

    auto& addButton = foldout.CreateWidget<NLS::UI::Widgets::ButtonSmall>("Add");
    addButton.ClickedEvent += [state]() mutable
    {
        RefreshArrayFieldState(state);
        if (state->array.Size() >= static_cast<size_t>(kMaxInspectorArraySize))
            return;
        ResizeArrayWrapper(state->array, state->elementType, state->array.Size() + 1);
        WriteArrayFieldValue(state);
        NotifyLayoutChanged(state->options, state->field);
    };

    auto& removeButton = foldout.CreateWidget<NLS::UI::Widgets::ButtonSmall>("Remove");
    removeButton.ClickedEvent += [state]() mutable
    {
        RefreshArrayFieldState(state);
        if (state->array.Size() > 0)
        {
            state->array.Remove(state->array.Size() - 1);
            WriteArrayFieldValue(state);
            NotifyLayoutChanged(state->options, state->field);
        }
    };
}

void DrawBoundingSphereField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = BuildFieldLabel(p_field, p_options);

    NLS::UI::GUIDrawer::DrawVec3(
        p_root,
        label + " Position",
        [p_instance, p_field]() mutable
        {
            return GetFieldValue<Render::Geometry::BoundingSphere>(*p_instance, p_field).position;
        },
        [p_instance, p_field, p_options](Maths::Vector3 p_value) mutable
        {
            auto sphere = GetFieldValue<Render::Geometry::BoundingSphere>(*p_instance, p_field);
            sphere.position = p_value;
            SetFieldValue(*p_instance, p_field, sphere);
            NotifyChanged(p_options, p_field);
        },
        0.01f,
        NLS::UI::GUIDrawer::_MIN_FLOAT,
        NLS::UI::GUIDrawer::_MAX_FLOAT);

    NLS::UI::GUIDrawer::DrawScalar<float>(
        p_root,
        label + " Radius",
        [p_instance, p_field]() mutable
        {
            return GetFieldValue<Render::Geometry::BoundingSphere>(*p_instance, p_field).radius;
        },
        [p_instance, p_field, p_options](float p_value) mutable
        {
            auto sphere = GetFieldValue<Render::Geometry::BoundingSphere>(*p_instance, p_field);
            sphere.radius = p_value;
            SetFieldValue(*p_instance, p_field, sphere);
            NotifyChanged(p_options, p_field);
        },
        0.01f,
        0.0f,
        NLS::UI::GUIDrawer::_MAX_FLOAT);
}

void DrawRectField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = BuildFieldLabel(p_field, p_options);
    NLS::UI::GUIDrawer::DrawVec4(
        p_root,
        label,
        [p_instance, p_field]() mutable
        {
            const auto rect = GetFieldValue<Maths::Rect>(*p_instance, p_field);
            return Maths::Vector4(rect.x, rect.y, rect.width, rect.height);
        },
        [p_instance, p_field, p_options](Maths::Vector4 p_value) mutable
        {
            Maths::Rect rect {p_value.x, p_value.y, p_value.z, p_value.w};
            SetFieldValue(*p_instance, p_field, rect);
            NotifyChanged(p_options, p_field);
        },
        0.01f,
        NLS::UI::GUIDrawer::_MIN_FLOAT,
        NLS::UI::GUIDrawer::_MAX_FLOAT);
}

void DrawBoundsField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = BuildFieldLabel(p_field, p_options);
    NLS::UI::GUIDrawer::DrawVec3(
        p_root,
        label + " Center",
        [p_instance, p_field]() mutable
        {
            return GetFieldValue<Render::Geometry::Bounds>(*p_instance, p_field).center;
        },
        [p_instance, p_field, p_options](Maths::Vector3 p_value) mutable
        {
            auto bounds = GetFieldValue<Render::Geometry::Bounds>(*p_instance, p_field);
            bounds.center = p_value;
            SetFieldValue(*p_instance, p_field, bounds);
            NotifyChanged(p_options, p_field);
        },
        0.01f,
        NLS::UI::GUIDrawer::_MIN_FLOAT,
        NLS::UI::GUIDrawer::_MAX_FLOAT);

    NLS::UI::GUIDrawer::DrawVec3(
        p_root,
        label + " Extents",
        [p_instance, p_field]() mutable
        {
            const auto size = GetFieldValue<Render::Geometry::Bounds>(*p_instance, p_field).size;
            return Maths::Vector3 {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
        },
        [p_instance, p_field, p_options](Maths::Vector3 p_value) mutable
        {
            auto bounds = GetFieldValue<Render::Geometry::Bounds>(*p_instance, p_field);
            bounds.size = Maths::Vector3 {p_value.x * 2.0f, p_value.y * 2.0f, p_value.z * 2.0f};
            SetFieldValue(*p_instance, p_field, bounds);
            NotifyChanged(p_options, p_field);
        },
        0.01f,
        NLS::UI::GUIDrawer::_MIN_FLOAT,
        NLS::UI::GUIDrawer::_MAX_FLOAT);
}

void DrawLayerMaskField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = BuildFieldLabel(p_field, p_options);
    NLS::UI::GUIDrawer::CreateTitle(p_root, label);

    auto& layerGroup = p_root.CreateWidget<NLS::UI::Widgets::Group>();
    const auto& layers = NLS::Editor::Settings::TagLayerSettings::GetLayers();
    for (size_t index = 0; index < layers.size(); ++index)
    {
        const auto layerName = layers[index].empty()
            ? "Layer " + std::to_string(index)
            : layers[index];

        auto& toggle = layerGroup.CreateWidget<NLS::UI::Widgets::CheckBox>(
            GetFieldValue<Engine::LayerMask>(*p_instance, p_field).ContainsLayer(static_cast<int>(index)),
            std::to_string(index) + ": " + layerName);
        auto& dispatcher = toggle.AddPlugin<NLS::UI::DataDispatcher<bool>>();
        dispatcher.RegisterGatherer([p_instance, p_field, index]() mutable
        {
            return GetFieldValue<Engine::LayerMask>(*p_instance, p_field).ContainsLayer(static_cast<int>(index));
        });
        dispatcher.RegisterProvider([p_instance, p_field, p_options, index](bool p_enabled) mutable
        {
            auto mask = GetFieldValue<Engine::LayerMask>(*p_instance, p_field).GetMask();
            const uint32_t bit = 1u << static_cast<uint32_t>(index);
            mask = p_enabled ? (mask | bit) : (mask & ~bit);
            SetFieldValue(*p_instance, p_field, Engine::LayerMask {mask});
            NotifyChanged(p_options, p_field);
        });
    }
}

void DrawEnumField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const std::map<int, std::string>& p_choices,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = BuildFieldLabel(p_field, p_options);
    NLS::UI::GUIDrawer::CreateTitle(p_root, label);
    auto& combo = p_root.CreateWidget<NLS::UI::Widgets::ComboBox>(GetEnumFieldValue(*p_instance, p_field));
    combo.choices = p_choices;
    combo.ValueChangedEvent += [p_instance, p_field, p_options](int p_value) mutable
    {
        SetEnumFieldValue(*p_instance, p_field, p_value);
        NotifyChanged(p_options, p_field);
    };
}

bool DrawReflectedFieldInternal(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options);

int DrawReflectedObjectInternal(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options);
}

namespace NLS::Editor::Panels
{
std::string FormatReflectedFieldLabel(const std::string& p_name)
{
    if (p_name.empty())
        return {};

    std::string result;
    result.reserve(p_name.size() + 4);

    for (size_t i = 0; i < p_name.size(); ++i)
    {
        const char c = p_name[i];
        if (i > 0 && std::isupper(static_cast<unsigned char>(c)) != 0)
            result.push_back(' ');
        result.push_back(c);
    }

    if (!result.empty())
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));

    return result;
}

std::string FormatEnumChoiceLabel(std::string_view p_key)
{
    if (p_key.empty())
        return {};

    auto formatWord = [](std::string_view p_word) -> std::string
    {
        if (p_word.empty())
            return {};

        std::string formatted;
        formatted.reserve(p_word.size());

        bool upperNext = true;
        for (const char raw : p_word)
        {
            const auto ch = static_cast<unsigned char>(raw);
            if (std::isdigit(ch) != 0)
            {
                formatted.push_back(static_cast<char>(ch));
                upperNext = false;
                continue;
            }

            if (std::isalpha(ch) == 0)
            {
                formatted.push_back(static_cast<char>(ch));
                continue;
            }

            formatted.push_back(static_cast<char>(upperNext ? std::toupper(ch) : std::tolower(ch)));
            upperNext = false;
        }

        return formatted;
    };

    std::string result;
    std::string current;

    const auto flushCurrent = [&]()
    {
        if (current.empty())
            return;

        if (!result.empty())
            result.push_back(' ');

        result += formatWord(current);
        current.clear();
    };

    const bool hasUnderscore = p_key.find('_') != std::string_view::npos;
    for (size_t i = 0; i < p_key.size(); ++i)
    {
        const char raw = p_key[i];
        const auto ch = static_cast<unsigned char>(raw);

        if (raw == '_')
        {
            flushCurrent();
            continue;
        }

        if (!hasUnderscore
            && !current.empty()
            && std::isupper(ch) != 0
            && std::islower(static_cast<unsigned char>(current.back())) != 0)
        {
            flushCurrent();
        }

        current.push_back(static_cast<char>(ch));
    }

    flushCurrent();
    return result;
}

std::map<int, std::string> BuildEnumChoices(const meta::Type& p_enumType)
{
    std::map<int, std::string> choices;
    if (!p_enumType.IsValid() || !p_enumType.IsEnum())
        return choices;

    const auto keys = p_enumType.GetEnum().GetKeys();
    for (const auto& key : keys)
    {
        const auto value = p_enumType.GetEnum().GetValue(key);
        if (!value.IsValid())
            continue;

        choices.emplace(value.ToInt(), FormatEnumChoiceLabel(key));
    }

    return choices;
}

std::string NormalizeReflectedSearchText(std::string_view p_text)
{
    std::string result;
    result.reserve(p_text.size());
    for (const char raw : p_text)
    {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isspace(ch) == 0)
            result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result;
}

ReflectedPropertySupport GetReflectedPropertySupport(const meta::Field& p_field)
{
    if (!p_field.IsValid())
        return ReflectedPropertySupport::Unsupported;

    const auto fieldType = p_field.GetType();
    if (fieldType.IsArray())
        return ReflectedPropertySupport::Array;
    if (fieldType.IsEnum() && !BuildEnumChoices(fieldType).empty())
        return ReflectedPropertySupport::Enum;
    if (fieldType == NLS_TYPEOF(bool))
        return ReflectedPropertySupport::Bool;
    if (fieldType == NLS_TYPEOF(int))
        return ReflectedPropertySupport::Int;
    if (fieldType == NLS_TYPEOF(float))
        return ReflectedPropertySupport::Float;
    if (fieldType == NLS_TYPEOF(std::string))
        return ReflectedPropertySupport::String;
    if (fieldType == NLS_TYPEOF(Maths::Vector2))
        return ReflectedPropertySupport::Vector2;
    if (fieldType == NLS_TYPEOF(Maths::Vector3))
        return ReflectedPropertySupport::Vector3;
    if (fieldType == NLS_TYPEOF(Maths::Vector4))
        return ReflectedPropertySupport::Vector4;
    if (fieldType == NLS_TYPEOF(Maths::Quaternion))
        return ReflectedPropertySupport::Quaternion;
    if (fieldType == NLS_TYPEOF(Maths::Color))
        return ReflectedPropertySupport::Color;
    if (fieldType == NLS_TYPEOF(Maths::Rect))
        return ReflectedPropertySupport::Rect;
    if (fieldType == NLS_TYPEOF(Render::Geometry::Bounds))
        return ReflectedPropertySupport::Bounds;
    if (fieldType == NLS_TYPEOF(Render::Geometry::BoundingSphere))
        return ReflectedPropertySupport::BoundingSphere;
    if (fieldType == NLS_TYPEOF(Engine::LayerMask))
        return ReflectedPropertySupport::LayerMask;
    if (IsSupportedIdentityObjectReferenceType(fieldType) || IsSceneObjectReferenceType(fieldType) || IsResourceObjectReferenceType(fieldType))
        return ReflectedPropertySupport::ObjectReference;
    return ReflectedPropertySupport::Unsupported;
}

bool ReflectedFieldMatchesSearch(const meta::Field& p_field, std::string_view p_searchText)
{
    const auto query = NormalizeReflectedSearchText(p_searchText);
    if (query.empty())
        return true;

    const auto name = NormalizeReflectedSearchText(p_field.GetName());
    const auto label = NormalizeReflectedSearchText(FormatReflectedFieldLabel(p_field.GetName()));
    return name.find(query) != std::string::npos || label.find(query) != std::string::npos;
}

bool DrawReflectedField(
    UI::Internal::WidgetContainer& p_root,
    meta::Variant&& p_instance,
    const meta::Field& p_field,
    const ReflectedPropertyDrawerOptions& p_options)
{
    auto sharedInstance = std::make_shared<meta::Variant>(std::move(p_instance));
    return DrawReflectedFieldInternal(p_root, sharedInstance, p_field, p_options);
}

}

namespace
{
bool DrawReflectedFieldInternal(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    using NLS::Editor::Panels::ReflectedPropertySupport;

    if (!p_field.IsValid() || !NLS::Editor::Panels::ReflectedFieldMatchesSearch(p_field, p_options.searchText))
        return false;

    const auto fieldType = p_field.GetType();
    const auto label = BuildFieldLabel(p_field, p_options);
    const auto support = NLS::Editor::Panels::GetReflectedPropertySupport(p_field);

    switch (support)
    {
    case ReflectedPropertySupport::Enum:
        DrawEnumField(p_root, p_instance, p_field, NLS::Editor::Panels::BuildEnumChoices(fieldType), p_options);
        return true;
    case ReflectedPropertySupport::Bool:
        NLS::UI::GUIDrawer::CreateTitle(p_root, label);
        p_root.CreateWidget<ReflectedBoolWidget>(
            [p_instance, p_field]() mutable
            {
                return GetFieldValue<bool>(*p_instance, p_field);
            },
            [p_instance, p_field, p_options](bool p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            });
        return true;
    case ReflectedPropertySupport::Int:
        if (const auto range = GetRangeLimits(p_field); range.has_value())
        {
            DrawRangeSliderField<int, NLS::UI::Widgets::SliderInt>(
                p_root,
                p_instance,
                p_field,
                label,
                static_cast<int>(std::lround(range->first)),
                static_cast<int>(std::lround(range->second)),
                "%d",
                p_options);
            return true;
        }
        NLS::UI::GUIDrawer::DrawScalar<int>(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<int>(*p_instance, p_field); },
            [p_instance, p_field, p_options](int p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            });
        return true;
    case ReflectedPropertySupport::Float:
        if (const auto range = GetRangeLimits(p_field); range.has_value())
        {
            DrawRangeSliderField<float, NLS::UI::Widgets::SliderFloat>(
                p_root,
                p_instance,
                p_field,
                label,
                range->first,
                range->second,
                "%.3f",
                p_options);
            return true;
        }
        NLS::UI::GUIDrawer::DrawScalar<float>(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<float>(*p_instance, p_field); },
            [p_instance, p_field, p_options](float p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
        return true;
    case ReflectedPropertySupport::String:
        NLS::UI::GUIDrawer::DrawDDString(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<std::string>(*p_instance, p_field); },
            [p_instance, p_field, p_options](std::string p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            },
            "File");
        return true;
    case ReflectedPropertySupport::Vector2:
        NLS::UI::GUIDrawer::DrawVec2(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<Maths::Vector2>(*p_instance, p_field); },
            [p_instance, p_field, p_options](Maths::Vector2 p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
        return true;
    case ReflectedPropertySupport::Vector3:
        NLS::UI::GUIDrawer::DrawVec3(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<Maths::Vector3>(*p_instance, p_field); },
            [p_instance, p_field, p_options](Maths::Vector3 p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
        return true;
    case ReflectedPropertySupport::Vector4:
        NLS::UI::GUIDrawer::DrawVec4(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<Maths::Vector4>(*p_instance, p_field); },
            [p_instance, p_field, p_options](Maths::Vector4 p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
        return true;
    case ReflectedPropertySupport::Quaternion:
        NLS::UI::GUIDrawer::DrawQuat(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<Maths::Quaternion>(*p_instance, p_field); },
            [p_instance, p_field, p_options](Maths::Quaternion p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
        return true;
    case ReflectedPropertySupport::Color:
        NLS::UI::GUIDrawer::DrawColor(
            p_root,
            label,
            [p_instance, p_field]() mutable { return GetFieldValue<Maths::Color>(*p_instance, p_field); },
            [p_instance, p_field, p_options](Maths::Color p_value) mutable
            {
                SetFieldValue(*p_instance, p_field, p_value);
                NotifyChanged(p_options, p_field);
            },
            true);
        return true;
    case ReflectedPropertySupport::Rect:
        DrawRectField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::Bounds:
        DrawBoundsField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::BoundingSphere:
        DrawBoundingSphereField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::ObjectReference:
        DrawObjectReferenceField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::Array:
        DrawGenericArrayField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::LayerMask:
        DrawLayerMaskField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::Unsupported:
    default:
        p_root.CreateWidget<NLS::UI::Widgets::Text>(label + " (unsupported)");
        p_root.CreateWidget<NLS::UI::Widgets::Text>(fieldType.GetName());
        return true;
    }
}
}

namespace NLS::Editor::Panels
{

int DrawReflectedObject(
    UI::Internal::WidgetContainer& p_root,
    meta::Variant&& p_instance,
    const ReflectedPropertyDrawerOptions& p_options)
{
    auto sharedInstance = std::make_shared<meta::Variant>(std::move(p_instance));
    return DrawReflectedObjectInternal(p_root, sharedInstance, p_options);
}

std::vector<std::string> GetReflectedPropertyLabels(const meta::Type& p_type)
{
    std::vector<std::string> labels;
    if (!p_type.IsValid())
        return labels;

    for (const auto& field : p_type.GetFields())
    {
        if (field.IsValid())
            labels.push_back(FormatReflectedFieldLabel(field.GetName()));
    }

    return labels;
}
}

namespace
{
int DrawReflectedObjectInternal(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    auto& columns = p_root.CreateWidget<NLS::UI::Widgets::Columns>(2);
    columns.widths[0] = p_options.labelWidth;

    int drawnFields = 0;
    const auto type = p_instance->GetType();
    for (const auto& field : type.GetFields())
    {
        if (DrawReflectedFieldInternal(columns, p_instance, field, p_options))
            ++drawnFields;
    }

    if (drawnFields == 0)
    {
        if (p_options.searchText.empty())
            p_root.CreateWidget<NLS::UI::Widgets::Text>("No reflected fields");
        else
            p_root.CreateWidget<NLS::UI::Widgets::Text>("No matching settings");
    }

    return drawnFields;
}
} // namespace
