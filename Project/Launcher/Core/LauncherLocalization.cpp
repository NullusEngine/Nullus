#include "LauncherLocalization.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace NLS
{
namespace
{
std::string Trim(std::string value)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> ParseCsvRow(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;

    for (size_t index = 0; index < line.size(); ++index)
    {
        const char ch = line[index];
        if (ch == '"')
        {
            if (inQuotes && index + 1 < line.size() && line[index + 1] == '"')
            {
                current.push_back('"');
                ++index;
            }
            else
            {
                inQuotes = !inQuotes;
            }
            continue;
        }

        if (ch == ',' && !inQuotes)
        {
            fields.push_back(current);
            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    fields.push_back(current);
    return fields;
}
}

std::string_view LauncherTextKeyName(LauncherTextKey key)
{
    switch (key)
    {
    case LauncherTextKey::LauncherTitle: return "launcher.title";
    case LauncherTextKey::ProjectsTitle: return "projects.title";
    case LauncherTextKey::ProjectsSearchHint: return "projects.search_hint";
    case LauncherTextKey::OpenProject: return "projects.open";
    case LauncherTextKey::NewProject: return "projects.new";
    case LauncherTextKey::CreateProject: return "wizard.create";
    case LauncherTextKey::Cancel: return "common.cancel";
    case LauncherTextKey::Name: return "projects.name";
    case LauncherTextKey::Modified: return "projects.modified";
    case LauncherTextKey::Backend: return "projects.backend";
    case LauncherTextKey::NoProjects: return "projects.empty";
    case LauncherTextKey::OpenProjectMenu: return "projects.menu.open";
    case LauncherTextKey::OpenInExplorer: return "projects.menu.open_explorer";
    case LauncherTextKey::OpenInFiles: return "projects.menu.open_files";
    case LauncherTextKey::AddCommandLineArgs: return "projects.menu.command_line_args";
    case LauncherTextKey::RemoveFromList: return "projects.menu.remove";
    case LauncherTextKey::ProjectNotFound: return "projects.not_found.title";
    case LauncherTextKey::ProjectNotExist: return "projects.not_found.body";
    case LauncherTextKey::OpenProjectFile: return "projects.open_file";
    case LauncherTextKey::Installs: return "installs.title";
    case LauncherTextKey::SelectEngineExecutable: return "installs.select_executable";
    case LauncherTextKey::AddEngineExecutable: return "installs.add_executable";
    case LauncherTextKey::RemoveEngineExecutable: return "installs.remove_executable";
    case LauncherTextKey::DefaultEngineExecutable: return "installs.default_executable";
    case LauncherTextKey::EngineExecutable: return "installs.executable";
    case LauncherTextKey::EngineExecutableHint: return "installs.executable_hint";
    case LauncherTextKey::InvalidEngineExecutable: return "installs.invalid_executable";
    case LauncherTextKey::NoEngineExecutable: return "installs.empty";
    case LauncherTextKey::NoInstalledVersions: return "installs.no_versions";
    case LauncherTextKey::VersionModifiedDate: return "installs.version_modified";
    case LauncherTextKey::InvalidInstalledVersion: return "installs.invalid_version";
    case LauncherTextKey::NewProjectTitle: return "wizard.title";
    case LauncherTextKey::AllTemplates: return "wizard.templates.all";
    case LauncherTextKey::SearchTemplates: return "wizard.templates.search";
    case LauncherTextKey::NoMatchTemplate: return "wizard.templates.no_match";
    case LauncherTextKey::SelectTemplate: return "wizard.templates.select";
    case LauncherTextKey::ProjectSettings: return "wizard.project_settings";
    case LauncherTextKey::ProjectName: return "wizard.project_name";
    case LauncherTextKey::Location: return "wizard.location";
    case LauncherTextKey::EditorVersion: return "wizard.editor_version";
    case LauncherTextKey::Browse: return "common.browse";
    case LauncherTextKey::SelectLocation: return "wizard.select_location";
    case LauncherTextKey::Advanced: return "wizard.advanced";
    case LauncherTextKey::BackendLabel: return "wizard.backend";
    case LauncherTextKey::Resolution: return "wizard.resolution";
    case LauncherTextKey::Samples: return "wizard.samples";
    case LauncherTextKey::PreviewImage: return "wizard.preview_image";
    case LauncherTextKey::NameEmpty: return "wizard.error.name_empty";
    case LauncherTextKey::NameTooLong: return "wizard.error.name_too_long";
    case LauncherTextKey::NameInvalid: return "wizard.error.name_invalid";
    case LauncherTextKey::LocationEmpty: return "wizard.error.location_empty";
    case LauncherTextKey::LocationNotExist: return "wizard.error.location_not_exist";
    case LauncherTextKey::DirExistsPrefix: return "wizard.error.dir_exists_prefix";
    case LauncherTextKey::DirExistsSuffix: return "wizard.error.dir_exists_suffix";
    case LauncherTextKey::ResolutionInvalid: return "wizard.error.resolution_invalid";
    case LauncherTextKey::CreateDirFailed: return "wizard.error.create_dir_failed";
    case LauncherTextKey::CreateFileFailed: return "wizard.error.create_file_failed";
    case LauncherTextKey::WizardEditorVersionRequired: return "wizard.error.editor_version_required";
    case LauncherTextKey::WizardNoEditorVersionsTitle: return "wizard.error.no_editor_versions.title";
    case LauncherTextKey::WizardNoEditorVersionsBody: return "wizard.error.no_editor_versions.body";
    case LauncherTextKey::TimeUnknown: return "time.unknown";
    case LauncherTextKey::TimeSecondsAgo: return "time.seconds_ago";
    case LauncherTextKey::TimeMinutesAgo: return "time.minutes_ago";
    case LauncherTextKey::TimeHoursAgo: return "time.hours_ago";
    case LauncherTextKey::TimeDaysAgo: return "time.days_ago";
    case LauncherTextKey::TimeYearsAgo: return "time.years_ago";
    case LauncherTextKey::MissingText: return "missing.text";
    }

    return "missing.text";
}

std::string NormalizeLocaleName(std::string_view locale)
{
    auto value = Trim(std::string(locale));
    if (value.empty())
        return {};

    const auto modifier = value.find_first_of(".@");
    if (modifier != std::string::npos)
        value.resize(modifier);

    std::replace(value.begin(), value.end(), '_', '-');
    const auto lowered = ToLower(value);
    if (lowered == "c" || lowered == "posix")
        return {};
    if (lowered.rfind("chinese", 0) == 0)
        return "zh-CN";
    if (lowered.rfind("english", 0) == 0)
        return "en-US";

    const auto separator = value.find('-');
    if (separator == std::string::npos)
    {
        if (lowered == "zh")
            return "zh-CN";
        if (lowered == "en")
            return "en-US";
        return lowered;
    }

    auto language = ToLower(value.substr(0, separator));
    auto region = value.substr(separator + 1);
    std::transform(region.begin(), region.end(), region.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    if (language.empty() || region.empty())
        return {};

    return language + "-" + region;
}

bool LauncherLocalization::Load(const std::filesystem::path& resourceRoot, std::string_view locale)
{
    m_text.clear();
    return LoadTable(resourceRoot / "strings.csv", locale);
}

std::string_view LauncherLocalization::Text(LauncherTextKey key) const
{
    const std::string keyName(LauncherTextKeyName(key));
    const auto found = m_text.find(keyName);
    if (found != m_text.end())
        return found->second;

    return LauncherTextKeyName(key);
}

const char* LauncherLocalization::CStr(LauncherTextKey key) const
{
    const std::string keyName(LauncherTextKeyName(key));
    const auto found = m_text.find(keyName);
    if (found != m_text.end())
        return found->second.c_str();

    return LauncherTextKeyName(key).data();
}

bool LauncherLocalization::LoadTable(const std::filesystem::path& filePath, std::string_view locale)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input.is_open())
        return false;

    std::string headerLine;
    if (!std::getline(input, headerLine))
        return false;
    if (!headerLine.empty() && static_cast<unsigned char>(headerLine[0]) == 0xEF)
    {
        if (headerLine.size() >= 3 &&
            static_cast<unsigned char>(headerLine[1]) == 0xBB &&
            static_cast<unsigned char>(headerLine[2]) == 0xBF)
        {
            headerLine.erase(0, 3);
        }
    }

    const auto headers = ParseCsvRow(headerLine);
    if (headers.size() < 2)
        return false;

    const auto normalizedLocale = NormalizeLocaleName(locale);
    size_t englishColumn = std::string::npos;
    size_t selectedColumn = std::string::npos;

    for (size_t index = 1; index < headers.size(); ++index)
    {
        const auto normalizedHeader = NormalizeLocaleName(headers[index]);
        if (normalizedHeader == "en-US")
            englishColumn = index;
        if (!normalizedLocale.empty() && normalizedHeader == normalizedLocale)
            selectedColumn = index;
    }

    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        const auto fields = ParseCsvRow(line);
        if (fields.empty())
            continue;

        const auto key = Trim(fields[0]);
        if (key.empty())
            continue;

        std::string selectedValue;
        if (selectedColumn != std::string::npos && selectedColumn < fields.size())
            selectedValue = fields[selectedColumn];

        std::string englishValue;
        if (englishColumn != std::string::npos && englishColumn < fields.size())
            englishValue = fields[englishColumn];

        if (!selectedValue.empty())
            m_text[key] = selectedValue;
        else if (!englishValue.empty())
            m_text[key] = englishValue;
    }

    return true;
}

LauncherLocalization& GetLauncherLocalization()
{
    static LauncherLocalization localization;
    return localization;
}
} // namespace NLS
