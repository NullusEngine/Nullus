#include "ScriptRuntimeSetup.h"

#include "CoreClrScriptBackend.h"
#include "Gen/MetaGenerated.h"
#include "LuaScriptBackend.h"
#include "ScriptApiManifest.h"

#include <Debug/Logger.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace NLS::Scripting
{
namespace
{
std::filesystem::path EnvironmentPath(const char* name)
{
    const auto* value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

std::vector<std::filesystem::path> SearchRoots(const std::filesystem::path& projectRoot)
{
    std::vector<std::filesystem::path> roots;
    const auto appendAncestors = [&roots](std::filesystem::path root)
    {
        if (root.empty())
            return;
        std::error_code error;
        root = std::filesystem::weakly_canonical(root, error);
        if (error)
            root = root.lexically_normal();
        for (;;)
        {
            if (std::find(roots.begin(), roots.end(), root) == roots.end())
                roots.push_back(root);
            const auto parent = root.parent_path();
            if (parent == root || parent.empty())
                break;
            root = parent;
        }
    };

    appendAncestors(projectRoot);
    appendAncestors(std::filesystem::current_path());
    return roots;
}

std::filesystem::path FirstExisting(
    const std::vector<std::filesystem::path>& candidates,
    bool directory)
{
    for (const auto& candidate : candidates)
    {
        if (candidate.empty())
            continue;
        std::error_code error;
        const bool exists = directory
            ? std::filesystem::is_directory(candidate, error)
            : std::filesystem::is_regular_file(candidate, error);
        if (exists && !error)
            return candidate;
    }
    return {};
}

std::filesystem::path ResolveScriptApiDirectory(const ScriptRuntimeSetupOptions& options)
{
    if (!options.scriptApiDirectory.empty())
        return options.scriptApiDirectory;
    if (const auto environment = EnvironmentPath("NLS_SCRIPT_API_DIRECTORY"); !environment.empty())
        return environment;

    std::vector<std::filesystem::path> candidates;
    for (const auto& root : SearchRoots(options.projectRoot))
        candidates.push_back(root / "Library" / "ScriptApi");
    return FirstExisting(candidates, true);
}

std::filesystem::path ResolveDotnetRoot(
    const ScriptRuntimeSetupOptions& options,
    const std::vector<std::filesystem::path>& roots)
{
    if (!options.dotnetRoot.empty())
        return options.dotnetRoot;
    for (const char* name : {
        "NLS_DOTNET_ROOT",
        "DOTNET_ROOT_X64",
        "DOTNET_ROOT",
        "DOTNET_ROOT(x86)"})
    {
        if (const auto environment = EnvironmentPath(name); !environment.empty())
            return environment;
    }

#if defined(_WIN32)
    constexpr const char* platform = "windows";
    constexpr const char* architecture = "x64";
#elif defined(__APPLE__)
    constexpr const char* platform = "macos";
    constexpr const char* architecture = "x64";
#else
    constexpr const char* platform = "linux";
    constexpr const char* architecture = "x64";
#endif

    std::vector<std::filesystem::path> candidates;
    for (const auto& root : roots)
        candidates.push_back(root / "Tools" / "Dotnet" / platform / architecture);
    return FirstExisting(candidates, true);
}

void ResolveManagedArtifacts(
    const ScriptRuntimeSetupOptions& options,
    const std::vector<std::filesystem::path>& roots,
    std::filesystem::path& runtimeConfig,
    std::filesystem::path& assembly)
{
    if (!options.managedRuntimeConfig.empty() && !options.managedAssembly.empty())
    {
        runtimeConfig = options.managedRuntimeConfig;
        assembly = options.managedAssembly;
        return;
    }

    if (const auto config = EnvironmentPath("NLS_MANAGED_RUNTIMECONFIG");
        !config.empty())
    {
        runtimeConfig = config;
    }
    if (const auto managed = EnvironmentPath("NLS_GAMESCRIPTS_ASSEMBLY");
        !managed.empty())
    {
        assembly = managed;
    }

    std::vector<std::filesystem::path> outputDirectories;
    constexpr const char* configurations[] = {"Debug", "RelWithDebInfo", "Release"};
    for (const auto& root : roots)
    {
        // Prefer artifacts produced by the active CMake build.  Older
        // checkouts may still contain build-scripting-vs5 output with a
        // different Script API schema; selecting it first makes the CoreCLR
        // handshake fail even though a current assembly is available.
        std::vector<std::filesystem::path> buildRoots;
        // The default Visual Studio/Ninja build writes managed artifacts
        // directly below <repo>/build/Managed/<config>.  Keep this explicit
        // path in addition to the legacy per-build-directory scan below;
        // treating "Managed" itself as a build root would otherwise produce
        // the invalid <repo>/build/Managed/Managed/<config> path.
        for (const auto configuration : configurations)
            outputDirectories.push_back(root / "build" / "Managed" / configuration);
        const auto buildDirectory = root / "build";
        std::error_code buildError;
        for (const auto& entry : std::filesystem::directory_iterator(buildDirectory, buildError))
        {
            if (!buildError && entry.is_directory(buildError))
                buildRoots.push_back(entry.path());
            buildError.clear();
        }
        std::sort(buildRoots.begin(), buildRoots.end());
        for (const auto& buildRoot : buildRoots)
        {
            for (const auto configuration : configurations)
                outputDirectories.push_back(buildRoot / "Managed" / configuration);
        }
        for (const auto configuration : configurations)
        {
            outputDirectories.push_back(root / "build-scripting-vs5" / "Managed" / configuration);
            outputDirectories.push_back(root / "Managed" / "Nullus.GameScripts" / "bin" / configuration / "net8.0");
        }
        // Debug builds initiated by the Editor are content-addressed and live
        // in one stable directory instead of timestamped folders.
        outputDirectories.push_back(root / "Library" / "ScriptBuild" / "Managed");
    }
    const auto findHashedPair = [](const std::filesystem::path& directory)
        -> std::pair<std::filesystem::path, std::filesystem::path>
    {
        constexpr std::string_view prefix = "GameScripts.";
        constexpr std::string_view configSuffix = ".runtimeconfig.json";
        std::error_code error;
        std::filesystem::path newestConfig;
        std::filesystem::path newestAssembly;
        std::filesystem::file_time_type newestTime{};
        for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        {
            if (error || !entry.is_regular_file(error))
                continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(prefix, 0) != 0 || name.size() <= prefix.size() + configSuffix.size() ||
                name.compare(name.size() - configSuffix.size(), configSuffix.size(), configSuffix) != 0)
                continue;
            const auto hash = name.substr(prefix.size(), name.size() - prefix.size() - configSuffix.size());
            if (hash.empty())
                continue;
            const auto assembly = directory / (std::string(prefix) + hash + ".dll");
            if (!std::filesystem::is_regular_file(assembly, error))
            {
                error.clear();
                continue;
            }
            const auto time = entry.last_write_time(error);
            if (error)
            {
                error.clear();
                continue;
            }
            if (newestConfig.empty() || time > newestTime)
            {
                newestConfig = entry.path();
                newestAssembly = assembly;
                newestTime = time;
            }
        }
        return {newestConfig, newestAssembly};
    };
    const auto findExactPair = [](const std::filesystem::path& directory)
        -> std::pair<std::filesystem::path, std::filesystem::path>
    {
        const auto runtimeConfig = directory / "GameScripts.runtimeconfig.json";
        const auto assembly = directory / "GameScripts.dll";
        std::error_code error;
        if (std::filesystem::is_regular_file(runtimeConfig, error) && !error &&
            std::filesystem::is_regular_file(assembly, error) && !error)
            return {runtimeConfig, assembly};
        return {};
    };
    const auto findArtifact = [](const std::filesystem::path& directory, std::string_view suffix)
        -> std::filesystem::path
    {
        std::error_code error;
        std::filesystem::path newest;
        std::filesystem::file_time_type newestTime{};
        for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        {
            if (error || !entry.is_regular_file(error))
                continue;
            const auto name = entry.path().filename().string();
            if (name.rfind("GameScripts.", 0) != 0 ||
                name.size() < suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            const auto time = entry.last_write_time(error);
            if (!error && (newest.empty() || time > newestTime))
            {
                newest = entry.path();
                newestTime = time;
            }
            error.clear();
        }
        return newest;
    };
    const auto findSelfContainedPair = [&](const std::filesystem::path& directory)
        -> std::pair<std::filesystem::path, std::filesystem::path>
    {
        auto pair = findHashedPair(directory);
        if (pair.first.empty() || std::filesystem::is_regular_file(directory / "Nullus.Managed.dll"))
            return pair;

        // Older Editor builds promoted only GameScripts.<hash>.* and left the
        // portable Nullus.Managed dependency in .staging. Repair that cache in
        // place before loading the stable artifact, so a later background build
        // can freely replace .staging without the running Editor locking it.
        const auto staging = directory / ".staging";
        const auto stagedAssembly = findArtifact(staging, ".dll");
        const auto stagedRuntimeConfig = findArtifact(staging, ".runtimeconfig.json");
        if (!stagedAssembly.empty()
            && !stagedRuntimeConfig.empty()
            && std::filesystem::is_regular_file(staging / "Nullus.Managed.dll"))
        {
            bool repaired = true;
            for (const auto* artifact : {"Nullus.Managed.dll", "Nullus.Managed.pdb", "Nullus.Managed.deps.json"})
            {
                const auto source = staging / artifact;
                if (!std::filesystem::is_regular_file(source))
                    continue;
                std::error_code copyError;
                if (!std::filesystem::copy_file(
                        source,
                        directory / artifact,
                        std::filesystem::copy_options::overwrite_existing,
                        copyError)
                    && !std::filesystem::is_regular_file(directory / artifact))
                {
                    repaired = false;
                    break;
                }
            }
            if (repaired && std::filesystem::is_regular_file(directory / "Nullus.Managed.dll"))
                return pair;
            // A locked or read-only cache can still be opened through staging;
            // this is a compatibility fallback for older project directories.
            return {stagedRuntimeConfig, stagedAssembly};
        }
        return pair;
    };
    // Several build roots can coexist during an Editor upgrade. Select the
    // newest complete pair atomically. Never combine a runtimeconfig from one
    // directory with a DLL from another directory: hostfxr resolves the
    // dependency graph relative to the runtimeconfig and that mismatch can
    // silently load the managed default registry (empty Behaviour manifest).
    std::filesystem::path newestProjectStableRuntimeConfig;
    std::filesystem::path newestProjectStableAssembly;
    std::filesystem::file_time_type newestProjectStableRuntimeTime{};
    std::filesystem::path newestProjectCachedRuntimeConfig;
    std::filesystem::path newestProjectCachedAssembly;
    std::filesystem::file_time_type newestProjectCachedRuntimeTime{};
    std::filesystem::path newestFallbackRuntimeConfig;
    std::filesystem::path newestFallbackAssembly;
    std::filesystem::file_time_type newestFallbackRuntimeTime{};
    const auto canonicalProjectRoot = [&]()
    {
        std::error_code error;
        auto root = std::filesystem::weakly_canonical(options.projectRoot, error);
        return error ? options.projectRoot.lexically_normal() : root;
    }();
    const auto isAncestor = [](const std::filesystem::path& ancestor, const std::filesystem::path& child)
    {
        if (ancestor.empty() || child.empty())
            return false;
        const auto relative = child.lexically_relative(ancestor);
        if (relative.empty() || relative == ".")
            return false;
        const auto first = relative.begin();
        return first == relative.end() || *first != "..";
    };
    const auto findRepositoryRoot = [](std::filesystem::path path)
    {
        std::error_code error;
        path = std::filesystem::weakly_canonical(path, error);
        if (error)
            return std::filesystem::path{};
        for (;;)
        {
            if (std::filesystem::is_directory(path / "Library" / "ScriptApi", error))
                return path;
            error.clear();
            const auto parent = path.parent_path();
            if (parent == path || parent.empty())
                break;
            path = parent;
        }
        return std::filesystem::path{};
    };
    for (const auto& directory : outputDirectories)
    {
        auto pair = findSelfContainedPair(directory);
        if (pair.first.empty())
            pair = findExactPair(directory);
        if (pair.first.empty() || pair.second.empty())
            continue;

        std::error_code timeError;
        const auto time = std::filesystem::last_write_time(pair.first, timeError);
        if (timeError)
            continue;
        const auto normalizedDirectory = directory.generic_string();
        const bool isProjectCache = normalizedDirectory.find("/Library/ScriptBuild/Managed") != std::string::npos;
        const bool belongsToProject = isAncestor(canonicalProjectRoot, directory);
        const auto repositoryRoot = findRepositoryRoot(directory);
        const bool repositoryBelongsToProject = !repositoryRoot.empty() &&
            (repositoryRoot == canonicalProjectRoot || isAncestor(repositoryRoot, canonicalProjectRoot));
        auto* selectedRuntimeConfig = &newestFallbackRuntimeConfig;
        auto* selectedAssembly = &newestFallbackAssembly;
        auto* selectedRuntimeTime = &newestFallbackRuntimeTime;
        if (isProjectCache && belongsToProject)
        {
            selectedRuntimeConfig = &newestProjectCachedRuntimeConfig;
            selectedAssembly = &newestProjectCachedAssembly;
            selectedRuntimeTime = &newestProjectCachedRuntimeTime;
        }
        else if (!isProjectCache && repositoryBelongsToProject)
        {
            selectedRuntimeConfig = &newestProjectStableRuntimeConfig;
            selectedAssembly = &newestProjectStableAssembly;
            selectedRuntimeTime = &newestProjectStableRuntimeTime;
        }
        if (!selectedRuntimeConfig->empty() && time <= *selectedRuntimeTime)
            continue;
        *selectedRuntimeConfig = pair.first;
        *selectedAssembly = pair.second;
        *selectedRuntimeTime = time;
    }
    const auto& selectedRuntimeConfig = !newestProjectStableRuntimeConfig.empty()
        ? newestProjectStableRuntimeConfig
        : (!newestProjectCachedRuntimeConfig.empty() ? newestProjectCachedRuntimeConfig : newestFallbackRuntimeConfig);
    const auto& selectedAssembly = !newestProjectStableAssembly.empty()
        ? newestProjectStableAssembly
        : (!newestProjectCachedAssembly.empty() ? newestProjectCachedAssembly : newestFallbackAssembly);
    if (runtimeConfig.empty())
        runtimeConfig = selectedRuntimeConfig;
    if (assembly.empty())
        assembly = selectedAssembly;
}
}

ScriptRuntimeSetupResult InitializeScriptRuntime(
    ScriptRuntime& runtime,
    const ScriptRuntimeSetupOptions& options)
{
    // Register the scripting reflection module before any scene or prefab
    // load asks Engine to resolve ScriptComponent by type name.
    NLS_META_GENERATED_LINK_FUNCTION();

    ScriptRuntimeSetupResult result;
    const auto roots = SearchRoots(options.projectRoot);
    result.scriptApiDirectory = ResolveScriptApiDirectory(options);
    if (result.scriptApiDirectory.empty())
    {
        result.status = ScriptStatus::Error(
            ScriptStatusCode::AssetNotLoaded,
            "Script API manifest directory was not found in the project or repository roots.");
        return result;
    }

    ScriptApiDatabase api;
    const auto manifestStatus = ScriptApiManifest::Read(result.scriptApiDirectory, api);
    if (!manifestStatus.Succeeded())
    {
        result.status = manifestStatus;
        return result;
    }
    result.schemaLoaded = true;

#if NLS_ENABLE_LUA_SCRIPTING
    if (options.enableLua)
    {
        auto backend = std::make_unique<LuaScriptBackend>();
        const auto debuggerStatus = backend->SetLuaPandaDebugging(
            options.enableLuaPanda,
            options.luaPandaHost,
            options.luaPandaPort);
        if (!debuggerStatus.Succeeded())
        {
            // LuaPanda is an optional debugger and is intentionally omitted
            // from Release/Player builds.  A persisted project setting must
            // not make the whole scripting runtime fail to initialize when
            // such a build is launched after a Debug session.  Invalid
            // debugger configuration is still a hard error.
            if (debuggerStatus.code != ScriptStatusCode::BackendUnavailable)
            {
                result.status = debuggerStatus;
                return result;
            }

            NLS_LOG_WARNING(
                "LuaPanda was requested but is unavailable in this build; "
                "continuing with Lua scripting without the debugger: " +
                debuggerStatus.message);
            (void)backend->SetLuaPandaDebugging(false, options.luaPandaHost, options.luaPandaPort);
        }
        else
        {
            result.luaPandaEnabled = options.enableLuaPanda;
        }
        const auto status = runtime.RegisterBackend(std::move(backend));
        if (!status.Succeeded())
        {
            result.status = status;
            return result;
        }
        result.luaRegistered = true;
    }
#endif

    ResolveManagedArtifacts(options, roots, result.managedRuntimeConfig, result.managedAssembly);
    NLS_LOG_INFO(
        "Resolved managed C# artifacts (runtimeconfig='" + result.managedRuntimeConfig.string() +
        "', assembly='" + result.managedAssembly.string() + "').");
    result.dotnetRoot = ResolveDotnetRoot(options, roots);
    const bool managedArtifactsAvailable =
        !result.managedRuntimeConfig.empty() &&
        !result.managedAssembly.empty() &&
        !result.dotnetRoot.empty();
#if NLS_ENABLE_CSHARP_SCRIPTING
    if (options.enableCSharp && managedArtifactsAvailable)
    {
        auto backend = std::make_unique<CoreClrScriptBackend>();
        backend->SetDotnetRoot(result.dotnetRoot);
        backend->SetHostArtifacts(result.managedRuntimeConfig, result.managedAssembly);
        const auto status = runtime.RegisterBackend(std::move(backend));
        if (!status.Succeeded())
        {
            result.status = status;
            return result;
        }
        result.csharpRegistered = true;
    }
#else
    (void)managedArtifactsAvailable;
#endif

    result.status = runtime.Initialize(api);

    // A debugger can also be compiled in but fail during backend startup (for
    // example because its bundled LuaSocket asset is incomplete).  Retry once
    // without the optional debugger so gameplay scripting remains usable.  A
    // genuine Lua/CoreCLR initialization failure is returned unchanged.
    if (!result.status.Succeeded() && result.luaPandaEnabled &&
        result.status.code == ScriptStatusCode::BackendUnavailable)
    {
        if (auto* lua = runtime.GetBackend(ScriptLanguage::Lua))
        {
            const auto disableStatus = dynamic_cast<LuaScriptBackend*>(lua)
                ? static_cast<LuaScriptBackend*>(lua)->SetLuaPandaDebugging(
                    false, options.luaPandaHost, options.luaPandaPort)
                : ScriptStatus::Error(
                    ScriptStatusCode::BackendUnavailable,
                    "The registered Lua backend does not expose LuaPanda settings.");
            if (disableStatus.Succeeded())
            {
                NLS_LOG_WARNING(
                    "LuaPanda failed during startup; retrying scripting runtime "
                    "without the optional debugger: " + result.status.message);
                result.luaPandaEnabled = false;
                result.status = runtime.Initialize(api);
            }
        }
    }
    return result;
}
}
