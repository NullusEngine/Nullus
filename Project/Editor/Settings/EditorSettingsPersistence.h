#pragma once

#include "Settings/EditorSettingsRegistry.h"

#include <filesystem>
#include <optional>

namespace NLS::Editor::Settings
{
// Launch-only values that older editor settings files stored next to the live
// settings. They now live in the project file and are only read here once to
// migrate an existing installation.
struct EditorLegacyLaunchOptionValues
{
    std::optional<bool> verticalSynchronization;
    std::optional<bool> threadedRendering;
    std::optional<int> renderDocStartupCaptureFrames;
};

class EditorSettingsPersistence
{
public:
    static bool Load(const std::filesystem::path& p_path, const EditorSettingsRegistry& p_registry);
    static bool Save(const std::filesystem::path& p_path, const EditorSettingsRegistry& p_registry);

    // Reads the launch options that previous editor versions persisted in the
    // settings file. Returns empty optionals when the file or the fields are
    // absent, so callers can fall back to the project file or to defaults.
    static EditorLegacyLaunchOptionValues ReadLegacyLaunchOptionValues(const std::filesystem::path& p_path);
};
}
