#include "LauncherProjectMetadata.h"

#include "LauncherSettings.h"

#include <Core/Filesystem/IniFile.h>

namespace NLS
{
namespace
{
constexpr const char* kLastEditorExecutableKey = "last_editor_executable";

std::filesystem::path FindProjectSettingsFilePath(const std::filesystem::path& projectPath)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(projectPath, ec) && projectPath.extension() == ".nullus")
        return projectPath;

    if (!std::filesystem::is_directory(projectPath, ec))
        return {};

    const auto canonicalNamePath = projectPath / (projectPath.filename().string() + ".nullus");
    if (std::filesystem::exists(canonicalNamePath, ec))
        return canonicalNamePath;

    for (const auto& entry : std::filesystem::directory_iterator(projectPath, ec))
    {
        if (ec)
            break;

        if (entry.is_regular_file(ec) && entry.path().extension() == ".nullus")
            return entry.path();
    }

    return {};
}
}

std::filesystem::path ResolveProjectSettingsFilePath(const std::filesystem::path& projectPath)
{
    return FindProjectSettingsFilePath(projectPath);
}

bool WriteProjectLastEditorExecutable(const std::filesystem::path& projectPath, const std::string& editorExecutablePath)
{
    const auto settingsFilePath = FindProjectSettingsFilePath(projectPath);
    if (settingsFilePath.empty())
        return false;

    Filesystem::IniFile settings(settingsFilePath.string());
    if (!editorExecutablePath.empty())
    {
        if (!settings.Set(kLastEditorExecutableKey, editorExecutablePath))
            settings.Add(kLastEditorExecutableKey, editorExecutablePath);
    }
    else
        settings.Remove(kLastEditorExecutableKey);
    settings.Rewrite();
    return true;
}

std::string ReadProjectLastEditorExecutable(const std::filesystem::path& projectPath)
{
    const auto settingsFilePath = FindProjectSettingsFilePath(projectPath);
    if (settingsFilePath.empty())
        return {};

    Filesystem::IniFile settings(settingsFilePath.string());
    return settings.GetOrDefault<std::string>(kLastEditorExecutableKey, "");
}

std::string DescribeProjectLastEditorVersion(const std::filesystem::path& projectPath)
{
    const auto editorExecutablePath = ReadProjectLastEditorExecutable(projectPath);
    if (editorExecutablePath.empty())
        return {};

    return LauncherSettings::DescribeEngineVersion(editorExecutablePath);
}
} // namespace NLS
