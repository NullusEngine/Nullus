#include "Panels/ReflectedPropertyDrawer.h"

#include <Reflection/Array.h>
#include <UI/GUIDrawer.h>
#include <UI/Widgets/AWidget.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Selection/ComboBox.h>
#include <UI/Widgets/Texts/Text.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string_view>

#include "Rendering/Geometry/BoundingSphere.h"

namespace
{
using namespace NLS;
using ReflectedVariantHandle = std::shared_ptr<meta::Variant>;

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

void NotifyChanged(const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options, const meta::Field& p_field)
{
    if (p_options.onFieldChanged)
        p_options.onFieldChanged(p_field);
}

void DrawFloatArrayField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = NLS::Editor::Panels::FormatReflectedFieldLabel(p_field.GetName());
    auto values = GetFieldValue<NLS::Array<float>>(*p_instance, p_field);
    if (values.empty())
        values.resize(16, 0.0f);

    for (size_t row = 0; row < 4; ++row)
    {
        NLS::UI::GUIDrawer::DrawVec4(
            p_root,
            label + " " + std::to_string(row),
            [p_instance, p_field, row]() mutable
            {
                auto current = GetFieldValue<NLS::Array<float>>(*p_instance, p_field);
                Maths::Vector4 value {};
                const size_t base = row * 4;
                for (size_t column = 0; column < 4; ++column)
                    value[column] = base + column < current.size() ? current[base + column] : 0.0f;
                return value;
            },
            [p_instance, p_field, row, p_options](Maths::Vector4 p_value) mutable
            {
                auto current = GetFieldValue<NLS::Array<float>>(*p_instance, p_field);
                if (current.size() < 16)
                    current.resize(16, 0.0f);

                const size_t base = row * 4;
                for (size_t column = 0; column < 4; ++column)
                    current[base + column] = p_value[column];

                SetFieldValue(*p_instance, p_field, current);
                NotifyChanged(p_options, p_field);
            },
            0.01f,
            NLS::UI::GUIDrawer::_MIN_FLOAT,
            NLS::UI::GUIDrawer::_MAX_FLOAT);
    }
}

void DrawStringArrayField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = NLS::Editor::Panels::FormatReflectedFieldLabel(p_field.GetName());
    auto values = GetFieldValue<NLS::Array<std::string>>(*p_instance, p_field);
    if (values.empty())
        values.resize(1);

    for (size_t index = 0; index < values.size(); ++index)
    {
        NLS::UI::GUIDrawer::DrawDDString(
            p_root,
            label + " " + std::to_string(index),
            [p_instance, p_field, index]() mutable
            {
                auto current = GetFieldValue<NLS::Array<std::string>>(*p_instance, p_field);
                return index < current.size() ? current[index] : std::string {};
            },
            [p_instance, p_field, index, p_options](std::string p_value) mutable
            {
                auto current = GetFieldValue<NLS::Array<std::string>>(*p_instance, p_field);
                if (current.size() <= index)
                    current.resize(index + 1);
                current[index] = std::move(p_value);
                SetFieldValue(*p_instance, p_field, current);
                NotifyChanged(p_options, p_field);
            },
            "File");
    }
}

void DrawBoundingSphereField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = NLS::Editor::Panels::FormatReflectedFieldLabel(p_field.GetName());

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

void DrawEnumField(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
    const std::map<int, std::string>& p_choices,
    const NLS::Editor::Panels::ReflectedPropertyDrawerOptions& p_options)
{
    const auto label = NLS::Editor::Panels::FormatReflectedFieldLabel(p_field.GetName());
    NLS::UI::GUIDrawer::CreateTitle(p_root, label);
    auto& combo = p_root.CreateWidget<NLS::UI::Widgets::ComboBox>(GetFieldValue<int>(*p_instance, p_field));
    combo.choices = p_choices;
    combo.ValueChangedEvent += [p_instance, p_field, p_options](int p_value) mutable
    {
        SetFieldValue(*p_instance, p_field, p_value);
        NotifyChanged(p_options, p_field);
    };
}

bool DrawReflectedFieldInternal(
    NLS::UI::Internal::WidgetContainer& p_root,
    const ReflectedVariantHandle& p_instance,
    const meta::Field& p_field,
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
    if (fieldType == NLS_TYPEOF(Maths::Vector3))
        return ReflectedPropertySupport::Vector3;
    if (fieldType == NLS_TYPEOF(Maths::Vector4))
        return ReflectedPropertySupport::Vector4;
    if (fieldType == NLS_TYPEOF(Maths::Quaternion))
        return ReflectedPropertySupport::Quaternion;
    if (fieldType == NLS_TYPEOF(Render::Geometry::BoundingSphere))
        return ReflectedPropertySupport::BoundingSphere;
    if (fieldType == NLS_TYPEOF(NLS::Array<std::string>))
        return ReflectedPropertySupport::StringArray;
    if (fieldType == NLS_TYPEOF(NLS::Array<float>))
        return ReflectedPropertySupport::FloatArray;
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
    meta::Variant& p_instance,
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
    const auto label = NLS::Editor::Panels::FormatReflectedFieldLabel(p_field.GetName());
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
    case ReflectedPropertySupport::BoundingSphere:
        DrawBoundingSphereField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::StringArray:
        DrawStringArrayField(p_root, p_instance, p_field, p_options);
        return true;
    case ReflectedPropertySupport::FloatArray:
        DrawFloatArrayField(p_root, p_instance, p_field, p_options);
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
    meta::Variant& p_instance,
    const ReflectedPropertyDrawerOptions& p_options)
{
    auto sharedInstance = std::make_shared<meta::Variant>(std::move(p_instance));
    auto& columns = p_root.CreateWidget<UI::Widgets::Columns>(2);
    columns.widths[0] = p_options.labelWidth;

    int drawnFields = 0;
    const auto type = sharedInstance->GetType();
    for (const auto& field : type.GetFields())
    {
        if (DrawReflectedFieldInternal(columns, sharedInstance, field, p_options))
            ++drawnFields;
    }

    if (drawnFields == 0)
    {
        if (p_options.searchText.empty())
            p_root.CreateWidget<UI::Widgets::Text>("No reflected fields");
        else
            p_root.CreateWidget<UI::Widgets::Text>("No matching settings");
    }

    return drawnFields;
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
