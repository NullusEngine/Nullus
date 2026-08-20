#include "ExternalCodeEditor.h"

#include <Platform/Process/Process.h>
#include <Platform/Utils/SystemCalls.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <optional>
#include <system_error>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace NLS::Editor::Debug
{
namespace
{
bool IsCSharpSource(const std::filesystem::path& sourcePath)
{
    const auto extension = sourcePath.extension().string();
    return extension.size() == 3u &&
        (extension[1] == 'c' || extension[1] == 'C') &&
        (extension[2] == 's' || extension[2] == 'S');
}

#ifdef _WIN32
std::optional<std::filesystem::path> FindVisualStudio2022()
{
    constexpr wchar_t kVisualStudioKey[] = L"SOFTWARE\\Microsoft\\VisualStudio\\SxS\\VS7";

    const auto readRegistryInstallPath = [&kVisualStudioKey](REGSAM view) -> std::optional<std::filesystem::path>
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kVisualStudioKey, 0, KEY_READ | view, &key) != ERROR_SUCCESS)
            return std::nullopt;

        DWORD type = 0;
        DWORD byteCount = 0;
        const auto queryResult = RegQueryValueExW(key, L"17.0", nullptr, &type, nullptr, &byteCount);
        if (queryResult != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || byteCount < sizeof(wchar_t))
        {
            RegCloseKey(key);
            return std::nullopt;
        }

        std::vector<wchar_t> buffer((byteCount / sizeof(wchar_t)) + 1u, L'\0');
        if (RegQueryValueExW(
                key,
                L"17.0",
                nullptr,
                &type,
                reinterpret_cast<LPBYTE>(buffer.data()),
                &byteCount) != ERROR_SUCCESS)
        {
            RegCloseKey(key);
            return std::nullopt;
        }
        RegCloseKey(key);

        const std::filesystem::path installPath(buffer.data());
        const auto devenv = installPath / "Common7" / "IDE" / "devenv.exe";
        std::error_code fileError;
        if (std::filesystem::is_regular_file(devenv, fileError) && !fileError)
            return devenv;
        return std::nullopt;
    };

    // An Editor process can be 32-bit or 64-bit, so inspect both registry
    // views. The registry is authoritative and also handles non-C: installs.
    if (const auto path = readRegistryInstallPath(KEY_WOW64_64KEY); path.has_value())
        return path;
    if (const auto path = readRegistryInstallPath(KEY_WOW64_32KEY); path.has_value())
        return path;

    std::vector<std::filesystem::path> programFiles;
    if (const auto* root = _wgetenv(L"ProgramFiles"); root != nullptr)
        programFiles.emplace_back(root);
    if (const auto* root = _wgetenv(L"ProgramFiles(x86)"); root != nullptr)
        programFiles.emplace_back(root);

    // Some Visual Studio installations do not publish the SxS value (for
    // example, a side-by-side install on a non-system drive). Scan the normal
    // Program Files layout on mounted drives as a deterministic fallback. This
    // is deliberately a filesystem probe, never a PATH lookup or shell call.
    const DWORD logicalDrives = GetLogicalDrives();
    for (unsigned int index = 0; index < 26u; ++index)
    {
        if ((logicalDrives & (1u << index)) == 0u)
            continue;
        wchar_t driveRoot[] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
        programFiles.emplace_back(std::filesystem::path(driveRoot) / "Program Files");
        programFiles.emplace_back(std::filesystem::path(driveRoot) / "Program Files (x86)");
    }

    constexpr wchar_t kEdition[] = L"2022";
    constexpr const wchar_t* kProducts[] = {L"Community", L"Professional", L"Enterprise", L"BuildTools"};
    for (const auto& root : programFiles)
    {
        for (const auto* product : kProducts)
        {
            const auto candidate = root / "Microsoft Visual Studio" / kEdition / product /
                "Common7" / "IDE" / "devenv.exe";
            std::error_code fileError;
            if (std::filesystem::is_regular_file(candidate, fileError) && !fileError)
                return candidate;
        }
    }
    return std::nullopt;
}

struct VisualStudioWindowMatch
{
    HWND window = nullptr;
    std::filesystem::path executable;
};

std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value;
}

bool ContainsInsensitive(const std::wstring_view value, const std::wstring_view needle)
{
    if (needle.empty())
        return false;
    const auto loweredValue = ToLower(std::wstring(value));
    const auto loweredNeedle = ToLower(std::wstring(needle));
    return loweredValue.find(loweredNeedle) != std::wstring::npos;
}

bool QueryProcessExecutable(const DWORD processId, std::filesystem::path& executable)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
        return false;

    wchar_t buffer[MAX_PATH] = {};
    DWORD bufferLength = static_cast<DWORD>(std::size(buffer));
    const bool queried = QueryFullProcessImageNameW(process, 0, buffer, &bufferLength) != FALSE;
    CloseHandle(process);
    if (!queried)
        return false;
    executable = std::filesystem::path(std::wstring(buffer, bufferLength));
    return true;
}

struct VisualStudioWindowSearch
{
    std::wstring projectName;
    VisualStudioWindowMatch match;
};

BOOL CALLBACK FindVisualStudioProjectWindowProc(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<VisualStudioWindowSearch*>(parameter);
    if (!IsWindowVisible(window))
        return TRUE;

    wchar_t title[1024] = {};
    const int titleLength = GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (titleLength <= 0 || !ContainsInsensitive(std::wstring_view(title, static_cast<size_t>(titleLength)), search.projectName))
        return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    std::filesystem::path executable;
    if (!QueryProcessExecutable(processId, executable) ||
        !ContainsInsensitive(executable.filename().wstring(), L"devenv.exe"))
    {
        return TRUE;
    }

    search.match = {window, std::move(executable)};
    return FALSE;
}

std::optional<VisualStudioWindowMatch> FindVisualStudioProjectWindow(
    const std::filesystem::path& projectFile)
{
    const auto projectName = projectFile.stem().wstring();
    if (projectName.empty())
        return std::nullopt;

    VisualStudioWindowSearch search {projectName, {}};
    EnumWindows(FindVisualStudioProjectWindowProc, reinterpret_cast<LPARAM>(&search));
    if (search.match.window == nullptr)
        return std::nullopt;
    return search.match;
}
#endif
}

std::vector<std::string> ExternalCodeEditor::BuildVisualStudioCodeArguments(
    const std::filesystem::path& sourcePath,
    int line,
    int column)
{
    const int safeLine = (std::max)(line, 1);
    const int safeColumn = (std::max)(column, 1);
    return {"--reuse-window", "--goto", sourcePath.string() + ":" +
        std::to_string(safeLine) + ":" + std::to_string(safeColumn)};
}

ExternalCodeEditorResult ExternalCodeEditor::Open(
    const std::filesystem::path& sourcePath,
    int line,
    int column,
    const std::filesystem::path& visualStudioCodePath)
{
    ExternalCodeEditorResult result;
    if (sourcePath.empty())
    {
        result.errorMessage = "Cannot open an empty script source path.";
        return result;
    }

    if (!visualStudioCodePath.empty())
    {
        const auto launch = NLS::Platform::Process::Launch(
            visualStudioCodePath,
            BuildVisualStudioCodeArguments(sourcePath, line, column));
        if (launch.success)
        {
            result.success = true;
            result.usedVisualStudioCode = true;
            return result;
        }
        result.errorMessage = launch.errorMessage;
    }

#ifdef _WIN32
    // VS 2022 is the managed C# fallback only. Lua uses VS Code/LuaPanda or
    // the user's .lua file association and must not open as a C# project.
    if (IsCSharpSource(sourcePath))
    {
        if (const auto visualStudioPath = FindVisualStudio2022(); visualStudioPath.has_value())
        {
            const auto launch = NLS::Platform::Process::Launch(
                *visualStudioPath,
                {"/Edit", sourcePath.string()});
            if (launch.success)
            {
                result.success = true;
                result.usedVisualStudio = true;
                return result;
            }
            result.errorMessage = launch.errorMessage;
        }
    }
#endif

    // Keep the final fallback deliberately free of PATH or shell interpolation.
    // It remains useful on platforms without Visual Studio.
    if (std::filesystem::exists(sourcePath))
    {
        NLS::Platform::SystemCalls::OpenFile(sourcePath.string());
        result.success = true;
        result.usedVisualStudioCode = false;
        return result;
    }

    if (result.errorMessage.empty())
        result.errorMessage = "Script source does not exist: " + sourcePath.string();
    else
        result.errorMessage += "; script source does not exist: " + sourcePath.string();
    return result;
}

ExternalCodeEditorResult ExternalCodeEditor::OpenProject(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& projectFile,
    const std::filesystem::path& visualStudioCodePath,
    const std::filesystem::path& sourcePath)
{
    ExternalCodeEditorResult result;
    if (projectRoot.empty() || projectFile.empty())
    {
        result.errorMessage = "Cannot open an empty managed project path.";
        return result;
    }
    if (!std::filesystem::exists(projectRoot))
    {
        result.errorMessage = "Managed project root does not exist: " + projectRoot.string();
        return result;
    }
    if (!std::filesystem::exists(projectFile))
    {
        result.errorMessage = "Managed project file does not exist: " + projectFile.string();
        return result;
    }

    if (!visualStudioCodePath.empty())
    {
        const auto launch = NLS::Platform::Process::Launch(
            visualStudioCodePath,
            {"--reuse-window", projectRoot.string()});
        if (launch.success)
        {
            result.success = true;
            result.usedVisualStudioCode = true;
            return result;
        }
        result.errorMessage = launch.errorMessage;
    }

#ifdef _WIN32
    if (const auto visualStudioPath = FindVisualStudio2022(); visualStudioPath.has_value())
    {
        if (!sourcePath.empty())
        {
            const auto matchingVisualStudio = FindVisualStudioProjectWindow(projectFile);
            if (matchingVisualStudio.has_value())
            {
                // Activate the matching solution before /Edit. This prevents
                // devenv from routing the file to an unrelated open VS window.
                ShowWindow(matchingVisualStudio->window, SW_RESTORE);
                SetForegroundWindow(matchingVisualStudio->window);
                const auto launch = NLS::Platform::Process::Launch(
                    matchingVisualStudio->executable.empty()
                        ? *visualStudioPath
                        : matchingVisualStudio->executable,
                    {"/Edit", sourcePath.string()});
                if (launch.success)
                {
                    result.success = true;
                    result.usedVisualStudio = true;
                    return result;
                }
                result.errorMessage = launch.errorMessage;
            }
        }

        std::vector<std::string> arguments;
        // devenv parses the first project/file operand as the workspace to
        // load. Put it first so File.OpenFile runs after the project system
        // has evaluated the linked Assets/**/*.cs items; otherwise the source
        // is opened as a Miscellaneous File even though it belongs to the
        // generated project.
        arguments.emplace_back(projectFile.string());
        if (!sourcePath.empty())
        {
            // Open the project first, then ask the same devenv instance to
            // open the source after the project is loaded. This avoids a
            // second standalone VS window and does not rely on /Edit's
            // running-instance timing semantics.
            arguments.emplace_back("/Command");
            const auto source = sourcePath.string();
            const bool sourceHasWhitespace =
                source.find_first_of(" \t\r\n") != std::string::npos;
            arguments.emplace_back(
                "File.OpenFile " +
                (sourceHasWhitespace ? "\"" + source + "\"" : source));
        }
        const auto launch = NLS::Platform::Process::Launch(*visualStudioPath, arguments);
        if (launch.success)
        {
            result.success = true;
            result.usedVisualStudio = true;
            return result;
        }
        result.errorMessage = launch.errorMessage;
    }
#endif

    // Opening through the OS association is the last resort only. This avoids
    // silently selecting an older Visual Studio when VS 2022 is installed.
    NLS::Platform::SystemCalls::OpenFile(projectFile.string(), projectRoot.string());
    result.success = true;
    result.usedVisualStudioCode = false;
    return result;
}

ExternalCodeEditorResult ExternalCodeEditor::OpenWorkspace(
    const std::filesystem::path& workspaceRoot,
    const std::filesystem::path& visualStudioCodePath)
{
    ExternalCodeEditorResult result;
    if (workspaceRoot.empty())
    {
        result.errorMessage = "Cannot open an empty workspace path.";
        return result;
    }
    if (!std::filesystem::exists(workspaceRoot))
    {
        result.errorMessage = "Workspace root does not exist: " + workspaceRoot.string();
        return result;
    }

    if (!visualStudioCodePath.empty())
    {
        const auto launch = NLS::Platform::Process::Launch(
            visualStudioCodePath,
            {"--reuse-window", workspaceRoot.string()});
        if (launch.success)
        {
            result.success = true;
            result.usedVisualStudioCode = true;
            return result;
        }
        result.errorMessage = launch.errorMessage;
    }

    NLS::Platform::SystemCalls::OpenFile(workspaceRoot.string());
    result.success = true;
    result.usedVisualStudioCode = false;
    return result;
}

ExternalCodeEditorResult ExternalCodeEditor::OpenDebugWorkspace(
    const ProjectDebugManifest& manifest,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& visualStudioCodePath)
{
    ExternalCodeEditorResult result;
    const auto workspace = !visualStudioCodePath.empty()
        ? manifest.visualStudioCodeWorkspace
        : manifest.visualStudioSolution;
    if (workspace.empty() || !std::filesystem::is_regular_file(workspace))
    {
        result.errorMessage = "Generated Nullus IDE workspace does not exist: " + workspace.string();
        return result;
    }

    if (!visualStudioCodePath.empty())
    {
        std::vector<std::string> arguments = {"--reuse-window", workspace.string()};
        if (!sourcePath.empty())
        {
            arguments.emplace_back("--goto");
            arguments.emplace_back(sourcePath.string() + ":1:1");
        }
        const auto launch = NLS::Platform::Process::Launch(visualStudioCodePath, arguments);
        if (!launch.success)
        {
            result.errorMessage = launch.errorMessage;
            return result;
        }
        result.success = true;
        result.usedVisualStudioCode = true;
        return result;
    }

#ifdef _WIN32
    const auto visualStudioPath = FindVisualStudio2022();
    if (!visualStudioPath.has_value())
    {
        result.errorMessage = "Visual Studio 2022 was not found for the Nullus project workspace.";
        return result;
    }
    std::vector<std::string> arguments = {workspace.string()};
    if (!sourcePath.empty())
    {
        arguments.emplace_back("/Command");
        const auto source = sourcePath.string();
        const bool sourceHasWhitespace = source.find_first_of(" \t\r\n") != std::string::npos;
        arguments.emplace_back("File.OpenFile " + (sourceHasWhitespace ? "\"" + source + "\"" : source));
    }
    const auto launch = NLS::Platform::Process::Launch(*visualStudioPath, arguments);
    if (!launch.success)
    {
        result.errorMessage = launch.errorMessage;
        return result;
    }
    result.success = true;
    result.usedVisualStudio = true;
    return result;
#else
    result.errorMessage = "Visual Studio project workspaces are supported on Windows; configure VS Code for this platform.";
    return result;
#endif
}
}
