#pragma once

#include "Settings/EditorSettingObject.h"

#include <string>
#include <string_view>
#include <vector>

namespace NLS::Editor::Settings
{
class EditorSettingsRegistry
{
public:
    bool Register(EditorSettingObject p_object);
    bool Contains(std::string_view p_id) const;
    const EditorSettingObject* Find(std::string_view p_id) const;
    const std::vector<EditorSettingObject>& GetObjects() const;
    std::vector<const EditorSettingObject*> Search(std::string_view p_query) const;
    std::vector<std::string> GetCategories() const;
    void Clear();

private:
    std::vector<EditorSettingObject> m_objects;
};

std::string NormalizeSettingsSearchText(std::string_view p_text);
}
