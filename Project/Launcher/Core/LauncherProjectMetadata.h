#pragma once

#include <filesystem>
#include <string>

namespace NLS
{
std::filesystem::path ResolveProjectSettingsFilePath(const std::filesystem::path& projectPath);
bool WriteProjectLastEditorExecutable(const std::filesystem::path& projectPath, const std::string& editorExecutablePath);
std::string ReadProjectLastEditorExecutable(const std::filesystem::path& projectPath);
std::string DescribeProjectLastEditorVersion(const std::filesystem::path& projectPath);
} // namespace NLS
