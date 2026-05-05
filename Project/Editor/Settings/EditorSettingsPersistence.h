#pragma once

#include "Settings/EditorSettingsRegistry.h"

#include <filesystem>

namespace NLS::Editor::Settings
{
class EditorSettingsPersistence
{
public:
    static bool Load(const std::filesystem::path& p_path, const EditorSettingsRegistry& p_registry);
    static bool Save(const std::filesystem::path& p_path, const EditorSettingsRegistry& p_registry);
};
}
