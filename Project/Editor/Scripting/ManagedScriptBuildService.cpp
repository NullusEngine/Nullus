#include "ManagedScriptBuildService.h"
#include "ManagedScriptProjectPaths.h"
#include "Profiling/Profiler.h"

#include <Scripting/ScriptApiManifest.h>
#include <Scripting/ScriptTypes.h>
#include <Json/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>
#include <string_view>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace NLS::Editor::Scripting
{
namespace
{
std::string Quote(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string quoted = "\"";
    for (const char character : value)
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

std::string BuildSourceDigest(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& managedProjectRoot,
    const std::filesystem::path& staging)
{
    std::vector<std::filesystem::path> sources;
    std::error_code error;
    for (const auto& directory : {
        projectRoot / "Assets",
        managedProjectRoot / "Managed" / "Nullus.EngineScripts" / "Scripts",
        managedProjectRoot / "Managed" / "Nullus.GameScripts" / "Scripts"})
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
    // A revision also owns its compiler output and SDK contract. Source-only
    // keys can reuse a loaded name after an SDK or generated API change.
    for (const auto* artifact : {"GameScripts.dll", "GameScripts.pdb", "GameScripts.deps.json",
        "GameScripts.runtimeconfig.json", "Nullus.Managed.dll", "EngineScripts.dll", "Nullus.ScriptGenerator.dll"})
    {
        combined += artifact;
        combined.push_back('\n');
        combined += ReadText(staging / artifact);
        combined.push_back('\n');
    }
    const auto hash = NLS::Scripting::MakeScriptContentHash(combined);
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

#if defined(_WIN32)
std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    UINT codePage = CP_UTF8;
    int required = MultiByteToWideChar(
        codePage,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    // filesystem::path::string() uses the native narrow encoding on MSVC.
    // Fall back to the active code page for project paths that are not UTF-8.
    if (required <= 0)
    {
        codePage = CP_ACP;
        required = MultiByteToWideChar(
            codePage,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
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

std::vector<wchar_t> BuildCleanEnvironmentBlock()
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
            const std::wstring value(entry);
            if (!IsPathEnvironmentEntry(value))
                append(entry);
        }
        FreeEnvironmentStringsW(raw);
    }

    std::vector<wchar_t> path(32768, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"Path", path.data(), static_cast<DWORD>(path.size()));
    if (length > 0 && length < path.size())
    {
        std::wstring pathEntry = L"Path=";
        pathEntry.append(path.data(), length);
        append(pathEntry.c_str());
    }

    // CreateProcess requires a double-NUL-terminated environment block.
    block.push_back(L'\0');
    return block;
}

int RunBuildCommand(const std::string& command, std::string& error)
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
    auto environment = BuildCleanEnvironmentBlock();

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
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
int RunBuildCommand(const std::string& command, std::string&)
{
    const int status = std::system(command.c_str());
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}
#endif
}

ManagedScriptBuildService::~ManagedScriptBuildService()
{
    if (m_activeBuild.valid())
        m_activeBuild.wait();
}

std::future<ManagedScriptBuildResult> ManagedScriptBuildService::Start(ManagedScriptBuildRequest request)
{
    if (m_activeBuild.valid())
        m_activeBuild.wait();
    m_activeBuild = std::async(std::launch::async, [request = std::move(request)]
    {
        NLS_PROFILE_REGISTER_THREAD("Managed Script Build");
        return Build(request);
    });
    return std::future<ManagedScriptBuildResult>(
        std::move(m_activeBuild));
}

std::filesystem::path ManagedScriptBuildService::ResolveRepositoryDotnet(
    const std::filesystem::path& projectRoot)
{
    if (const auto* configured = std::getenv("NLS_DOTNET_EXECUTABLE");
        configured != nullptr && configured[0] != '\0')
    {
        return configured;
    }

    std::error_code error;
    auto normalizedRoot = std::filesystem::weakly_canonical(projectRoot, error);
    if (error)
        normalizedRoot = projectRoot.lexically_normal();
    for (auto root = normalizedRoot; !root.empty(); root = root.parent_path())
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
        if (root == root.parent_path())
            break;
    }
    // CMake accepts a pinned host SDK when the repository-local bootstrap is
    // absent. Keep the editor build service consistent with that resolution.
    return "dotnet";
}

std::string ManagedScriptBuildService::BuildCommand(const ManagedScriptBuildRequest& request)
{
    const auto projectPaths = ResolveManagedScriptProject(request.projectRoot);
    const auto project = projectPaths.has_value()
        ? projectPaths->projectFile
        : request.projectRoot / "Managed" / "Nullus.GameScripts" / "Nullus.GameScripts.csproj";
    const auto generatorProject = projectPaths.has_value()
        ? projectPaths->managedProjectRoot / "Managed" / "Nullus.ScriptGenerator" / "Nullus.ScriptGenerator.csproj"
        : request.projectRoot / "Managed" / "Nullus.ScriptGenerator" / "Nullus.ScriptGenerator.csproj";
    const auto managedProject = projectPaths.has_value()
        ? projectPaths->managedProjectRoot / "Managed" / "Nullus.Managed" / "Nullus.Managed.csproj"
        : request.projectRoot / "Managed" / "Nullus.Managed" / "Nullus.Managed.csproj";
    const auto engineProject = projectPaths.has_value()
        ? projectPaths->managedProjectRoot / "Managed" / "Nullus.EngineScripts" / "Nullus.EngineScripts.csproj"
        : request.projectRoot / "Managed" / "Nullus.EngineScripts" / "Nullus.EngineScripts.csproj";
    // Project editor scripts compile into their own assembly beside the runtime
    // one, so editor-only code never reaches the player build.
    const auto editorScriptsProject = projectPaths.has_value()
        ? projectPaths->managedProjectRoot / "Managed" / "Nullus.GameScriptsEditor" / "Nullus.GameScriptsEditor.csproj"
        : request.projectRoot / "Managed" / "Nullus.GameScriptsEditor" / "Nullus.GameScriptsEditor.csproj";
    const auto outputRoot = request.outputRoot.empty()
        ? request.projectRoot / "Library" / "ScriptBuild" / "Managed"
        : request.outputRoot;
    const auto staging = outputRoot / ".staging";
    const auto diagnostics = outputRoot / "last-failure.sarif";
    const auto dotnet = request.dotnetPath.empty()
        ? ResolveRepositoryDotnet(request.projectRoot)
        : request.dotnetPath;
    const auto cliHome = outputRoot / ".dotnet-home";
    const auto schemaArgument = request.scriptApiManifest.empty()
        ? std::string{}
        : " -p:NullusScriptApiManifest=" + Quote(request.scriptApiManifest);
#if defined(_WIN32)
    const auto environmentPrefix =
        "set \"DOTNET_CLI_HOME=" + cliHome.string() + "\" && "
        "set \"DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1\" && "
        "set \"DOTNET_NOLOGO=1\" && "
        "set \"DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0\" && ";
#else
    const auto environmentPrefix =
        "DOTNET_CLI_HOME=" + Quote(cliHome) + " "
        "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 "
        "DOTNET_NOLOGO=1 "
        "DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0 ";
#endif
    const auto generatorBuild = environmentPrefix + Quote(dotnet) + " build " + Quote(generatorProject) +
        " --configuration " + request.configuration +
        " --no-restore --no-incremental --nologo --output " + Quote(staging) +
        " -p:ErrorLog=" + Quote(outputRoot / "generator.sarif");
    const auto engineBuild = Quote(dotnet) + " build " + Quote(engineProject) +
        " --configuration " + request.configuration +
        " --no-restore --no-incremental --nologo --output " + Quote(staging) +
        " -p:NullusUsePrebuiltScriptGenerator=true" +
        " -p:NullusForceScriptGenerator=true" + schemaArgument +
        " -p:NullusScriptGeneratorPath=" + Quote(staging / "Nullus.ScriptGenerator.dll") +
        " -p:NullusUseScriptAnalysis=false" +
        " -p:ErrorLog=" + Quote(outputRoot / "engine.sarif");
    const auto managedBuild = Quote(dotnet) + " build " + Quote(managedProject) +
        " --configuration " + request.configuration +
        " --no-restore --no-incremental --nologo --output " + Quote(staging) +
        " -p:NullusUsePrebuiltScriptGenerator=true" +
        " -p:NullusForceScriptGenerator=true" + schemaArgument +
        " -p:NullusScriptGeneratorPath=" + Quote(staging / "Nullus.ScriptGenerator.dll") +
        " -p:NullusUseScriptAnalysis=false" +
        " -p:ErrorLog=" + Quote(outputRoot / "managed.sarif");
    const auto gameBuild = Quote(dotnet) + " build " + Quote(project) +
        " --configuration " + request.configuration +
        " --no-restore --no-incremental --nologo" +
        " -p:NullusProjectAssets=" + Quote(request.projectRoot / "Assets") +
        " -p:NullusUsePrebuiltScriptGenerator=true" +
        " -p:NullusForceScriptGenerator=true" + schemaArgument +
        " -p:NullusScriptGeneratorPath=" + Quote(staging / "Nullus.ScriptGenerator.dll") +
        " -p:NullusEngineScriptsPath=" + Quote(staging / "EngineScripts.dll") +
        " -p:NullusUseScriptAnalysis=false" +
        " -p:DebugType=portable -p:DebugSymbols=true -p:Optimize=false" +
        " -p:Deterministic=true -p:OutputPath=" + Quote(staging) +
        " -p:ErrorLog=" + Quote(diagnostics);
    const auto editorScriptsBuild = Quote(dotnet) + " build " + Quote(editorScriptsProject) +
        " --configuration " + request.configuration +
        " --no-restore --no-incremental --nologo -m:1" +
        " -p:NullusProjectAssets=" + Quote(request.projectRoot / "Assets") +
        " -p:NullusGameScriptsPath=" + Quote(staging / "GameScripts.dll") +
        " -p:NullusUsePrebuiltScriptGenerator=true" +
        " -p:NullusForceScriptGenerator=true" + schemaArgument +
        " -p:NullusScriptGeneratorPath=" + Quote(staging / "Nullus.ScriptGenerator.dll") +
        " -p:NullusUseScriptAnalysis=false" +
        " -p:DebugType=portable -p:DebugSymbols=true -p:Optimize=false" +
        " -p:Deterministic=true -p:OutputPath=" + Quote(staging) +
        " -p:ErrorLog=" + Quote(outputRoot / "editor-scripts.sarif");
    return generatorBuild + " && " + environmentPrefix + managedBuild +
        " && " + environmentPrefix + engineBuild +
        " && " + environmentPrefix + gameBuild +
        " && " + environmentPrefix + editorScriptsBuild;
}

ManagedScriptBuildResult ManagedScriptBuildService::Build(const ManagedScriptBuildRequest& request)
{
    ManagedScriptBuildResult result;
    result.projectRoot = request.projectRoot;
    if (request.projectRoot.empty())
    {
        result.output = "Managed script build requires a project root.";
        return result;
    }

    std::string schemaContents;
    if (!request.scriptApiManifest.empty())
    {
        schemaContents = ReadText(request.scriptApiManifest);
        NLS::Scripting::ScriptApiDatabase schema;
        const auto status = NLS::Scripting::ScriptApiManifest::FromJson(schemaContents, schema);
        if (!status.Succeeded() || (!request.expectedSchemaHash.empty() &&
            schema.GetSchemaHashHex() != request.expectedSchemaHash))
        {
            result.output = "The C# build Script API manifest does not match the Editor runtime: " +
                request.scriptApiManifest.string() + (status.Succeeded() ? std::string{} : " (" + status.message + ")");
            return result;
        }
    }

    const auto outputRoot = request.outputRoot.empty()
        ? request.projectRoot / "Library" / "ScriptBuild" / "Managed"
        : request.outputRoot;
    const auto staging = outputRoot / ".staging";
    const auto logPath = outputRoot / "last-build.log";
    result.sarifDiagnostics = outputRoot / "last-failure.sarif";
    result.sarifDiagnosticFiles = { outputRoot / "generator.sarif", outputRoot / "managed.sarif",
        outputRoot / "engine.sarif", result.sarifDiagnostics, outputRoot / "editor-scripts.sarif" };
    std::error_code error;
    std::filesystem::create_directories(staging, error);
    if (error)
    {
        result.output = "Unable to create managed build output: " + error.message();
        return result;
    }

    // Do not let a failed/incomplete build accidentally reuse an artifact from
    // an earlier source version. Keep the staging directory stable, but clear
    // only the build-owned files that this transaction replaces.
    for (const auto* artifact : {
        "GameScripts.dll",
        "GameScripts.pdb",
        "GameScripts.deps.json",
        "GameScripts.runtimeconfig.json",
        "GameScriptsEditor.dll",
        "GameScriptsEditor.pdb",
        "GameScriptsEditor.deps.json",
        "Nullus.Editor.dll",
        "Nullus.Editor.pdb",
        "Nullus.Editor.deps.json",
        "EngineScripts.dll",
        "EngineScripts.pdb",
        "EngineScripts.deps.json",
        "EngineScripts.runtimeconfig.json",
        "Nullus.Managed.dll",
        "Nullus.Managed.pdb",
        "Nullus.Managed.deps.json"})
    {
        std::filesystem::remove(staging / artifact, error);
        error.clear();
    }
    for (const auto& diagnosticFile : result.sarifDiagnosticFiles)
    {
        std::filesystem::remove(diagnosticFile, error);
        error.clear();
    }

    const auto dotnet = request.dotnetPath.empty()
        ? ResolveRepositoryDotnet(request.projectRoot)
        : request.dotnetPath;
    if (dotnet != std::filesystem::path("dotnet") && !std::filesystem::exists(dotnet))
    {
        result.output = "Repository-local .NET SDK was not found: " + dotnet.string();
        return result;
    }

    // A failed early step must not leave the previous transaction's output in
    // place. Redirection belongs to the whole command group, not just the last
    // build in the chain.
    {
        std::ofstream log(logPath, std::ios::binary | std::ios::trunc);
        if (!log)
        {
            result.output = "Unable to create managed build log: " + logPath.string();
            return result;
        }
    }
    const auto diagnostics = outputRoot / "last-failure.sarif";
    const auto command = "(" + BuildCommand(request) + ") > " + Quote(logPath) + " 2>&1";
    std::string launchError;
    result.exitCode = RunBuildCommand(command, launchError);
    result.output = ReadText(logPath);
    if (!launchError.empty())
        result.output += "\n" + launchError;
    result.sarifDiagnostics = diagnostics;
    if (result.exitCode != 0)
        return result;
    if (!request.scriptApiManifest.empty() && ReadText(request.scriptApiManifest) != schemaContents)
    {
        result.output += "\nThe Script API manifest changed during C# compilation; rebuild the scripts.";
        result.exitCode = -1;
        return result;
    }

    const auto projectPaths = ResolveManagedScriptProject(request.projectRoot);
    const auto digest = BuildSourceDigest(
        request.projectRoot,
        projectPaths.has_value() ? projectPaths->managedProjectRoot : request.projectRoot,
        staging);
    const auto filesEqual = [](const std::filesystem::path& left, const std::filesystem::path& right)
    {
        std::error_code leftError;
        std::error_code rightError;
        if (!std::filesystem::is_regular_file(left, leftError)
            || !std::filesystem::is_regular_file(right, rightError)
            || leftError
            || rightError
            || std::filesystem::file_size(left, leftError) != std::filesystem::file_size(right, rightError)
            || leftError
            || rightError)
            return false;
        std::ifstream leftStream(left, std::ios::binary);
        std::ifstream rightStream(right, std::ios::binary);
        return leftStream.good()
            && rightStream.good()
            && std::equal(
                std::istreambuf_iterator<char>(leftStream),
                std::istreambuf_iterator<char>(),
                std::istreambuf_iterator<char>(rightStream));
    };
    const auto copyArtifact = [&](std::string_view extension, std::filesystem::path& destination)
    {
        const auto source = staging / ("GameScripts" + std::string(extension));
        if (!std::filesystem::exists(source))
        {
            error = std::make_error_code(std::errc::no_such_file_or_directory);
            return false;
        }
        destination = outputRoot / ("GameScripts." + digest + std::string(extension));
        // CoreCLR may keep an identical published revision mapped. It does
        // not need a write, and Windows correctly refuses to replace it.
        if (filesEqual(source, destination)) { error.clear(); return true; }
        return std::filesystem::copy_file(source, destination,
            std::filesystem::copy_options::overwrite_existing, error);
    };
    if (!copyArtifact(".dll", result.assembly) ||
        !copyArtifact(".pdb", result.symbols) ||
        !copyArtifact(".deps.json", result.deps) ||
        !copyArtifact(".runtimeconfig.json", result.runtimeConfig))
    {
        result.output += "\nManaged build could not publish a GameScripts artifact: " + error.message();
        result.exitCode = -1;
        return result;
    }

    // The content-addressed GameScripts artifact is loaded directly by
    // hostfxr. Keep its portable runtime dependency beside it; otherwise the
    // deps.json probe succeeds but type resolution fails with a misleading
    // COR_E_TYPEINITIALIZATION error. Nullus.Managed is shared by all project
    // versions, so it is intentionally copied with the current artifact.
    // Roslyn records the unversioned `GameScripts.pdb` file name in the PE
    // CodeView entry before Nullus gives the artifacts their content-addressed
    // names. Keep a current-symbol alias beside the loaded assembly so VS can
    // resolve symbols after attaching to a collectible ALC. The versioned PDB
    // remains the authoritative artifact used by reload bookkeeping.
    const auto currentSymbols = outputRoot / "GameScripts.pdb";
    if (!std::filesystem::copy_file(
            staging / "GameScripts.pdb",
            currentSymbols,
            std::filesystem::copy_options::overwrite_existing,
            error))
    {
        error.clear();
        if (!filesEqual(staging / "GameScripts.pdb", currentSymbols))
        {
            result.output += "\nManaged build completed but the current GameScripts PDB alias could not be updated.";
            result.exitCode = -1;
            return result;
        }
    }

    // EngineScripts is a fixed dependency of every GameScripts artifact. Keep
    // it beside the content-addressed assembly so hostfxr and collectible ALC
    // resolution can load the built-in registry without another search path.
    const auto copyEngineArtifact = [&](const char* artifact)
    {
        const auto source = staging / artifact;
        const auto destination = outputRoot / artifact;
        if (!std::filesystem::is_regular_file(source))
            return false;
        if (std::filesystem::copy_file(
                source,
                destination,
                std::filesystem::copy_options::overwrite_existing,
                error))
            return true;
        error.clear();
        return filesEqual(source, destination);
    };
    for (const auto* artifact : {
        "EngineScripts.dll",
        "EngineScripts.pdb",
        "EngineScripts.deps.json",
        "EngineScripts.runtimeconfig.json"})
    {
        if (!copyEngineArtifact(artifact))
        {
            result.output += "\nManaged build completed but the EngineScripts dependency was missing.";
            result.exitCode = -1;
            return result;
        }
    }

    const auto copyRuntimeDependency = [&](const char* artifact)
    {
        const auto source = staging / artifact;
        const auto destination = outputRoot / artifact;
        if (!std::filesystem::is_regular_file(source))
            return false;
        if (std::filesystem::copy_file(
                source,
                destination,
                std::filesystem::copy_options::overwrite_existing,
                error))
            return true;
        error.clear();
        // The currently active CoreCLR can keep the previous shared runtime
        // file open. It is safe to continue only when that locked copy is byte
        // identical to the build output.
        return filesEqual(source, destination);
    };
    for (const auto* artifact : {"Nullus.Managed.dll", "Nullus.Managed.pdb", "Nullus.Managed.deps.json",
        "Nullus.Editor.dll", "Nullus.Editor.pdb", "Nullus.Editor.deps.json"})
    {
        if (!copyRuntimeDependency(artifact))
        {
            result.output += "\nManaged build could not publish dependency '" + std::string(artifact) + "'.";
            result.exitCode = -1;
            return result;
        }
    }

    // The editor resolves the project editor-scripts assembly beside the loaded
    // scripting artifact, matching how Nullus.Managed is deployed. A project
    // without an Editor directory simply produces no such assembly.
    for (const auto* artifact : {
        "GameScriptsEditor.dll",
        "GameScriptsEditor.pdb",
        "GameScriptsEditor.deps.json"})
    {
        std::error_code presenceError;
        if (!std::filesystem::is_regular_file(staging / artifact, presenceError))
            continue;
        if (!copyRuntimeDependency(artifact))
        {
            result.output += "\nManaged build completed but the GameScriptsEditor artifact was missing.";
            result.exitCode = -1;
            return result;
        }
    }
    result.succeeded = true;
    return result;
}

std::vector<ManagedBuildDiagnostic> ManagedScriptBuildService::ReadDiagnostics(const ManagedScriptBuildResult& result)
{
    using Json = nlohmann::json;
    std::vector<ManagedBuildDiagnostic> diagnostics;
    std::unordered_set<std::string> seen;
    auto files = result.sarifDiagnosticFiles;
    if (files.empty() && !result.sarifDiagnostics.empty()) files.push_back(result.sarifDiagnostics);
    const auto decodeUri = [](std::string uri)
    {
        if (uri.rfind("file://", 0) == 0) uri.erase(0, 7);
#if defined(_WIN32)
        if (uri.size() >= 3 && uri[0] == '/' && uri[2] == ':') uri.erase(0, 1);
#endif
        const auto digit = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string decoded;
        for (size_t i = 0; i < uri.size(); ++i)
        {
            if (uri[i] == '%' && i + 2 < uri.size())
            {
                const int high = digit(uri[i + 1]);
                const int low = digit(uri[i + 2]);
                if (high >= 0 && low >= 0)
                {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(uri[i]);
        }
        return decoded;
    };
    for (const auto& file : files)
    {
        const auto content = ReadText(file);
        if (content.empty()) continue;
        const auto document = Json::parse(content, nullptr, false);
        if (document.is_discarded() || !document.is_object()) continue;
        try
        {
            const auto runs = document.value("runs", Json::array());
            if (!runs.is_array()) continue;
            for (const auto& run : runs)
            {
                const auto results = run.value("results", Json::array());
                if (!results.is_array()) continue;
                for (const auto& item : results)
                {
                    if (!item.is_object()) continue;
                    ManagedBuildDiagnostic diagnostic;
                    const auto level = item.value("level", std::string("warning"));
                    diagnostic.severity = level == "error" ? ManagedBuildDiagnostic::Severity::Error :
                        level == "warning" ? ManagedBuildDiagnostic::Severity::Warning : ManagedBuildDiagnostic::Severity::Info;
                    if (item.contains("message"))
                    {
                        const auto& message = item["message"];
                        if (message.is_string()) diagnostic.message = message.get<std::string>();
                        else if (message.is_object()) diagnostic.message = message.value("text", std::string{});
                    }
                    const auto code = item.value("ruleId", std::string{});
                    if (!code.empty()) diagnostic.message = code + ": " + diagnostic.message;
                    const auto locations = item.value("locations", Json::array());
                    if (locations.is_array() && !locations.empty() && locations[0].is_object())
                    {
                        const auto& location = locations[0];
                        const Json* physical = &location;
                        if (location.contains("physicalLocation") && location["physicalLocation"].is_object()) physical = &location["physicalLocation"];
                        else if (location.contains("resultFile") && location["resultFile"].is_object()) physical = &location["resultFile"];
                        std::string uri;
                        if (physical->contains("artifactLocation") && (*physical)["artifactLocation"].is_object())
                            uri = (*physical)["artifactLocation"].value("uri", std::string{});
                        else uri = physical->value("uri", std::string{});
                        if (!uri.empty())
                        {
                            auto path = std::filesystem::u8path(decodeUri(uri));
                            if (path.is_relative() && !result.projectRoot.empty()) path = result.projectRoot / path;
                            const auto utf8 = path.lexically_normal().generic_u8string();
                            diagnostic.sourcePath.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
                        }
                        if (physical->contains("region") && (*physical)["region"].is_object())
                        {
                            diagnostic.line = (*physical)["region"].value("startLine", 0);
                            diagnostic.column = (*physical)["region"].value("startColumn", 0);
                        }
                    }
                    if (diagnostic.message.empty()) continue;
                    const auto key = std::to_string(static_cast<int>(diagnostic.severity)) + "\n" +
                        diagnostic.message + "\n" + diagnostic.sourcePath + "\n" +
                        std::to_string(diagnostic.line) + ":" + std::to_string(diagnostic.column);
                    if (seen.insert(key).second) diagnostics.push_back(std::move(diagnostic));
                }
            }
        }
        catch (const std::exception&)
        {
            // A malformed or partial diagnostics file must not hide a build failure.
        }
    }
    const bool hasError = std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic)
        { return diagnostic.severity == ManagedBuildDiagnostic::Severity::Error; });
    if (!result.succeeded && !hasError)
    {
        ManagedBuildDiagnostic diagnostic;
        diagnostic.message = result.output.empty()
            ? "Managed script compilation failed (exit code " + std::to_string(result.exitCode) + ")."
            : result.output;
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}
}
