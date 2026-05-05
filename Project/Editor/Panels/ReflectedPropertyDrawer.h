#pragma once

#include <Reflection/Enum.h>
#include <Reflection/Field.h>
#include <Reflection/Type.h>
#include <Reflection/Variant.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace NLS::UI::Internal
{
class WidgetContainer;
}

namespace NLS::Editor::Panels
{
enum class ReflectedPropertySupport
{
    Unsupported,
    Bool,
    Int,
    Float,
    String,
    Vector3,
    Vector4,
    Quaternion,
    BoundingSphere,
    StringArray,
    FloatArray,
    Enum
};

struct ReflectedPropertyDrawerOptions
{
    float labelWidth = 104.0f;
    std::string searchText;
    std::function<void(const meta::Field&)> onFieldChanged;
};

std::string FormatReflectedFieldLabel(const std::string& p_name);
std::string FormatEnumChoiceLabel(std::string_view p_key);
std::map<int, std::string> BuildEnumChoices(const meta::Type& p_enumType);
std::string NormalizeReflectedSearchText(std::string_view p_text);

ReflectedPropertySupport GetReflectedPropertySupport(const meta::Field& p_field);
bool ReflectedFieldMatchesSearch(const meta::Field& p_field, std::string_view p_searchText);

bool DrawReflectedField(
    UI::Internal::WidgetContainer& p_root,
    meta::Variant& p_instance,
    const meta::Field& p_field,
    const ReflectedPropertyDrawerOptions& p_options = {});

int DrawReflectedObject(
    UI::Internal::WidgetContainer& p_root,
    meta::Variant& p_instance,
    const ReflectedPropertyDrawerOptions& p_options = {});

std::vector<std::string> GetReflectedPropertyLabels(const meta::Type& p_type);
}
