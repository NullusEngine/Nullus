#include "Settings/EditorSettingsRegistry.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "Panels/ReflectedPropertyDrawer.h"

namespace NLS::Editor::Settings
{
std::string NormalizeSettingsSearchText(std::string_view p_text)
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

bool EditorSettingsRegistry::Register(EditorSettingObject p_object)
{
    if (!p_object.IsValid() || Contains(p_object.id))
        return false;

    m_objects.push_back(std::move(p_object));
    std::sort(
        m_objects.begin(),
        m_objects.end(),
        [](const EditorSettingObject& p_left, const EditorSettingObject& p_right)
        {
            if (p_left.categoryPath != p_right.categoryPath)
                return p_left.categoryPath < p_right.categoryPath;
            if (p_left.displayName != p_right.displayName)
                return p_left.displayName < p_right.displayName;
            return p_left.id < p_right.id;
        });
    return true;
}

bool EditorSettingsRegistry::Contains(std::string_view p_id) const
{
    return Find(p_id) != nullptr;
}

const EditorSettingObject* EditorSettingsRegistry::Find(std::string_view p_id) const
{
    const auto it = std::find_if(
        m_objects.begin(),
        m_objects.end(),
        [p_id](const EditorSettingObject& p_object)
        {
            return p_object.id == p_id;
        });
    return it != m_objects.end() ? &*it : nullptr;
}

const std::vector<EditorSettingObject>& EditorSettingsRegistry::GetObjects() const
{
    return m_objects;
}

std::vector<const EditorSettingObject*> EditorSettingsRegistry::Search(std::string_view p_query) const
{
    const auto normalizedQuery = NormalizeSettingsSearchText(p_query);
    std::vector<const EditorSettingObject*> results;

    for (const auto& object : m_objects)
    {
        bool matches = normalizedQuery.empty()
            || NormalizeSettingsSearchText(object.id).find(normalizedQuery) != std::string::npos
            || NormalizeSettingsSearchText(object.displayName).find(normalizedQuery) != std::string::npos
            || NormalizeSettingsSearchText(object.categoryPath).find(normalizedQuery) != std::string::npos;

        if (!matches && object.type.IsValid())
        {
            for (const auto& label : Panels::GetReflectedPropertyLabels(object.type))
            {
                if (NormalizeSettingsSearchText(label).find(normalizedQuery) != std::string::npos)
                {
                    matches = true;
                    break;
                }
            }
        }

        if (matches)
            results.push_back(&object);
    }

    return results;
}

std::vector<std::string> EditorSettingsRegistry::GetCategories() const
{
    std::set<std::string> categories;
    for (const auto& object : m_objects)
        categories.insert(object.categoryPath);
    return { categories.begin(), categories.end() };
}

void EditorSettingsRegistry::Clear()
{
    m_objects.clear();
}
}
