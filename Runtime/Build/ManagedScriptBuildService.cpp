#include "ManagedScriptBuildService.h"

#include "Assets/ArtifactManifest.h"
#include "Platform/Process/Process.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace NLS::Build
{
namespace
{
struct ManagedScriptProjectPaths
{
    std::filesystem::path projectRoot;
    std::filesystem::path managedRoot;
    std::filesystem::path projectFile;
};

std::string Quote(const std::filesystem::path& path)
{
    std::string quoted = "\"";
    for (const char character : path.string())
    {
        if (character == '"')
            quoted += '\\';
        quoted += character;
    }
    quoted += '"';
    return quoted;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::optional<ManagedScriptProjectPaths> ResolveManagedScriptProject(
    const std::filesystem::path& projectRoot)
{
    if (projectRoot.empty())
        return std::nullopt;

    std::error_code error;
    auto normalizedRoot = std::filesystem::weakly_canonical(projectRoot, error);
    if (error)
        normalizedRoot = projectRoot.lexically_normal();

    std::vector<std::filesystem::path> probes;
    for (auto candidate = normalizedRoot; !candidate.empty(); candidate = candidate.parent_path())
    {
        probes.push_back(candidate);
        if (candidate == candidate.parent_path())
            break;
    }
    const auto resourceRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
    for (auto candidate = resourceRoots.installRoot; !candidate.empty(); candidate = candidate.parent_path())
    {
        probes.push_back(candidate);
        if (candidate == candidate.parent_path())
            break;
    }

    for (const auto& candidate : probes)
    {
        const auto projectFile =
            candidate / "Managed" / "Nullus.GameScripts" / "Nullus.GameScripts.csproj";
        if (std::filesystem::is_regular_file(projectFile, error))
        {
            return ManagedScriptProjectPaths {
                normalizedRoot,
                candidate,
                projectFile};
        }
    }
    return std::nullopt;
}

std::string BuildSourceDigest(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& managedRoot)
{
    std::vector<std::filesystem::path> sources;
    std::error_code error;
    for (const auto& directory : {
        projectRoot / "Assets",
        managedRoot / "Managed" / "Nullus.EngineScripts" / "Scripts"})
    {
        if (!std::filesystem::is_directory(directory, error))
            continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error))
        {
            if (error)
                break;
            if (entry.is_regular_file(error) && entry.path().extension() == ".cs")
                sources.push_back(entry.path());
        }
    }
    std::sort(sources.begin(), sources.end());

    std::string combined;
    for (const auto& source : sources)
    {
        combined += source.lexically_relative(projectRoot).generic_string();
        combined.push_back('\n');
        combined += ReadText(source);
        combined.push_back('\n');
    }
    const auto bytes = NLS::Core::Assets::ComputeSha256(
        reinterpret_cast<const uint8_t*>(combined.data()),
        combined.size());
    return NLS::Core::Assets::ToHexString(bytes);
}

#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string_view value)
{
    if (value.empty())
        return {};
    int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (required <= 0)
    {
        codePage = CP_ACP;
        required = MultiByteToWideChar(
            codePage, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (required <= 0)
        return {};
    std::wstring output(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(
        codePage,
        codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        required);
    return output;
}

bool IsPathEnvironmentEntry(const std::wstring& entry)
{
    const auto separator = entry.find(L'=');
    if (separator == std::wstring::npos)
        return false;
    const auto name = entry.substr(0, separator);
    return name.size() == 4 &&
        (name[0] == L'P' || name[0] == L'p') &&
        (name[1] == L'A' || name[1] == L'a') &&
        (name[2] == L'T' || name[2] == L't') &&
        (name[3] == L'H' || name[3] == L'h');
}

std::vector<wchar_t> BuildEnvironmentBlock()
{
    std::vector<wchar_t> block;
    const auto append = [&block](const wchar_t* value)
    {
        const auto length = wcslen(value);
        block.insert(block.end(), value, value + length);
        block.push_back(L'\0');
    };

    if (LPWCH raw = GetEnvironmentStringsW())
    {
        for (const wchar_t* entry = raw; *entry != L'\0'; entry += wcslen(entry) + 1)
        {
            if (!IsPathEnvironmentEntry(entry))
                append(entry);
        }
        FreeEnvironmentStringsW(raw);
    }
    if (LPWCH raw = GetEnvironmentStringsW())
    {
        for (const wchar_t* entry = raw; *entry != L'\0'; entry += wcslen(entry) + 1)
        {
            if (IsPathEnvironmentEntry(entry))
                append(entry);
        }
        FreeEnvironmentStringsW(raw);
    }
    block.push_back(L'\0');
    return block;
}

int RunCommand(const std::string& command, std::string& error)
{
    const auto wideCommand = Utf8ToWide(command);
    if (wideCommand.empty())
    {
        error = "Unable to convert the managed build command to UTF-16.";
        return -1;
    }
    wchar_t comspec[MAX_PATH] = {};
    DWORD comspecLength = GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
    if (comspecLength == 0 || comspecLength >= MAX_PATH)
        wcscpy_s(comspec, L"C:\\Windows\\System32\\cmd.exe");

    std::wstring commandLine = L"\"";
    commandLine += comspec;
    commandLine += L"\" /d /c \"";
    commandLine += wideCommand;
    commandLine += L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    auto environment = BuildEnvironmentBlock();

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo {};
    if (!CreateProcessW(
        comspec,
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        environment.data(),
        nullptr,
        &startupInfo,
        &processInfo))
    {
        error = "Unable to start the managed build process (Win32 error " +
            std::to_string(GetLastError()) + ").";
        return -1;
    }
    WaitForSingleObject(processInfo.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return static_cast<int>(exitCode);
}
#else
int RunCommand(const std::string& command, std::string&)
{
    return std::system(command.c_str());
}
#endif
} // namespace

std::filesystem::path ManagedScriptBuildService::ResolveRepositoryDotnet(
    const std::filesystem::path& projectRoot)
{
    if (const auto* configured = std::getenv("NLS_DOTNET_EXECUTABLE");
        configured != nullptr && configured[0] != '\0')
    {
        return configured;
    }

    std::vector<std::filesystem::path> probes;
    const auto resourceRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
    for (auto candidate = resourceRoots.installRoot; !candidate.empty(); candidate = candidate.parent_path())
    {
        probes.push_back(candidate);
        if (candidate == candidate.parent_path())
            break;
    }
    std::error_code error;
    for (auto candidate = projectRoot; !candidate.empty(); candidate = candidate.parent_path())
    {
        probes.push_back(candidate);
        if (candidate == candidate.parent_path())
            break;
    }
    for (const auto& root : probes)
    {
#if defined(_WIN32)
        const auto dotnet = root / "Tools" / "Dotnet" / "windows" / "x64" / "dotnet.exe";
#elif defined(__APPLE__)
        const auto dotnet = root / "Tools" / "Dotnet" / "macos" / "x64" / "dotnet";
#else
        const auto dotnet = root / "Tools" / "Dotnet" / "linux" / "x64" / "dotnet";
#endif
        if (std::filesystem::is_regular_file(dotnet, error))
            return dotnet;
    }
    return "dotnet";
}

std::string ManagedScriptBuildService::BuildCommand(const ManagedScriptBuildRequest& request)
{
    const auto paths = ResolveManagedScriptProject(request.projectRoot);
    if (!paths.has_value())
        return {};

    const auto generatorProject = paths->managedRoot / "Managed" / "Nullus.ScriptGenerator" / "Nullus.ScriptGenerator.csproj";
    const auto managedProject = paths->managedRoot / "Managed" / "Nullus.Managed" / "Nullus.Managed.csproj";
    const auto engineProject = paths->managedRoot / "Managed" / "Nullus.EngineScripts" / "Nullus.EngineScripts.csproj";
    const auto gameProject = paths->projectFile;
    const auto outputRoot = request.outputRoot.empty()
        ? request.projectRoot / "Library" / "ScriptBuild" / "ProjectManaged"
        : request.outputRoot;
    const auto staging = outputRoot / ".staging";
    const auto cliHome = outputRoot / ".dotnet-home";
    const auto dotnet = request.dotnetPath.empty()
        ? ResolveRepositoryDotnet(request.projectRoot)
        : request.dotnetPath;

#if defined(_WIN32)
    const auto environment =
        "set \"DOTNET_CLI_HOME=" + cliHome.string() + "\" && "
        "set \"DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1\" && "
        "set \"DOTNET_NOLOGO=1\" && "
        "set \"DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0\" && ";
#else
    const auto environment =
        "DOTNET_CLI_HOME=" + Quote(cliHome) + " "
        "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 "
        "DOTNET_NOLOGO=1 "
        "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0 ";
#endif

    const auto restore = [&](const std::filesystem::path& project)
    {
        return environment + Quote(dotnet) + " restore " + Quote(project) + " --nologo";
    };
    const auto build = [&](const std::filesystem::path& project, const std::string& extra)
    {
        return Quote(dotnet) + " build " + Quote(project) +
            " --configuration " + request.configuration +
            " --no-restore --no-incremental --nologo --output " + Quote(staging) + extra;
    };

    const auto generatorExtra =
        " -p:NullusUsePrebuiltScriptGenerator=true"
        " -p:NullusScriptGeneratorPath=" + Quote(staging / "Nullus.ScriptGenerator.dll");
    const auto baseExtra =
        " -p:NullusUsePrebuiltScriptGenerator=true"
        " -p:NullusScriptGeneratorPath=" + Quote(staging / "Nullus.ScriptGenerator.dll") +
        " -p:NullusUseScriptAnalysis=false";
    const auto gameExtra =
        " -p:NullusProjectAssets=" + Quote(request.projectRoot / "Assets") +
        " -p:NullusUsePrebuiltScriptGenerator=true"
        " -p:NullusScriptGeneratorPath=" + Quote(staging / "Nullus.ScriptGenerator.dll") +
        " -p:NullusEngineScriptsPath=" + Quote(staging / "EngineScripts.dll") +
        " -p:NullusUseScriptAnalysis=false"
        " -p:DebugType=portable -p:DebugSymbols=true" +
        std::string(request.configuration == "Release" ? " -p:Optimize=true" : " -p:Optimize=false") +
        " -p:Deterministic=true -p:OutputPath=" + Quote(staging);

    return restore(generatorProject) + " && " + restore(managedProject) +
        " && " + restore(engineProject) + " && " + restore(gameProject) +
        " && " + environment + build(generatorProject, generatorExtra) +
        " && " + environment + build(managedProject, baseExtra) +
        " && " + environment + build(engineProject, baseExtra) +
        " && " + environment + build(gameProject, gameExtra);
}

ManagedScriptBuildResult ManagedScriptBuildService::Build(const ManagedScriptBuildRequest& request)
{
    ManagedScriptBuildResult result;
    if (request.projectRoot.empty())
    {
        result.output = "Managed script build requires a project root.";
        return result;
    }
    const auto paths = ResolveManagedScriptProject(request.projectRoot);
    if (!paths.has_value())
    {
        result.output = "No Managed/Nullus.GameScripts project was found for the project.";
        return result;
    }

    const auto outputRoot = request.outputRoot.empty()
        ? request.projectRoot / "Library" / "ScriptBuild" / "ProjectManaged"
        : request.outputRoot;
    const auto staging = outputRoot / ".staging";
    const auto logPath = outputRoot / "last-build.log";
    std::error_code error;
    std::filesystem::remove_all(staging, error);
    std::filesystem::create_directories(staging, error);
    if (error)
    {
        result.output = "Unable to create managed build output: " + error.message();
        return result;
    }
    std::filesystem::remove(outputRoot / "last-failure.sarif", error);
    error.clear();

    const auto dotnet = request.dotnetPath.empty()
        ? ResolveRepositoryDotnet(request.projectRoot)
        : request.dotnetPath;
    if (dotnet != std::filesystem::path("dotnet") && !std::filesystem::exists(dotnet))
    {
        result.output = "Repository-local .NET SDK was not found: " + dotnet.string();
        return result;
    }

    const auto command = BuildCommand(request);
    if (command.empty())
    {
        result.output = "Managed script build command could not be constructed.";
        return result;
    }
    std::string launchError;
    result.exitCode = RunCommand(command + " > " + Quote(logPath) + " 2>&1", launchError);
    result.output = ReadText(logPath);
    if (!launchError.empty())
        result.output += "\n" + launchError;
    if (result.exitCode != 0)
        return result;

    const auto require = [&](const char* name)
    {
        const auto path = staging / name;
        if (!std::filesystem::is_regular_file(path, error))
        {
            result.output += "\nManaged build completed but '" + std::string(name) + "' was missing.";
            result.exitCode = -1;
            return std::filesystem::path {};
        }
        return path;
    };
    result.assembly = require("GameScripts.dll");
    result.deps = require("GameScripts.deps.json");
    result.runtimeConfig = require("GameScripts.runtimeconfig.json");
    result.engineAssembly = require("EngineScripts.dll");
    result.engineDeps = require("EngineScripts.deps.json");
    result.engineRuntimeConfig = require("EngineScripts.runtimeconfig.json");
    result.managedAssembly = require("Nullus.Managed.dll");
    if (result.assembly.empty() || result.deps.empty() || result.runtimeConfig.empty() ||
        result.engineAssembly.empty() || result.engineDeps.empty() ||
        result.engineRuntimeConfig.empty() || result.managedAssembly.empty())
    {
        return result;
    }

    // Keep the built assembly name stable for Scripts/GameScripts.dll while
    // recording the source digest in the build log for diagnostics.
    result.output += "\nManaged script source digest: " +
        BuildSourceDigest(request.projectRoot, paths->managedRoot);
    result.succeeded = true;
    return result;
}
} // namespace NLS::Build
