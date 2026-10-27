#include "Inspector/ObjectReferenceSupport.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_set>

#include <Core/ResourceManagement/MaterialManager.h>
#include <Core/ResourceManagement/MeshManager.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ResourceManagement/TextureManager.h>
#include <Core/ServiceLocator.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <Engine/UI/UIResourceReference.h>
#include <Engine/UI/UIDocumentReference.h>
#include <Object/Object.h>

#include "Assets/EditorAssetDragPayload.h"
#include "Components/Component.h"
#include "GameObject.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture2D.h"
#include "SceneSystem/Scene.h"
#include "Serialize/PPtr.h"
#include "Serialize/PPtrResourceTypes.h"

namespace NLS::Editor::Inspector
{
namespace
{
bool IsSinglePointerType(const meta::Type& type)
{
    const auto typeName = type.GetName();
    return type.IsPointer() &&
        !typeName.empty() &&
        typeName.back() == '*' &&
        (typeName.size() < 2u || typeName[typeName.size() - 2u] != '*');
}

bool IsGameObjectPointerType(const meta::Type& type)
{
    return IsSinglePointerType(type) && type.GetDecayedType() == NLS_TYPEOF(Engine::GameObject);
}

bool IsGenericObjectPointerType(const meta::Type& type)
{
    return IsSinglePointerType(type) && type.GetDecayedType() == NLS_TYPEOF(Object);
}

bool IsComponentPointerType(const meta::Type& type)
{
    if (!IsSinglePointerType(type))
        return false;

    const auto objectType = type.GetDecayedType();
    const auto componentType = NLS_TYPEOF(Engine::Components::Component);
    return objectType.IsValid() && (objectType == componentType || objectType.DerivesFrom(componentType));
}

bool IsSupportedPPtrType(const meta::Type& type)
{
    if (!type.IsValid())
        return false;
#define NLS_INSPECTOR_MATCH_PPTR_RESOURCE_TYPE(targetType, label, artifactType, subAssetPrefix) \
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<targetType>)) return true;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_INSPECTOR_MATCH_PPTR_RESOURCE_TYPE)
#undef NLS_INSPECTOR_MATCH_PPTR_RESOURCE_TYPE
    return type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>) ||
        type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Components::Component>);
}

bool IsPPtrSceneObjectType(const meta::Type& type)
{
    return type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>) ||
        type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Components::Component>);
}

bool IsResourcePointerType(const meta::Type& type)
{
    return type == NLS_TYPEOF(Render::Resources::Texture2D*) ||
        type == NLS_TYPEOF(Render::Resources::Mesh*) ||
        type == NLS_TYPEOF(Render::Resources::Shader*) ||
        type == NLS_TYPEOF(Render::Resources::Material*);
}

bool IsUIDocumentReferenceType(const meta::Type& type)
{
    return type == NLS_TYPEOF(Engine::UI::UIDocumentReference);
}

bool IsUITextureReferenceType(const meta::Type& type)
{
    return type == NLS_TYPEOF(Engine::UI::UITextureReference);
}

bool IsUIFontReferenceType(const meta::Type& type)
{
    return type == NLS_TYPEOF(Engine::UI::UIFontReference);
}

bool IsUIResourceReferenceType(const meta::Type& type)
{
    return IsUITextureReferenceType(type) || IsUIFontReferenceType(type);
}

template <typename Reference>
std::string DescribeUIResourceReference(
    const meta::Variant& value,
    const char* label)
{
    const auto& reference = value.GetValue<Reference>();
    if (reference.IsNull())
        return std::string("None (") + label + ")";

    const auto& path = reference.GetResourcePath();
    const auto fileName = std::filesystem::path(path).filename().generic_string();
    const auto source = fileName.empty()
        ? (path.empty() ? reference.GetAssetIdString() : path)
        : fileName;
    return (source.empty() ? std::string("Assigned") : source) +
        " (" + label + ")";
}

template <typename Reference>
bool AssignUIResourceReference(
    meta::Variant& value,
    const Assets::EditorAssetDragPayload& payload)
{
    const auto assetId = Assets::GetEditorAssetDragPayloadAssetId(payload);
    const auto path = Assets::GetEditorAssetDragPayloadPath(payload);
    if (!assetId.IsValid() && path.empty())
        return false;
    value = Reference(
        assetId,
        Assets::GetEditorAssetDragPayloadSubAssetKey(payload),
        path);
    return true;
}

std::string ShortTypeName(const meta::Type& type)
{
    std::string name = type.IsValid() ? type.GetName() : std::string {};
    const auto separator = name.rfind("::");
    if (separator != std::string::npos)
        name.erase(0u, separator + 2u);
    return name;
}

std::string NormalizeSearchText(const std::string_view text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (const auto raw : text)
    {
        const auto character = static_cast<unsigned char>(raw);
        if (std::isspace(character) == 0)
            normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

std::string BuildGameObjectHierarchyPath(const Engine::GameObject& gameObject, const Engine::SceneSystem::Scene* scene)
{
    std::vector<std::string> names;
    std::unordered_set<const Engine::GameObject*> visited;
    const auto* current = &gameObject;
    while (current != nullptr && current->GetScene() == scene && visited.insert(current).second)
    {
        names.push_back(current->GetName().empty() ? "<Unnamed>" : current->GetName());
        current = current->GetParent();
    }

    std::reverse(names.begin(), names.end());
    std::string result;
    for (const auto& name : names)
    {
        if (!result.empty())
            result += '/';
        result += name;
    }
    return result;
}

bool IsAssignableSceneComponent(const meta::Type& type, const Engine::Components::Component* component)
{
    if (component == nullptr || !IsComponentPointerType(type))
        return false;

    const auto requiredType = type.GetDecayedType();
    const auto actualType = component->GetType();
    const auto componentType = NLS_TYPEOF(Engine::Components::Component);
    return requiredType == componentType || actualType == requiredType || actualType.DerivesFrom(requiredType);
}

std::optional<Engine::Serialize::InstanceID> GetPPtrInstanceId(const meta::Variant& value, const meta::Type& type)
{
#define NLS_INSPECTOR_GET_PPTR_INSTANCE_ID(targetType, label, artifactType, subAssetPrefix) \
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<targetType>)) \
        return value.GetValue<Engine::Serialize::PPtr<targetType>>().GetInstanceID();
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_INSPECTOR_GET_PPTR_INSTANCE_ID)
#undef NLS_INSPECTOR_GET_PPTR_INSTANCE_ID
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>))
        return value.GetValue<Engine::Serialize::PPtr<Engine::GameObject>>().GetInstanceID();
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Components::Component>))
        return value.GetValue<Engine::Serialize::PPtr<Engine::Components::Component>>().GetInstanceID();
    return std::nullopt;
}

std::string GetPPtrTypeLabel(const meta::Type& type)
{
#define NLS_INSPECTOR_PPTR_TYPE_LABEL(targetType, label, artifactType, subAssetPrefix) \
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<targetType>)) return label;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_INSPECTOR_PPTR_TYPE_LABEL)
#undef NLS_INSPECTOR_PPTR_TYPE_LABEL
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>))
        return "GameObject";
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Components::Component>))
        return "Component";
    return ShortTypeName(type);
}

bool SetPPtrInstanceId(meta::Variant& value, const meta::Type& type, const Engine::Serialize::InstanceID instanceId)
{
#define NLS_INSPECTOR_SET_PPTR_INSTANCE_ID(targetType, label, artifactType, subAssetPrefix) \
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<targetType>)) \
    { \
        value = Engine::Serialize::PPtr<targetType>(instanceId); \
        return true; \
    }
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_INSPECTOR_SET_PPTR_INSTANCE_ID)
#undef NLS_INSPECTOR_SET_PPTR_INSTANCE_ID
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>))
    {
        value = Engine::Serialize::PPtr<Engine::GameObject>(instanceId);
        return true;
    }
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Components::Component>))
    {
        value = Engine::Serialize::PPtr<Engine::Components::Component>(instanceId);
        return true;
    }
    return false;
}

bool IsNullPPtr(const meta::Variant& value, const meta::Type& type)
{
    return GetPPtrInstanceId(value, type).value_or(Engine::Serialize::InstanceID_None) ==
        Engine::Serialize::InstanceID_None;
}

bool IsCompatiblePPtrArtifact(const meta::Type& type, const NLS::Core::Assets::ArtifactType artifactType)
{
    if(type==NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Assets::AnimationAsset>))
        return artifactType==NLS::Core::Assets::ArtifactType::Skeleton||artifactType==NLS::Core::Assets::ArtifactType::Skin||
            artifactType==NLS::Core::Assets::ArtifactType::AnimationClip||artifactType==NLS::Core::Assets::ArtifactType::AnimationGraph||artifactType==NLS::Core::Assets::ArtifactType::SkeletalMesh;
#define NLS_INSPECTOR_MATCH_PPTR_ARTIFACT(targetType, label, expectedArtifactType, subAssetPrefix) \
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<targetType>)) return artifactType == expectedArtifactType;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_INSPECTOR_MATCH_PPTR_ARTIFACT)
#undef NLS_INSPECTOR_MATCH_PPTR_ARTIFACT
    return false;
}

std::string DefaultPPtrSubAssetKey(const meta::Type& type, const std::string& assetPath)
{
    std::string stem = std::filesystem::path(assetPath).stem().generic_string();
    if (stem.empty())
        stem = "Main";

#define NLS_INSPECTOR_PPTR_SUBASSET_KEY(targetType, label, artifactType, subAssetPrefix) \
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<targetType>)) return std::string(subAssetPrefix) + ":" + stem;
    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_INSPECTOR_PPTR_SUBASSET_KEY)
#undef NLS_INSPECTOR_PPTR_SUBASSET_KEY
    return {};
}

std::optional<Engine::Serialize::ObjectIdentifier> MakeAssetIdentifier(
    const meta::Type& type,
    const Assets::EditorAssetDragPayload& payload)
{
    const auto assetPath = Assets::GetEditorAssetDragPayloadPath(payload);
    const auto assetId = Assets::GetEditorAssetDragPayloadAssetId(payload);
    if (assetPath.empty() || !assetId.IsValid() || !IsCompatiblePPtrArtifact(type, Assets::GetEditorAssetDragPayloadArtifactType(payload)))
        return std::nullopt;

    auto subAssetKey = Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
    if (subAssetKey.empty())
        subAssetKey = DefaultPPtrSubAssetKey(type, assetPath);
    if (subAssetKey.empty())
        return std::nullopt;

    const auto guid = assetId.GetGuid();
    return Engine::Serialize::ObjectIdentifier::Asset(
        Engine::Serialize::AssetId(guid),
        Engine::Serialize::MakeLocalIdentifierInFile(guid, subAssetKey),
        assetPath);
}

std::string ResolveAssetLoadPath(const Assets::EditorAssetDragPayload& payload)
{
    const auto assetId = Assets::GetEditorAssetDragPayloadAssetId(payload);
    const auto subAssetKey = Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
    const auto artifactType = Assets::GetEditorAssetDragPayloadArtifactType(payload);
    if (assetId.IsValid() && !subAssetKey.empty() &&
        Core::ServiceLocator::Contains<Engine::Assets::RuntimeAssetDatabase>())
    {
        const auto& database = Core::ServiceLocator::Get<Engine::Assets::RuntimeAssetDatabase>();
        const auto* entry = database.Resolve(assetId, subAssetKey);
        if (entry != nullptr && entry->artifactType == artifactType && !entry->artifactPath.empty())
            return entry->artifactPath;
    }
    return Assets::GetEditorAssetDragPayloadPath(payload);
}

std::string DescribeObjectPointer(Object* object, const meta::Type& type)
{
    const auto label = ShortTypeName(type.GetDecayedType());
    if (object == nullptr)
        return "None (" + label + ")";
    if (const auto* gameObject = dynamic_cast<const Engine::GameObject*>(object))
        return (gameObject->GetName().empty() ? std::string("<Unnamed>") : gameObject->GetName()) + " (GameObject)";
    if (const auto* component = dynamic_cast<const Engine::Components::Component*>(object))
    {
        const auto* owner = component->gameobject();
        const auto ownerName = owner == nullptr || owner->GetName().empty() ? std::string("<Unnamed>") : owner->GetName();
        return ownerName + " (" + ShortTypeName(component->GetType()) + ")";
    }
    return "Assigned (" + label + ")";
}

template <typename T>
std::string DescribeResourcePointer(T* resource, const char* label, const std::string& path = {})
{
    if (resource == nullptr)
        return std::string("None (") + label + ")";
    const auto fileName = std::filesystem::path(path).filename().generic_string();
    return (fileName.empty() ? (path.empty() ? std::string("Assigned") : path) : fileName) + " (" + label + ")";
}

Object* GetSceneObjectPointer(const meta::Variant& value)
{
    return value.GetValue<Object*>();
}

std::optional<InspectorTargetHandle> GetScenePingTarget(Object* object)
{
    if (auto* gameObject = dynamic_cast<Engine::GameObject*>(object))
        return InspectorTargetHandle::FromObject(*gameObject, InspectorTargetKind::GameObject);
    if (auto* component = dynamic_cast<Engine::Components::Component*>(object))
    {
        if (auto* owner = component->gameobject())
            return InspectorTargetHandle::FromObject(*owner, InspectorTargetKind::GameObject);
    }
    return std::nullopt;
}
} // namespace

bool IsInspectorSceneObjectReferenceType(const meta::Type& type)
{
    return IsGameObjectPointerType(type) || IsComponentPointerType(type) ||
        IsGenericObjectPointerType(type) || IsPPtrSceneObjectType(type);
}

bool IsInspectorAssetObjectReferenceType(const meta::Type& type)
{
    return IsSupportedPPtrType(type) || IsResourcePointerType(type) ||
        IsUIDocumentReferenceType(type) || IsUIResourceReferenceType(type);
}

bool IsSupportedInspectorObjectReferenceType(const meta::Type& type)
{
    return IsInspectorSceneObjectReferenceType(type) || IsInspectorAssetObjectReferenceType(type);
}

bool IsCompatibleInspectorReferenceArtifact(const meta::Type& type, const NLS::Core::Assets::ArtifactType artifactType)
{
    if (IsCompatiblePPtrArtifact(type, artifactType))
        return true;
    if (type == NLS_TYPEOF(Render::Resources::Texture2D*))
        return artifactType == NLS::Core::Assets::ArtifactType::Texture;
    if (type == NLS_TYPEOF(Render::Resources::Mesh*))
        return artifactType == NLS::Core::Assets::ArtifactType::Mesh;
    if (type == NLS_TYPEOF(Render::Resources::Shader*))
        return artifactType == NLS::Core::Assets::ArtifactType::Shader;
    if (type == NLS_TYPEOF(Render::Resources::Material*))
        return artifactType == NLS::Core::Assets::ArtifactType::Material;
    if (IsUIDocumentReferenceType(type))
        return artifactType == NLS::Core::Assets::ArtifactType::UIDocument;
    if (IsUITextureReferenceType(type))
        return artifactType == NLS::Core::Assets::ArtifactType::Texture;
    if (IsUIFontReferenceType(type))
        return artifactType == NLS::Core::Assets::ArtifactType::Font;
    return false;
}

std::string GetInspectorObjectReferenceDisplay(const meta::Variant& value, const meta::Type& type)
{
    if (!value.IsValid())
        return "Unavailable";

    if (IsSupportedPPtrType(type))
    {
        const auto instanceId = GetPPtrInstanceId(value, type).value_or(Engine::Serialize::InstanceID_None);
        Engine::Serialize::ObjectIdentifier identifier;
        if (instanceId == Engine::Serialize::InstanceID_None ||
            !Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(instanceId, identifier) ||
            !identifier.IsValid())
        {
            return "None (" + GetPPtrTypeLabel(type) + ")";
        }
        const auto source = !identifier.filePath.empty() ? identifier.filePath : identifier.guid.ToString();
        return source + " (" + GetPPtrTypeLabel(type) + ")";
    }

    if (IsUIDocumentReferenceType(type))
    {
        const auto& reference = value.GetValue<Engine::UI::UIDocumentReference>();
        if (reference.IsNull())
            return "None (UI Document)";
        const auto& path = reference.GetResourcePath();
        const auto fileName = std::filesystem::path(path).filename().generic_string();
        const auto source = fileName.empty()
            ? (path.empty() ? reference.GetAssetIdString() : path)
            : fileName;
        return (source.empty() ? std::string("Assigned") : source) + " (UI Document)";
    }

    if (IsUITextureReferenceType(type))
        return DescribeUIResourceReference<Engine::UI::UITextureReference>(value, "Texture");
    if (IsUIFontReferenceType(type))
        return DescribeUIResourceReference<Engine::UI::UIFontReference>(value, "Font");

    if (IsInspectorSceneObjectReferenceType(type))
        return DescribeObjectPointer(GetSceneObjectPointer(value), type);
    if (type == NLS_TYPEOF(Render::Resources::Texture2D*))
    {
        const auto* resource = value.GetValue<Render::Resources::Texture2D*>();
        return DescribeResourcePointer(resource, "Texture2D", resource != nullptr ? resource->path : std::string {});
    }
    if (type == NLS_TYPEOF(Render::Resources::Mesh*))
    {
        const auto* resource = value.GetValue<Render::Resources::Mesh*>();
        return DescribeResourcePointer(resource, "Mesh", resource != nullptr ? resource->GetName() : std::string {});
    }
    if (type == NLS_TYPEOF(Render::Resources::Shader*))
    {
        const auto* resource = value.GetValue<Render::Resources::Shader*>();
        return DescribeResourcePointer(resource, "Shader", resource != nullptr ? resource->path : std::string {});
    }
    if (type == NLS_TYPEOF(Render::Resources::Material*))
    {
        const auto* resource = value.GetValue<Render::Resources::Material*>();
        return DescribeResourcePointer(resource, "Material", resource != nullptr ? resource->path : std::string {});
    }
    return "None (" + ShortTypeName(type) + ")";
}

bool ClearInspectorObjectReference(meta::Variant& value, const meta::Type& type)
{
    if (IsUIDocumentReferenceType(type))
    {
        auto reference = value.GetValue<Engine::UI::UIDocumentReference>();
        if (reference.IsNull())
            return false;
        reference.Reset();
        value = std::move(reference);
        return true;
    }

    if (IsUITextureReferenceType(type))
    {
        auto reference = value.GetValue<Engine::UI::UITextureReference>();
        if (reference.IsNull())
            return false;
        reference.Reset();
        value = std::move(reference);
        return true;
    }
    if (IsUIFontReferenceType(type))
    {
        auto reference = value.GetValue<Engine::UI::UIFontReference>();
        if (reference.IsNull())
            return false;
        reference.Reset();
        value = std::move(reference);
        return true;
    }

    if (IsSupportedPPtrType(type))
    {
        if (IsNullPPtr(value, type))
            return false;
        return SetPPtrInstanceId(value, type, Engine::Serialize::InstanceID_None);
    }
    if (IsInspectorSceneObjectReferenceType(type))
    {
        if (GetSceneObjectPointer(value) == nullptr)
            return false;
        Object* empty = nullptr;
        value = ObjectVariant(empty);
        return true;
    }
    if (type == NLS_TYPEOF(Render::Resources::Texture2D*))
    {
        if (value.GetValue<Render::Resources::Texture2D*>() == nullptr) return false;
        value = static_cast<Render::Resources::Texture2D*>(nullptr);
        return true;
    }
    if (type == NLS_TYPEOF(Render::Resources::Mesh*))
    {
        if (value.GetValue<Render::Resources::Mesh*>() == nullptr) return false;
        value = static_cast<Render::Resources::Mesh*>(nullptr);
        return true;
    }
    if (type == NLS_TYPEOF(Render::Resources::Shader*))
    {
        if (value.GetValue<Render::Resources::Shader*>() == nullptr) return false;
        value = static_cast<Render::Resources::Shader*>(nullptr);
        return true;
    }
    if (type == NLS_TYPEOF(Render::Resources::Material*))
    {
        if (value.GetValue<Render::Resources::Material*>() == nullptr) return false;
        value = static_cast<Render::Resources::Material*>(nullptr);
        return true;
    }
    return false;
}

bool AssignInspectorAssetReference(
    meta::Variant& value,
    const meta::Type& type,
    const Assets::EditorAssetDragPayload& payload)
{
    const auto artifactType = Assets::GetEditorAssetDragPayloadArtifactType(payload);
    if (!IsCompatibleInspectorReferenceArtifact(type, artifactType))
        return false;

    if (IsSupportedPPtrType(type))
    {
        const auto identifier = MakeAssetIdentifier(type, payload);
        if (!identifier.has_value())
            return false;
        const auto instanceId = Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(*identifier);
        return instanceId != Engine::Serialize::InstanceID_None && SetPPtrInstanceId(value, type, instanceId);
    }

    if (IsUIDocumentReferenceType(type))
    {
        const auto assetId = Assets::GetEditorAssetDragPayloadAssetId(payload);
        const auto path = Assets::GetEditorAssetDragPayloadPath(payload);
        if (!assetId.IsValid() && path.empty())
            return false;
        value = Engine::UI::UIDocumentReference(assetId, path);
        return true;
    }

    if (IsUITextureReferenceType(type))
        return AssignUIResourceReference<Engine::UI::UITextureReference>(value, payload);
    if (IsUIFontReferenceType(type))
        return AssignUIResourceReference<Engine::UI::UIFontReference>(value, payload);

    const auto path = ResolveAssetLoadPath(payload);
    if (path.empty())
        return false;
    if (type == NLS_TYPEOF(Render::Resources::Texture2D*))
    {
        if (!Core::ServiceLocator::Contains<Core::ResourceManagement::TextureManager>()) return false;
        auto* resource = Core::ServiceLocator::Get<Core::ResourceManagement::TextureManager>().GetArtifactResource(path, true);
        if (resource == nullptr) return false;
        value = resource;
        return true;
    }
    if (type == NLS_TYPEOF(Render::Resources::Mesh*))
    {
        if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>()) return false;
        auto* resource = Core::ServiceLocator::Get<Core::ResourceManagement::MeshManager>().GetResource(path, true);
        if (resource == nullptr) return false;
        value = resource;
        return true;
    }
    if (type == NLS_TYPEOF(Render::Resources::Shader*))
    {
        if (!Core::ServiceLocator::Contains<Core::ResourceManagement::ShaderManager>()) return false;
        auto* resource = Core::ServiceLocator::Get<Core::ResourceManagement::ShaderManager>().GetResource(path, true);
        if (resource == nullptr) return false;
        value = resource;
        return true;
    }
    if (type == NLS_TYPEOF(Render::Resources::Material*))
    {
        if (!Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>()) return false;
        auto* resource = Core::ServiceLocator::Get<Core::ResourceManagement::MaterialManager>().GetResource(path, true);
        if (resource == nullptr) return false;
        value = resource;
        return true;
    }
    return false;
}

bool AssignInspectorSceneReference(
    meta::Variant& value,
    const meta::Type& type,
    Engine::GameObject* gameObject,
    Engine::Components::Component* component)
{
    if (gameObject == nullptr)
        return false;

    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>))
        return SetPPtrInstanceId(value, type, gameObject->GetInstanceID());
    if (type == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Components::Component>))
    {
        if (component == nullptr)
            component = gameObject->GetTransform();
        return component != nullptr && SetPPtrInstanceId(value, type, component->GetInstanceID());
    }

    if (IsGameObjectPointerType(type) || IsGenericObjectPointerType(type))
    {
        Object* object = gameObject;
        value = ObjectVariant(object);
        return true;
    }
    if (!IsComponentPointerType(type))
        return false;
    if (component != nullptr && component->gameobject() != gameObject)
        return false;

    if (component == nullptr && type.GetDecayedType() == NLS_TYPEOF(Engine::Components::Component))
    {
        for (const auto& candidate : gameObject->GetComponents())
        {
            if (candidate != nullptr && candidate.get() != gameObject->GetTransform())
            {
                component = candidate.get();
                break;
            }
        }
        if (component == nullptr)
            component = gameObject->GetTransform();
    }
    else if (component == nullptr)
    {
        component = gameObject->GetComponent(type.GetDecayedType(), true);
    }
    if (!IsAssignableSceneComponent(type, component))
        return false;

    Object* object = component;
    value = ObjectVariant(object);
    return true;
}

std::vector<SceneObjectReferencePickerEntry> BuildInspectorSceneObjectReferencePickerEntries(
    const meta::Type& fieldType,
    const Engine::SceneSystem::Scene* scene,
    const std::string_view searchText)
{
    std::vector<SceneObjectReferencePickerEntry> result;
    if (scene == nullptr || !IsInspectorSceneObjectReferenceType(fieldType))
        return result;

    const auto query = NormalizeSearchText(searchText);
    const auto append = [&result, &query](SceneObjectReferencePickerEntry entry)
    {
        if (query.empty() || NormalizeSearchText(entry.displayName).find(query) != std::string::npos)
            result.push_back(std::move(entry));
    };

    for (auto* gameObject : scene->GetGameObjects())
    {
        if (gameObject == nullptr || !gameObject->IsAlive() || gameObject->GetScene() != scene)
            continue;
        const auto hierarchyPath = BuildGameObjectHierarchyPath(*gameObject, scene);
        if (IsGameObjectPointerType(fieldType) || IsGenericObjectPointerType(fieldType) ||
            fieldType == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>))
        {
            append({gameObject, nullptr, hierarchyPath + " (GameObject)"});
        }
        if (IsGameObjectPointerType(fieldType) || fieldType == NLS_TYPEOF(Engine::Serialize::PPtr<Engine::GameObject>))
            continue;

        for (const auto& candidate : gameObject->GetComponents())
        {
            if (candidate == nullptr)
                continue;
            if (!IsGenericObjectPointerType(fieldType) &&
                fieldType != NLS_TYPEOF(Engine::Serialize::PPtr<Engine::Components::Component>) &&
                !IsAssignableSceneComponent(fieldType, candidate.get()))
            {
                continue;
            }
            append({gameObject, candidate.get(), hierarchyPath + " (" + ShortTypeName(candidate->GetType()) + ")"});
        }
    }
    return result;
}

std::optional<InspectorTargetHandle> GetInspectorObjectReferencePingTarget(
    const meta::Variant& value,
    const meta::Type& type)
{
    if (!value.IsValid())
        return std::nullopt;
    if (IsInspectorSceneObjectReferenceType(type))
        return GetScenePingTarget(GetSceneObjectPointer(value));
    if (IsSupportedPPtrType(type))
    {
        const auto instanceId = GetPPtrInstanceId(value, type).value_or(Engine::Serialize::InstanceID_None);
        if (auto* object = Object::IDToPointer(instanceId))
            return GetScenePingTarget(object);

        Engine::Serialize::ObjectIdentifier identifier;
        if (instanceId != Engine::Serialize::InstanceID_None &&
            Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(instanceId, identifier) &&
            identifier.guid.IsValid())
        {
            const auto kind = type == NLS_TYPEOF(Engine::Serialize::PPtr<Render::Resources::Material>)
                ? InspectorTargetKind::Material
                : InspectorTargetKind::AssetImporter;
            return InspectorTargetHandle::FromAsset(kind, identifier.filePath, identifier.guid.ToString(), GetPPtrTypeLabel(type));
        }
    }
    if (IsUIDocumentReferenceType(type))
    {
        const auto& reference = value.GetValue<Engine::UI::UIDocumentReference>();
        if (!reference.IsNull())
        {
            return InspectorTargetHandle::FromAsset(
                InspectorTargetKind::AssetImporter,
                reference.GetResourcePath(),
                reference.GetAssetIdString(),
                "UI Document");
        }
    }
    if (IsUITextureReferenceType(type))
    {
        const auto& reference = value.GetValue<Engine::UI::UITextureReference>();
        if (!reference.IsNull())
        {
            return InspectorTargetHandle::FromAsset(
                InspectorTargetKind::AssetImporter,
                reference.GetResourcePath(),
                reference.GetAssetIdString(),
                "Texture");
        }
    }
    if (IsUIFontReferenceType(type))
    {
        const auto& reference = value.GetValue<Engine::UI::UIFontReference>();
        if (!reference.IsNull())
        {
            return InspectorTargetHandle::FromAsset(
                InspectorTargetKind::AssetImporter,
                reference.GetResourcePath(),
                reference.GetAssetIdString(),
                "Font");
        }
    }
    if (type == NLS_TYPEOF(Render::Resources::Material*))
    {
        const auto* material = value.GetValue<Render::Resources::Material*>();
        if (material != nullptr)
            return InspectorTargetHandle::FromAsset(InspectorTargetKind::Material, material->path, {}, "Material");
    }
    return std::nullopt;
}
} // namespace NLS::Editor::Inspector
