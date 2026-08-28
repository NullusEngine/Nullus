#pragma once

#include <Reflection/Enum.h>
#include <Reflection/Field.h>
#include <Reflection/Type.h>
#include <Reflection/Variant.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::UI::Internal
{
class WidgetContainer;
}

namespace NLS::Engine
{
class GameObject;
namespace Components { class Component; }
namespace SceneSystem { class Scene; }
}

namespace NLS::Editor::Panels
{
struct SceneObjectReferencePickerEntry
{
    Engine::GameObject* gameObject = nullptr;
    Engine::Components::Component* component = nullptr;
    std::string displayName;
};

enum class ReflectedPropertySupport
{
    Unsupported,
    Bool,
    Int,
    Float,
    String,
    Vector2,
    Vector3,
    Vector4,
    Quaternion,
    Color,
    Rect,
    Bounds,
    BoundingSphere,
    ObjectReference,
    Array,
    LayerMask,
    Enum
};

struct ReflectedPropertyDrawerOptions
{
    float labelWidth = 104.0f;
    std::string searchText;
    std::function<void(const meta::Field&)> onFieldChanged;
    std::function<void(const meta::Field&)> onFieldLayoutChanged;
    std::function<std::string(const meta::Field&)> fieldBadgeProvider;
    // The Inspector supplies the inspected object's owning scene. This keeps
    // object fields scoped to the active scene or Prefab Stage instead of
    // falling back to an unrelated global scene.
    Engine::SceneSystem::Scene* sceneObjectPickerScene = nullptr;
    // Lets a specialised Inspector control take ownership of a reflected
    // field without rendering a second generic editor for the same value.
    std::function<bool(const meta::Field&)> shouldDrawField;
};

std::string FormatReflectedFieldLabel(const std::string& p_name);
std::string FormatEnumChoiceLabel(std::string_view p_key);
std::map<int, std::string> BuildEnumChoices(const meta::Type& p_enumType);
std::string NormalizeReflectedSearchText(std::string_view p_text);

std::vector<SceneObjectReferencePickerEntry> BuildSceneObjectReferencePickerEntries(
    const meta::Type& p_fieldType,
    const Engine::SceneSystem::Scene* p_scene,
    std::string_view p_searchText = {});

bool ApplySceneObjectReferencePickerSelection(
    meta::Variant& p_instance,
    const meta::Field& p_field,
    const SceneObjectReferencePickerEntry& p_entry);

ReflectedPropertySupport GetReflectedPropertySupport(const meta::Field& p_field);
bool ReflectedFieldMatchesSearch(const meta::Field& p_field, std::string_view p_searchText);

bool DrawReflectedField(
    UI::Internal::WidgetContainer& p_root,
    meta::Variant&& p_instance,
    const meta::Field& p_field,
    const ReflectedPropertyDrawerOptions& p_options = {});

int DrawReflectedObject(
    UI::Internal::WidgetContainer& p_root,
    meta::Variant&& p_instance,
    const ReflectedPropertyDrawerOptions& p_options = {});

std::vector<std::string> GetReflectedPropertyLabels(const meta::Type& p_type);
}
