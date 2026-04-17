#include "LauncherSettings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace NLS
{
namespace
{
constexpr const char* kDefaultEngineExecutableKey = "default_engine_executable";
constexpr const char* kInstallKeyPrefix = "install.";

std::string Trim(std::string value)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

bool PathsEqual(const std::filesystem::path& left, const std::filesystem::path& right)
{
    std::error_code ec;
    if (std::filesystem::equivalent(left, right, ec))
        return true;

#ifdef _WIN32
    auto normalize = [](std::filesystem::path value)
    {
        auto native = value.lexically_normal().string();
        std::transform(native.begin(), native.end(), native.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return native;
    };
    return normalize(left) == normalize(right);
#else
    return left.lexically_normal() == right.lexically_normal();
#endif
}
}

LauncherSettings::LauncherSettings(std::filesystem::path filePath)
    : m_filePath(std::move(filePath))
{
}

bool LauncherSettings::Load()
{
    m_engineInstallations.clear();
    m_defaultEngineExecutablePath.clear();

    std::ifstream input(m_filePath, std::ios::binary);
    if (!input.is_open())
        return false;

    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;

        const auto separator = line.find('=');
        if (separator == std::string::npos)
            continue;

        const auto key = Trim(line.substr(0, separator));
        auto value = line.substr(separator + 1);
        if (!value.empty() && value.back() == '\r')
            value.pop_back();

        if (key == kDefaultEngineExecutableKey)
        {
            m_defaultEngineExecutablePath = value;
            continue;
        }

        if (key.rfind(kInstallKeyPrefix, 0) == 0 && !value.empty())
        {
            const std::filesystem::path executablePath(value);
            const bool exists = std::any_of(m_engineInstallations.begin(), m_engineInstallations.end(), [&](const LauncherInstallEntry& entry) {
                return PathsEqual(entry.executablePath, executablePath);
            });
            if (!exists)
                m_engineInstallations.push_back({ executablePath.string() });
        }
    }

    EnsureDefaultSelection();
    return true;
}

bool LauncherSettings::Save() const
{
    if (m_filePath.has_parent_path())
        std::filesystem::create_directories(m_filePath.parent_path());

    std::ofstream output(m_filePath, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
        return false;

    output << kDefaultEngineExecutableKey << "=" << m_defaultEngineExecutablePath << "\n";
    for (size_t index = 0; index < m_engineInstallations.size(); ++index)
        output << kInstallKeyPrefix << index << "=" << m_engineInstallations[index].executablePath << "\n";
    return true;
}

bool LauncherSettings::AddEngineExecutablePath(const std::filesystem::path& executablePath)
{
    if (!IsValidEngineExecutablePath(executablePath))
        return false;

    const bool exists = std::any_of(m_engineInstallations.begin(), m_engineInstallations.end(), [&](const LauncherInstallEntry& entry) {
        return PathsEqual(entry.executablePath, executablePath);
    });
    if (!exists)
        m_engineInstallations.push_back({ executablePath.string() });

    if (m_defaultEngineExecutablePath.empty())
        m_defaultEngineExecutablePath = executablePath.string();

    EnsureDefaultSelection();
    return true;
}

bool LauncherSettings::RemoveEngineExecutablePath(const std::filesystem::path& executablePath)
{
    const auto previousSize = m_engineInstallations.size();
    m_engineInstallations.erase(
        std::remove_if(m_engineInstallations.begin(), m_engineInstallations.end(), [&](const LauncherInstallEntry& entry) {
            return PathsEqual(entry.executablePath, executablePath);
        }),
        m_engineInstallations.end());

    if (m_engineInstallations.size() == previousSize)
        return false;

    if (PathsEqual(m_defaultEngineExecutablePath, executablePath))
        m_defaultEngineExecutablePath.clear();

    EnsureDefaultSelection();
    return true;
}

bool LauncherSettings::SetDefaultEngineExecutablePath(const std::filesystem::path& executablePath)
{
    const auto found = std::find_if(m_engineInstallations.begin(), m_engineInstallations.end(), [&](const LauncherInstallEntry& entry) {
        return PathsEqual(entry.executablePath, executablePath);
    });
    if (found == m_engineInstallations.end())
        return false;

    m_defaultEngineExecutablePath = found->executablePath;
    return true;
}

bool LauncherSettings::SetEngineExecutablePath(const std::filesystem::path& executablePath)
{
    if (!AddEngineExecutablePath(executablePath))
        return false;

    return SetDefaultEngineExecutablePath(executablePath);
}

bool LauncherSettings::HasValidDefaultEngineExecutablePath() const
{
    if (m_defaultEngineExecutablePath.empty())
        return false;

    return IsValidEngineExecutablePath(m_defaultEngineExecutablePath);
}

std::vector<LauncherInstallView> LauncherSettings::GetEngineInstallationViews() const
{
    std::vector<LauncherInstallView> views;
    views.reserve(m_engineInstallations.size());

    for (const auto& entry : m_engineInstallations)
    {
        LauncherInstallView view;
        view.executablePath = entry.executablePath;
        view.versionLabel = DescribeEngineVersion(entry.executablePath);
        view.isValid = IsValidEngineExecutablePath(entry.executablePath);
        view.isDefault = !m_defaultEngineExecutablePath.empty() && PathsEqual(entry.executablePath, m_defaultEngineExecutablePath);
        views.push_back(std::move(view));
    }

    return views;
}

bool LauncherSettings::HasAnyValidEngineInstallations() const
{
    return std::any_of(m_engineInstallations.begin(), m_engineInstallations.end(), [](const LauncherInstallEntry& entry) {
        return IsValidEngineExecutablePath(entry.executablePath);
    });
}

bool LauncherSettings::IsValidEngineExecutablePath(const std::filesystem::path& executablePath)
{
    std::error_code ec;
    if (!std::filesystem::exists(executablePath, ec) || std::filesystem::is_directory(executablePath, ec))
        return false;

#ifdef _WIN32
    auto extension = executablePath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (extension != ".exe")
        return false;
#endif

    return true;
}

std::string LauncherSettings::DescribeEngineVersion(const std::filesystem::path& executablePath)
{
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(executablePath, ec);
    if (ec)
        return {};

    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        writeTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(systemTime);

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &timestamp);
#else
    localtime_r(&timestamp, &localTime);
#endif

    std::ostringstream output;
    output << std::put_time(&localTime, "%Y-%m-%d %H:%M");
    return output.str();
}

void LauncherSettings::EnsureDefaultSelection()
{
    if (!m_defaultEngineExecutablePath.empty())
    {
        const auto found = std::find_if(m_engineInstallations.begin(), m_engineInstallations.end(), [&](const LauncherInstallEntry& entry) {
            return PathsEqual(entry.executablePath, m_defaultEngineExecutablePath);
        });
        if (found != m_engineInstallations.end())
        {
            m_defaultEngineExecutablePath = found->executablePath;
            return;
        }
    }

    if (!m_engineInstallations.empty())
    {
        m_defaultEngineExecutablePath = m_engineInstallations.front().executablePath;
        return;
    }

    m_defaultEngineExecutablePath.clear();
}
} // namespace NLS
