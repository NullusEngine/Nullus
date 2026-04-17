#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace NLS
{
struct LauncherInstallEntry
{
    std::string executablePath;
};

struct LauncherInstallView
{
    std::string executablePath;
    std::string versionLabel;
    bool isValid = false;
    bool isDefault = false;
};

class LauncherSettings
{
public:
    explicit LauncherSettings(std::filesystem::path filePath);

    bool Load();
    bool Save() const;

    bool AddEngineExecutablePath(const std::filesystem::path& executablePath);
    bool RemoveEngineExecutablePath(const std::filesystem::path& executablePath);
    bool SetDefaultEngineExecutablePath(const std::filesystem::path& executablePath);

    const std::vector<LauncherInstallEntry>& GetEngineInstallations() const { return m_engineInstallations; }
    const std::string& GetDefaultEngineExecutablePath() const { return m_defaultEngineExecutablePath; }
    bool HasValidDefaultEngineExecutablePath() const;

    bool SetEngineExecutablePath(const std::filesystem::path& executablePath);
    const std::string& GetEngineExecutablePath() const { return m_defaultEngineExecutablePath; }
    bool HasValidEngineExecutablePath() const { return HasValidDefaultEngineExecutablePath(); }

    std::vector<LauncherInstallView> GetEngineInstallationViews() const;
    bool HasAnyValidEngineInstallations() const;

    static bool IsValidEngineExecutablePath(const std::filesystem::path& executablePath);
    static std::string DescribeEngineVersion(const std::filesystem::path& executablePath);

private:
    void EnsureDefaultSelection();

    std::filesystem::path m_filePath;
    std::vector<LauncherInstallEntry> m_engineInstallations;
    std::string m_defaultEngineExecutablePath;
};
} // namespace NLS
