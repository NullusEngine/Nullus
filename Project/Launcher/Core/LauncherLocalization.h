#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace NLS
{
enum class LauncherTextKey
{
    LauncherTitle,
    ProjectsTitle,
    ProjectsSearchHint,
    OpenProject,
    NewProject,
    CreateProject,
    Cancel,
    Name,
    Modified,
    Backend,
    NoProjects,
    OpenProjectMenu,
    OpenInExplorer,
    OpenInFiles,
    AddCommandLineArgs,
    RemoveFromList,
    ProjectNotFound,
    ProjectNotExist,
    OpenProjectFile,
    Installs,
    SelectEngineExecutable,
    AddEngineExecutable,
    RemoveEngineExecutable,
    DefaultEngineExecutable,
    EngineExecutable,
    EngineExecutableHint,
    InvalidEngineExecutable,
    NoEngineExecutable,
    NoInstalledVersions,
    VersionModifiedDate,
    InvalidInstalledVersion,
    NewProjectTitle,
    AllTemplates,
    SearchTemplates,
    NoMatchTemplate,
    SelectTemplate,
    ProjectSettings,
    ProjectName,
    Location,
    EditorVersion,
    Browse,
    SelectLocation,
    Advanced,
    BackendLabel,
    Resolution,
    Samples,
    PreviewImage,
    NameEmpty,
    NameTooLong,
    NameInvalid,
    LocationEmpty,
    LocationNotExist,
    DirExistsPrefix,
    DirExistsSuffix,
    ResolutionInvalid,
    CreateDirFailed,
    CreateFileFailed,
    WizardEditorVersionRequired,
    WizardNoEditorVersionsTitle,
    WizardNoEditorVersionsBody,
    TimeUnknown,
    TimeSecondsAgo,
    TimeMinutesAgo,
    TimeHoursAgo,
    TimeDaysAgo,
    TimeYearsAgo,
    MissingText
};

class LauncherLocalization
{
public:
    bool Load(const std::filesystem::path& resourceRoot, std::string_view locale);
    std::string_view Text(LauncherTextKey key) const;
    const char* CStr(LauncherTextKey key) const;

private:
    bool LoadTable(const std::filesystem::path& filePath, std::string_view locale);

    std::unordered_map<std::string, std::string> m_text;
};

std::string_view LauncherTextKeyName(LauncherTextKey key);
std::string NormalizeLocaleName(std::string_view locale);
LauncherLocalization& GetLauncherLocalization();
} // namespace NLS
