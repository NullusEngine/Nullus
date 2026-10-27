#include "BuildPipeline.h"

#include "BuildDependencyCollector.h"
#include "BuildFileUtils.h"
#include "BuildPipelineInternal.h"
#include "BuildPipelineStaging.h"
#include "BuildReport.h"
#include "IBuildPlatform.h"
#include "ManagedScriptBuildService.h"
#include "PackageInspection.h"
#include "Version.h"

#include "CookTypes.h"

#include "Assets/AssetMeta.h"
#include "Assets/NpakArchive.h"
#include "Engine/Assets/PlayerDescriptor.h"
#include "Guid.h"
#include "Platform/Process/Process.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <set>
#include <vector>

namespace NLS::Build
{
namespace
{
class PhaseScope
{
public:
    PhaseScope(BuildReport& report, std::string phase, const BuildContext& context)
        : m_report(report), m_phase(std::move(phase)), m_begin(std::chrono::steady_clock::now())
    {
        if (context.progress)
            context.progress(m_phase, "start");
    }

    ~PhaseScope()
    {
        const auto seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_begin).count();
        m_report.phases.push_back({m_phase, seconds});
    }

private:
    BuildReport& m_report;
    std::string m_phase;
    std::chrono::steady_clock::time_point m_begin;
};

void Progress(const BuildContext& context, const std::string& phase, const std::string& message)
{
    if (context.progress)
        context.progress(phase, message);
}

bool HasFileWithExtension(const std::filesystem::path& root, const std::string& extension)
{
    for (const auto& file : EnumerateFiles(root, true))
    {
        if (file.extension() == extension)
            return true;
    }
    return false;
}

std::string DescribeArtifactPlatformTags(
    const DependencyCollectionResult& collection)
{
    // Content collection already filters artifacts by the profile's target
    // platform; the tag is recorded so the report proves the check ran.
    std::set<std::string> tags;
    for (const auto& artifact : collection.artifacts)
        tags.insert(artifact.artifactPath.substr(0u, artifact.artifactPath.find('/')));
    std::string result;
    for (const auto& tag : tags)
    {
        if (!result.empty())
            result += ",";
        result += tag;
    }
    return result;
}

// Engine shaders are addressed through the virtual Assets/Engine tree but
// physically live beside the executable. Mirror them into a small synthetic
// project before the project bake so the shared ShaderLab cooker can publish
// artifacts with stable identities and correct virtual source paths.
std::filesystem::path PrepareEngineShaderCookRoot(const NLS::Cook::CookOptions& options)
{
    const auto resourceRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
    std::vector<std::filesystem::path> engineRoots;
    engineRoots.push_back(resourceRoots.engineAssetsRoot);
    for (auto probe = resourceRoots.installRoot; !probe.empty(); probe = probe.parent_path())
    {
        engineRoots.push_back(probe / "App" / "Assets" / "Engine");
        engineRoots.push_back(probe / "Assets" / "Engine");
        if (probe == probe.parent_path())
            break;
    }

    std::filesystem::path engineRoot;
    std::error_code error;
    for (const auto& candidate : engineRoots)
    {
        if (std::filesystem::is_directory(candidate / "Shaders", error))
        {
            engineRoot = candidate;
            break;
        }
    }
    if (engineRoot.empty())
        return {};

    const auto shaderRoot = engineRoot / "Shaders";
    const auto cookRoot = options.projectRoot / "Library" / "EngineCook";
    const auto syntheticAssets = cookRoot / "Assets";
    std::filesystem::remove_all(cookRoot, error);
    std::filesystem::create_directories(syntheticAssets, error);

    size_t shaderCount = 0u;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(shaderRoot, error))
    {
        if (error)
            break;
        if (!entry.is_regular_file() || entry.path().extension() != ".shader")
            continue;

        const auto relative = std::filesystem::relative(entry.path(), engineRoot, error);
        if (error || relative.empty() || relative.generic_string().rfind("..", 0u) == 0u)
            continue;
        const auto destination = syntheticAssets / "Engine" / relative;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (!std::filesystem::copy_file(
                entry.path(),
                destination,
                std::filesystem::copy_options::overwrite_existing,
                error))
        {
            continue;
        }

        const auto virtualPath = "Assets/Engine/" + relative.generic_string();
        std::ofstream meta(destination.string() + ".meta", std::ios::binary | std::ios::trunc);
        meta << "GUID=" << NLS::Cook::MakeEngineAssetGuid(virtualPath).ToString() << "\n"
             << "ASSET_TYPE=shader\n"
             << "IMPORTER_ID=shader\n"
             << "IMPORTER_VERSION=" << NLS::Core::Assets::GetCurrentImporterVersion(
                    NLS::Core::Assets::AssetType::Shader) << "\n";
        if (meta)
            ++shaderCount;
    }
    return shaderCount > 0u ? cookRoot : std::filesystem::path {};
}

void AppendCookDiagnostics(
    const NLS::Cook::CookSummary& summary,
    BuildReport& report)
{
    for (const auto& diagnostic : summary.diagnostics)
    {
        report.AddDiagnostic(
            diagnostic.severity == NLS::Cook::CookSeverity::Error
                ? DiagnosticSeverity::Error
                : diagnostic.severity == NLS::Cook::CookSeverity::Warning
                    ? DiagnosticSeverity::Warning
                    : DiagnosticSeverity::Info,
            diagnostic.code,
            diagnostic.message,
            diagnostic.path);
    }
}

std::filesystem::path BundledDotnetRoot(const BuildTarget& target)
{
    const char* os = nullptr;
    switch (target.platform)
    {
    case PlatformFamily::Windows: os = "windows"; break;
    case PlatformFamily::Linux: os = "linux"; break;
    case PlatformFamily::MacOS: os = "macos"; break;
    default: return {};
    }
    const char* architecture = target.architecture == Architecture::Arm64 ? "arm64" : "x64";

    const auto roots = NLS::Platform::Process::ResolveInstallResourceRoots();
    std::vector<std::filesystem::path> installRoots;
    installRoots.push_back(roots.installRoot);
    for (auto probe = roots.installRoot; !probe.empty(); probe = probe.parent_path())
    {
        installRoots.push_back(probe);
        if (probe == probe.parent_path())
            break;
    }
    for (const auto& installRoot : installRoots)
    {
        const auto candidate = installRoot / "Tools" / "Dotnet" / os / architecture;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate / "dotnet.exe", error) ||
            std::filesystem::is_regular_file(candidate / "dotnet", error))
        {
            return candidate;
        }
    }
    return {};
}

bool StageBundledDotnetRuntime(
    const BuildContext& context,
    BuildReport& report,
    std::string* error)
{    const auto destination = context.stagingRoot / "Runtime" / "dotnet";
    std::error_code fileError;
    const auto expectedHost = context.profile.target.platform == PlatformFamily::Windows
        ? "dotnet.exe"
        : "dotnet";
    const auto dotnetHost = destination / expectedHost;
    const auto dotnetFramework = destination / "shared";
    const auto dotnetHostFxr = destination / "host";
    if (std::filesystem::is_regular_file(dotnetHost, fileError) &&
        std::filesystem::is_directory(dotnetFramework, fileError) &&
        std::filesystem::is_directory(dotnetHostFxr, fileError))
    {
        return true;
    }
    std::filesystem::remove_all(destination, fileError);

    const auto source = BundledDotnetRoot(context.profile.target);
    if (source.empty())
    {
        if (error != nullptr)
            *error = "no bundled .NET runtime was found under Tools/Dotnet for target " +
                BuildTargetId(context.profile.target);
        return false;
    }

    std::error_code errorCode;
    std::filesystem::create_directories(destination, errorCode);
    if (errorCode)
    {
        if (error != nullptr)
            *error = "cannot create the player .NET runtime directory: " + errorCode.message();
        return false;
    }

    const auto copyEntry = [&](const std::filesystem::path& relative)
    {
        const auto from = source / relative;
        const auto to = destination / relative;
        if (!std::filesystem::exists(from, errorCode))
            return true;
        std::filesystem::create_directories(to.parent_path(), errorCode);
        if (errorCode)
            return false;
        std::filesystem::copy(
            from,
            to,
            std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing |
                std::filesystem::copy_options::copy_symlinks,
            errorCode);
        return !errorCode;
    };

    const bool hostCopied = std::filesystem::is_regular_file(source / "dotnet.exe", errorCode)
        ? copyEntry("dotnet.exe")
        : copyEntry("dotnet");
    if (!hostCopied)
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(destination, cleanupError);
        if (error != nullptr)
            *error = "cannot copy the bundled .NET host: " + errorCode.message();
        return false;
    }
    if (!copyEntry("host") || !copyEntry("shared/Microsoft.NETCore.App"))
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(destination, cleanupError);
        if (error != nullptr)
            *error = "cannot copy the bundled .NET runtime: " + errorCode.message();
        return false;
    }

    std::error_code permissionError;
    std::filesystem::permissions(
        destination / (std::filesystem::is_regular_file(source / "dotnet.exe", errorCode)
            ? "dotnet.exe"
            : "dotnet"),
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        permissionError);

    report.AddDiagnostic(
        DiagnosticSeverity::Info,
        "build-dotnet-runtime-staged",
        "The self-contained .NET runtime was added for target " +
            BuildTargetId(context.profile.target) + ".",
        source.string());
    return true;
}

bool StageProjectManagedScripts(
    const BuildContext& context,
    const ManagedScriptBuildResult& build,
    std::string* error)
{
    const auto scripts = context.stagingRoot / "Scripts";
    std::error_code errorCode;
    std::filesystem::create_directories(scripts, errorCode);
    if (errorCode)
    {
        if (error != nullptr)
            *error = "cannot create the player Scripts directory: " + errorCode.message();
        return false;
    }

    const auto copy = [&](const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        if (source.empty() || !std::filesystem::is_regular_file(source, errorCode))
            return true;
        std::filesystem::copy_file(
            source,
            destination,
            std::filesystem::copy_options::overwrite_existing,
            errorCode);
        return !errorCode;
    };

    if (!copy(build.assembly, scripts / "GameScripts.dll") ||
        !copy(build.deps, scripts / "GameScripts.deps.json") ||
        !copy(build.runtimeConfig, scripts / "GameScripts.runtimeconfig.json") ||
        !copy(build.engineAssembly, scripts / "EngineScripts.dll") ||
        !copy(build.engineDeps, scripts / "EngineScripts.deps.json") ||
        !copy(build.engineRuntimeConfig, scripts / "EngineScripts.runtimeconfig.json") ||
        !copy(build.managedAssembly, scripts / "Nullus.Managed.dll") ||
        !copy(
            build.managedAssembly.parent_path() / "Nullus.Managed.deps.json",
            scripts / "Nullus.Managed.deps.json"))
    {
        if (error != nullptr)
            *error = "cannot stage the project managed assembly payload";
        return false;
    }
    return true;
}
} // namespace

BuildOutcome BuildPipeline::Run(const BuildRequest& request, BuildReport& report)
{
    BuildOutcome outcome;
    report = {};
    report.schemaVersion = "1";
    report.startedUtc = CurrentUtcTimestamp();
    report.engineVersion = NLS::Core::kEngineVersionString;

    RegisterDefaultBuildPlatforms();

    BuildContext context;
    if (!PrepareContext(request, context, report, &outcome.error))
    {
        report.succeeded = false;
        report.finishedUtc = CurrentUtcTimestamp();
        return outcome;
    }

    report.profileId = context.profile.id;
    report.profileName = context.profile.displayName;
    report.buildMode = ToString(context.profile.mode);
    report.targetId = BuildTargetId(context.profile.target);
    report.artifactPlatform = context.profile.target.artifactPlatform;
    report.productName = context.product.productName;
    report.productVersion = context.product.version;
    report.buildNumber = context.product.buildNumber;
    report.playerId = context.product.playerId;
    report.configuration = BuildModeConfiguration(context.profile.mode);
    report.outputDirectory = context.outputRoot.string();
    outcome.stagingDirectory = context.stagingRoot;

    // 1. Validate inputs and toolchain.
    auto* platform = BuildPlatformRegistry::Get().Find(context.profile.target);
    if (platform == nullptr)
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Error,
            "build-platform-adapter-missing",
            "No build adapter exists for the selected target.",
            report.targetId);
        outcome.error = "no build adapter for target " + report.targetId;
    }
    else
    {
        const auto capability = platform->GetCapability(context.profile.target);
        if (!capability.adapterAvailable || !capability.toolchainAvailable)
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-platform-unavailable",
                "The target cannot be built on this host.",
                capability.reason);
            outcome.error = capability.reason;
            platform = nullptr;
        }
    }

    if (platform != nullptr)
    {
        PhaseScope phase(report, "validate-inputs", context);
        if (!platform->CheckToolchain(context, report, &outcome.error))
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-toolchain",
                "Toolchain validation failed.",
                outcome.error);
            platform = nullptr;
        }
        else if (IsCancelled(context))
        {
            report.AddDiagnostic(DiagnosticSeverity::Error, "build-cancelled", "The build was cancelled.");
            outcome.error = "cancelled";
            platform = nullptr;
        }
    }

    std::optional<PlayerTemplateInfo> player;
    std::optional<ManagedScriptBuildResult> projectManaged;
    DependencyCollectionResult collection;
    std::vector<std::string> contentPackages;
    std::filesystem::path symbolsDirectory;

    if (platform != nullptr && request.bakeAssets)
    {
        // 2. Cook supported assets for the target before dependency collection.
        // Unsupported types stay warnings: the collector fails later if the
        // packaged closure actually references one without a cooked artifact.
        PhaseScope phase(report, "bake-assets", context);
        NLS::Cook::CookOptions cookOptions;
        cookOptions.projectRoot = context.projectRoot;
        cookOptions.assetsRoot = context.projectAssetsPath;
        cookOptions.artifactDatabasePath = context.artifactDatabasePath;
        cookOptions.artifactRoot = context.libraryPath / "Artifacts";
        cookOptions.stagingRoot = context.libraryPath / "ArtifactStaging" / "Cook";
        cookOptions.target.platform = ToString(context.profile.target.platform);
        cookOptions.target.architecture = ToString(context.profile.target.architecture);
        cookOptions.target.graphicsBackend = ToString(context.profile.target.graphicsBackend);
        cookOptions.target.artifactPlatform = context.profile.target.artifactPlatform;
        cookOptions.target.shaderTarget = context.profile.target.shaderTarget;
        cookOptions.target.textureFormatPolicy = context.profile.target.textureFormatPolicy;
        cookOptions.maxTextureSize = context.profile.target.maxTextureSize;
        cookOptions.force = request.forceBake;
        cookOptions.cancel = context.cancel;
        cookOptions.progress = [&context](const std::string& assetPath, const size_t completed, const size_t total)
        {
            if (context.progress && (completed == 0u || completed == total || completed % 64u == 0u))
            {
                context.progress(
                    "bake-assets",
                    "Cooked " + std::to_string(completed) + "/" + std::to_string(total) + ": " + assetPath);
            }
        };

        const auto engineCookRoot = PrepareEngineShaderCookRoot(cookOptions);
        if (!engineCookRoot.empty())
        {
            auto engineOptions = cookOptions;
            engineOptions.projectRoot = engineCookRoot;
            engineOptions.assetsRoot = engineCookRoot / "Assets";
            const auto engineSummary = NLS::Cook::AssetCooker::CookProject(engineOptions);
            AppendCookDiagnostics(engineSummary, report);
            report.cachedStages.push_back(
                "cook:engine-shaders,cooked=" + std::to_string(engineSummary.cooked) +
                ",skipped=" + std::to_string(engineSummary.skipped));
            if (!engineSummary.Succeeded())
            {
                outcome.error = "engine shader cooking failed";
                platform = nullptr;
            }
        }

        const auto cookSummary = NLS::Cook::AssetCooker::CookProject(cookOptions);
        AppendCookDiagnostics(cookSummary, report);
        report.cachedStages.push_back(
            "cook:cooked=" + std::to_string(cookSummary.cooked) +
            ",skipped=" + std::to_string(cookSummary.skipped) +
            ",unsupported=" + std::to_string(cookSummary.unsupported));
        if (!cookSummary.Succeeded())
        {
            outcome.error = "asset cooking failed";
            platform = nullptr;
        }
    }

    if (platform != nullptr)
    {
        // 2. Freeze inputs, then collect dependencies against them.
        report.inputHash = ComputeBuildInputHash(context);
        const auto assetsListingBefore = Detail::HashDirectoryListing(context.projectAssetsPath, true);
        const auto databaseListingBefore = Detail::HashDirectoryListing(context.artifactDatabasePath, true);

        {
            PhaseScope phase(report, "collect-dependencies", context);
            if (!BuildDependencyCollector::Collect(
                    context.projectRoot,
                    context.projectAssetsPath,
                    context.profile,
                    context.artifactDatabasePath,
                    collection,
                    report))
            {
                outcome.error = "dependency collection failed";
                platform = nullptr;
            }
            else
            {
                for (const auto& artifact : collection.artifacts)
                {
                    BuildIncludedAsset included;
                    included.reference = artifact.reference;
                    included.artifactPath = artifact.artifactPath;
                    included.package = artifact.package;
                    included.reason = artifact.reason;
                    included.size = artifact.size;
                    report.includedAssets.push_back(std::move(included));
                }
                for (const auto& raw : collection.rawFiles)
                {
                    BuildIncludedAsset included;
                    included.reference = ToForwardSlashes(raw.relativePath);
                    included.sourcePath = raw.relativePath;
                    included.package = "raw";
                    included.reason = raw.reason;
                    included.size = raw.size;
                    report.includedAssets.push_back(std::move(included));
                }
            }
        }

        // The project must not change while the build reads it, otherwise the
        // package would mix asset versions.
        if (platform != nullptr)
        {
            const auto assetsListingAfter = Detail::HashDirectoryListing(context.projectAssetsPath, true);
            const auto databaseListingAfter = Detail::HashDirectoryListing(context.artifactDatabasePath, true);
            if (assetsListingAfter != assetsListingBefore || databaseListingAfter != databaseListingBefore)
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-inputs-changed",
                    "Project content changed while the build was collecting dependencies; save and retry.",
                    context.projectRoot.string());
                outcome.error = "project inputs changed during the build";
                platform = nullptr;
            }
        }
    }

    if (platform != nullptr)
    {
        // 3. Bake: phase 1 consumes artifacts already cooked for the target
        // platform. Re-labelling editor artifacts is explicitly rejected here.
        PhaseScope phase(report, "bake-content", context);
        report.cachedStages.push_back("bake:platform-tags=" + DescribeArtifactPlatformTags(collection));
        report.AddDiagnostic(
            DiagnosticSeverity::Info,
            "build-bake-platform-validated",
            "All collected artifacts were cooked for target platform '" +
                context.profile.target.artifactPlatform + "'.",
            context.profile.target.shaderTarget);
    }

    if (platform != nullptr)
    {
        // 4. Build the project's own C# scripts. This must happen before the
        // player template is resolved so the template's sample GameScripts
        // assembly can never silently replace project content.
        const bool requestsCSharp = std::find(
            context.profile.target.scriptBackends.begin(),
            context.profile.target.scriptBackends.end(),
            ScriptBackend::CSharp) != context.profile.target.scriptBackends.end();
        if (requestsCSharp && HasFileWithExtension(context.projectAssetsPath, ".cs"))
        {
            PhaseScope phase(report, "build-scripts", context);
            ManagedScriptBuildRequest scriptRequest;
            scriptRequest.projectRoot = context.projectRoot;
            scriptRequest.outputRoot = context.libraryPath / "ScriptBuild" / "ProjectManaged";
            scriptRequest.configuration =
                context.profile.mode == BuildMode::Shipping ? "Release" : "Debug";
            auto scriptResult = ManagedScriptBuildService::Build(scriptRequest);
            report.cachedStages.push_back(
                "scripts:managed,exit=" + std::to_string(scriptResult.exitCode));
            if (!scriptResult.succeeded)
            {
                std::string detail = scriptResult.output;
                if (detail.size() > 4000u)
                    detail = detail.substr(detail.size() - 4000u);
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-project-scripts",
                    "The project's C# scripts could not be compiled.",
                    detail);
                outcome.error = "project C# script compilation failed";
                platform = nullptr;
            }
            else
            {
                projectManaged = std::move(scriptResult);
            }
        }
    }

    if (platform != nullptr)
    {
        // 5. Prepare the program and script payloads.
        PhaseScope phase(report, "prepare-programs", context);
        std::string playerError;
        player = platform->ResolvePlayerTemplate(context, report, &playerError);
        if (!player.has_value())
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-player-template",
                "The prebuilt player could not be resolved.",
                playerError);
            outcome.error = playerError;
            platform = nullptr;
        }
        else
        {
            report.playerTemplatePath = player->directory.string();
            if (!player->engineVersion.empty() && player->engineVersion != context.product.engineVersion)
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Warning,
                    "build-player-version",
                    "The player template was built for engine '" + player->engineVersion +
                        "' while the tool is '" + context.product.engineVersion + "'.",
                    player->directory.string());
            }
            if (!player->targetId.empty() && player->targetId != report.targetId &&
                context.profile.customNativeModules.empty())
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-player-target-mismatch",
                    "The prebuilt player targets '" + player->targetId +
                        "' but the profile targets '" + report.targetId + "'.",
                    player->directory.string());
                outcome.error = "player template target mismatch";
                platform = nullptr;
            }

            // Script hosts: a shipping build that requests C# content must ship
            // a managed payload; a project without C# content may omit it.
            const bool requestsCSharp = std::find(
                context.profile.target.scriptBackends.begin(),
                context.profile.target.scriptBackends.end(),
                ScriptBackend::CSharp) != context.profile.target.scriptBackends.end();
            if (requestsCSharp && platform != nullptr)
            {
                const bool projectHasCSharp = HasFileWithExtension(context.projectAssetsPath, ".cs");
                const bool playerHasManaged = player->hasManagedAssemblies &&
                    std::filesystem::is_regular_file(player->directory / "Scripts" / "GameScripts.dll");
                const bool projectManagedAvailable =
                    projectManaged.has_value() && projectManaged->succeeded;
                if (projectHasCSharp && !playerHasManaged && !projectManagedAvailable)
                {
                    report.AddDiagnostic(
                        context.profile.mode == BuildMode::Shipping
                            ? DiagnosticSeverity::Error
                            : DiagnosticSeverity::Warning,
                        "build-script-host-unavailable",
                        "The project contains C# scripts but the player template ships no managed payload "
                        "(GameScripts.dll). Build the managed script target or drop C# from the profile.",
                        player->directory.string());
                    if (context.profile.mode == BuildMode::Shipping)
                    {
                        outcome.error = "C# script host is unavailable for a shipping build";
                        platform = nullptr;
                    }
                }
                else if (projectHasCSharp)
                {
                    const auto dotnetRoot = player->directory / "Runtime" / "dotnet";
                    if (!std::filesystem::is_directory(dotnetRoot) &&
                        context.profile.mode == BuildMode::Shipping &&
                        BundledDotnetRoot(context.profile.target).empty())
                    {
                        report.AddDiagnostic(
                            DiagnosticSeverity::Error,
                            "build-dotnet-runtime-missing",
                            "The player template has no self-contained .NET runtime, so a shipping build "
                            "cannot run C# scripts without an installed SDK.",
                            dotnetRoot.string());
                        outcome.error = ".NET runtime missing from the player template";
                        platform = nullptr;
                    }
                }
            }
        }
    }

    if (platform != nullptr && player.has_value())
    {
        // 5. Assemble the staging tree: player first, then content.
        {
            PhaseScope phase(report, "assemble-player", context);
            if (context.cleanStaging)
                RemoveDirectoryIfExists(context.stagingRoot);
            std::string stagingError;
            if (!EnsureDirectory(context.stagingRoot, &stagingError))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-staging",
                    "The staging directory could not be created.",
                    stagingError);
                outcome.error = stagingError;
                platform = nullptr;
            }
            else if (!platform->AssemblePlayer(context, *player, report, &outcome.error))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-assemble-player",
                    "The player payload could not be assembled.",
                    outcome.error);
                platform = nullptr;
            }

            if (platform != nullptr &&
                std::find(
                    context.profile.target.scriptBackends.begin(),
                    context.profile.target.scriptBackends.end(),
                    ScriptBackend::CSharp) != context.profile.target.scriptBackends.end() &&
                HasFileWithExtension(context.projectAssetsPath, ".cs"))
            {
                std::string managedError;
                if (projectManaged.has_value() && projectManaged->succeeded)
                {
                    if (!StageProjectManagedScripts(context, *projectManaged, &managedError))
                    {
                        report.AddDiagnostic(
                            DiagnosticSeverity::Error,
                            "build-project-scripts-stage",
                            "The project managed assembly payload could not be staged.",
                            managedError);
                        outcome.error = managedError;
                        platform = nullptr;
                    }
                }
                if (platform != nullptr &&
                    !StageBundledDotnetRuntime(context, report, &managedError))
                {
                    report.AddDiagnostic(
                        DiagnosticSeverity::Error,
                        "build-dotnet-runtime-missing",
                        "C# scripts were requested, but no player .NET runtime is available.",
                        managedError);
                    outcome.error = managedError;
                    platform = nullptr;
                }
            }
        }

        if (platform != nullptr)
        {
            PhaseScope phase(report, "package-content", context);
            std::string stageError;
            if (!Detail::StageContent(context, collection, report, contentPackages, &stageError))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-package-content",
                    "Content packaging failed.",
                    stageError);
                outcome.error = stageError;
                platform = nullptr;
            }
            else
            {
                Detail::WriteLicenseInventory(context, report);
            }
        }

        if (platform != nullptr)
        {
            PhaseScope phase(report, "finalize-player", context);
            std::string finalizeError;
            if (!platform->FinalizeStagingPlayer(context, report, &finalizeError))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-finalize-player",
                    "Player finalization failed.",
                    finalizeError);
                outcome.error = finalizeError;
                platform = nullptr;
            }
            else if (!Detail::WritePlayerDescriptorFile(
                         context,
                         *player,
                         contentPackages,
                         report,
                         &finalizeError))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-player-descriptor",
                    "The player descriptor could not be written.",
                    finalizeError);
                outcome.error = finalizeError;
                platform = nullptr;
            }
        }
    }

    if (platform != nullptr)
    {
        // 6. Verify staged packages before signing or archiving.
        PhaseScope phase(report, "verify-content", context);
        std::string verifyError;
        if (!Detail::VerifyStagedContent(context, report, contentPackages, &verifyError))
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-verify-content",
                "Staged content verification failed.",
                verifyError);
            outcome.error = verifyError;
            platform = nullptr;
        }
    }

    if (platform != nullptr)
    {
        // 7. Sign staged binaries, then produce the distributable output.
        PhaseScope phase(report, "sign", context);
        std::string signError;
        if (!platform->SignStaging(context, report, &signError))
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-sign",
                "Signing failed.",
                signError);
            outcome.error = signError;
            platform = nullptr;
        }
    }

    if (platform != nullptr)
    {
        PhaseScope phase(report, "package-output", context);
        std::filesystem::path outputRoot;
        std::string packageError;
        if (!platform->PackageOutput(context, context.stagingRoot, report, outputRoot, &packageError))
        {
            report.AddDiagnostic(
                DiagnosticSeverity::Error,
                "build-package-output",
                "The distributable output could not be produced.",
                packageError);
            outcome.error = packageError;
            platform = nullptr;
        }
        else
        {
            outcome.outputDirectory = outputRoot;

            PhaseScope symbolsPhase(report, "stage-symbols", context);
            std::string symbolsError;
            if (!Detail::StageSymbols(
                    context,
                    player.has_value() ? player->symbolFiles : std::vector<std::filesystem::path> {},
                    report,
                    symbolsDirectory,
                    &symbolsError))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Warning,
                    "build-symbols",
                    "The debug symbol package could not be produced.",
                    symbolsError);
            }

            PhaseScope validateOutputPhase(report, "validate-output", context);
            std::string outputError;
            if (!platform->ValidateOutput(
                    context,
                    std::filesystem::path(report.packageDirectory),
                    report,
                    &outputError))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-output-validation",
                    "The packaged player failed its final self-test.",
                    outputError);
                outcome.error = outputError;
                platform = nullptr;
            }
        }
    }

    if (platform != nullptr &&
        context.profile.outputFormat == OutputFormat::Zip &&
        !report.packageDirectory.empty())
    {
        // Validate the archive that is actually delivered, not only the
        // staging tree it was produced from.
        PhaseScope phase(report, "verify-archive", context);
        const auto packageName = std::filesystem::path(report.packageDirectory).filename().string();
        const auto archivePath = context.outputRoot / (packageName + ".zip");
        if (std::filesystem::is_regular_file(archivePath))
        {
            PackageInspection inspection;
            if (!ValidateArchive(archivePath, inspection))
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Error,
                    "build-archive-validation",
                    "The delivered archive failed validation.",
                    FormatInspection(inspection));
                outcome.error = "archive validation failed";
                platform = nullptr;
            }
            else
            {
                report.AddDiagnostic(
                    DiagnosticSeverity::Info,
                    "build-archive-validated",
                    "The delivered archive was extracted and validated.",
                    archivePath.string());
            }
        }
    }

    if (platform != nullptr && !report.packageDirectory.empty())
    {
        // 8. Record the final files and their checksums, including the archive.
        PhaseScope phase(report, "collect-outputs", context);
        const std::filesystem::path packageDirectory(report.packageDirectory);
        const auto executableName = context.product.executableName + ".exe";
        const auto executablePath = packageDirectory / executableName;
        if (std::filesystem::is_regular_file(executablePath))
        {
            BuildOutputFile executable;
            executable.path = executableName;
            executable.kind = "executable";
            executable.size = std::filesystem::file_size(executablePath);
            executable.sha256 = ComputeFileSha256(executablePath);
            report.outputs.push_back(std::move(executable));
        }

        for (const auto& package : contentPackages)
        {
            const auto path = packageDirectory / "Data" / "User" / "Packages" / package;
            if (!std::filesystem::is_regular_file(path))
                continue;
            BuildOutputFile file;
            file.path = "Data/User/Packages/" + package;
            file.kind = "content-package";
            file.size = std::filesystem::file_size(path);
            file.sha256 = ComputeFileSha256(path);
            report.outputs.push_back(std::move(file));
        }

        const auto manifestPath = packageDirectory / "Data" / "User" / "RuntimeAssetManifest.json";
        if (std::filesystem::is_regular_file(manifestPath))
        {
            BuildOutputFile file;
            file.path = "Data/User/RuntimeAssetManifest.json";
            file.kind = "runtime-manifest";
            file.size = std::filesystem::file_size(manifestPath);
            file.sha256 = ComputeFileSha256(manifestPath);
            report.outputs.push_back(std::move(file));
        }

        const auto descriptorPath = packageDirectory / NLS::Engine::Assets::PlayerDescriptor::FileName();
        if (std::filesystem::is_regular_file(descriptorPath))
        {
            BuildOutputFile file;
            file.path = NLS::Engine::Assets::PlayerDescriptor::FileName();
            file.kind = "player-descriptor";
            file.size = std::filesystem::file_size(descriptorPath);
            file.sha256 = ComputeFileSha256(descriptorPath);
            report.outputs.push_back(std::move(file));
        }

        if (!report.packageDirectory.empty())
        {
            const auto packageName = packageDirectory.filename().string();
            const auto archivePath = context.outputRoot / (packageName + ".zip");
            if (std::filesystem::is_regular_file(archivePath))
            {
                BuildOutputFile output;
                output.path = archivePath.filename().string();
                output.kind = "archive";
                output.size = std::filesystem::file_size(archivePath);
                output.sha256 = ComputeFileSha256(archivePath);
                report.outputs.push_back(std::move(output));
            }
        }

        for (const auto& file : EnumerateFiles(symbolsDirectory, true))
        {
            BuildOutputFile output;
            std::error_code relativeError;
            const auto relative = std::filesystem::relative(file, symbolsDirectory, relativeError);
            output.path = "symbols/" + (relativeError ? file.filename().string() : relative.generic_string());
            output.kind = "symbols";
            output.size = std::filesystem::file_size(file);
            output.sha256 = ComputeFileSha256(file);
            report.outputs.push_back(std::move(output));
        }
    }

    // 9. Commit or roll back. A failed or cancelled build removes only the
    // staging tree it owns; existing products stay untouched.
    report.succeeded = !report.HasErrors() && platform != nullptr;
    if (!report.succeeded)
    {
        if (outcome.error.empty())
            outcome.error = "build failed";
        RemoveDirectoryIfExists(context.stagingRoot);
    }
    else
    {
        RemoveDirectoryIfExists(context.stagingRoot);
        outcome.outputDirectory = context.outputRoot;
    }

    report.finishedUtc = CurrentUtcTimestamp();
    if (report.succeeded)
    {
        // The report travels with the product so `NullusBuild validate` can
        // verify the shipped artifact without the project.
        const std::filesystem::path packageDirectory(report.packageDirectory);
        if (!packageDirectory.empty())
        {
            report.Save(packageDirectory / "BuildReport.json", nullptr);
            BuildOutputFile reportOutput;
            reportOutput.path = "BuildReport.json";
            reportOutput.kind = "build-report";
            reportOutput.size = std::filesystem::file_size(packageDirectory / "BuildReport.json");
            reportOutput.sha256 = ComputeFileSha256(packageDirectory / "BuildReport.json");
            report.outputs.push_back(std::move(reportOutput));
        }
    }
    report.Save(
        context.outputRoot / (report.succeeded ? "BuildReport.json" : "BuildReport.failed.json"),
        nullptr);

    outcome.succeeded = report.succeeded;
    outcome.reportPath = context.outputRoot /
        (report.succeeded ? "BuildReport.json" : "BuildReport.failed.json");
    return outcome;
}
} // namespace NLS::Build
