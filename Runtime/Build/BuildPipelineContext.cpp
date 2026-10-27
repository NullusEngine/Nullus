#include "BuildPipeline.h"

#include "BuildFileUtils.h"
#include "BuildPipelineInternal.h"
#include "BuildProfileIO.h"
#include "IBuildPlatform.h"

#include "Assets/ArtifactManifest.h"
#include "Version.h"

#include <algorithm>

namespace NLS::Build
{
namespace
{
std::string ProjectNameFromFile(const std::filesystem::path& projectFile)
{
    auto name = projectFile.stem().string();
    if (name.empty())
        name = "NullusGame";
    return name;
}
} // namespace

bool BuildPipeline::PrepareContext(
    const BuildRequest& request,
    BuildContext& context,
    BuildReport& report,
    std::string* error)
{
    context = {};

    if (request.projectFile.empty())
    {
        if (error != nullptr)
            *error = "no project file was provided (use --project <MyGame.nullus>)";
        report.AddDiagnostic(DiagnosticSeverity::Error, "build-project-missing", "No project file was provided.");
        return false;
    }

    std::error_code projectError;
    if (!std::filesystem::is_regular_file(request.projectFile, projectError))
    {
        if (error != nullptr)
            *error = "project file not found: " + request.projectFile.string();
        report.AddDiagnostic(
            DiagnosticSeverity::Error,
            "build-project-missing",
            "The project file was not found.",
            request.projectFile.string());
        return false;
    }

    context.projectFile = std::filesystem::absolute(request.projectFile);
    context.projectRoot = context.projectFile.parent_path();
    context.projectAssetsPath = context.projectRoot / "Assets";
    context.projectScriptsPath = context.projectRoot / "Scripts";
    context.libraryPath = context.projectRoot / "Library";
    context.artifactDatabasePath = context.libraryPath / "ArtifactDB";
    context.buildProfilesPath = context.projectRoot / BuildProfilesFileName();
    context.playerTemplatePath = request.playerTemplatePath;
    context.editorExecutablePath = request.editorExecutablePath;
    context.sign = request.sign;
    context.validateLaunch = request.validateLaunch;
    context.verifyPackages = request.verifyPackages;
    context.cleanStaging = request.cleanStaging;
    context.cancel = request.cancel;
    context.progress = request.progress;

    const auto settingsText = Detail::ReadTextFileOrEmpty(context.projectFile);

    // Profiles come from the project document when present; otherwise the
    // pipeline synthesises the implicit host default so fresh projects build.
    BuildProfilesDocument document;
    std::vector<BuildDiagnostic> profileDiagnostics;
    if (std::filesystem::is_regular_file(context.buildProfilesPath, projectError))
    {
        LoadBuildProfiles(context.buildProfilesPath, document, profileDiagnostics);
    }
    else
    {
        BuildProfile implicitProfile = MakeDefaultBuildProfile(
            Detail::ReadIniValue(settingsText, "executable_name", ProjectNameFromFile(context.projectFile)),
            ProjectNameFromFile(context.projectFile),
            Detail::ReadIniValue(settingsText, "start_scene", std::string {}),
            Detail::DetectHostPlatform(),
            Detail::DetectHostArchitecture(),
            Detail::DetectHostBackend());
        document.profiles.push_back(std::move(implicitProfile));
        document.defaultProfileId = document.profiles.front().id;
        report.AddDiagnostic(
            DiagnosticSeverity::Info,
            "build-profiles-implicit",
            "The project has no BuildProfiles.json; an implicit default profile was used.",
            context.buildProfilesPath.string());
    }

    for (const auto& diagnostic : profileDiagnostics)
        report.diagnostics.push_back(diagnostic);

    const auto wantedProfileId = request.profileId.empty() ? document.defaultProfileId : request.profileId;
    const auto* profile = FindBuildProfile(document, wantedProfileId);
    if (profile == nullptr)
    {
        if (error != nullptr)
            *error = "build profile '" + wantedProfileId + "' was not found";
        report.AddDiagnostic(
            DiagnosticSeverity::Error,
            "build-profile-not-found",
            "The requested build profile does not exist.",
            wantedProfileId);
        return false;
    }
    context.profile = *profile;

    // Fill target defaults the user is allowed to omit.
    if (context.profile.target.artifactPlatform.empty())
        context.profile.target.artifactPlatform = DefaultArtifactPlatform(context.profile.target.platform);
    if (context.profile.target.shaderTarget.empty())
        context.profile.target.shaderTarget = Detail::DefaultShaderTarget(context.profile.target.platform);
    if (context.profile.target.minimumOSVersion.empty())
        context.profile.target.minimumOSVersion = Detail::DefaultMinimumOSVersion(context.profile.target.platform);

    std::vector<BuildDiagnostic> validationDiagnostics;
    ValidateBuildProfile(context.profile, validationDiagnostics);
    for (const auto& diagnostic : validationDiagnostics)
        report.diagnostics.push_back(diagnostic);

    context.product.productName = context.profile.productName.empty()
        ? ProjectNameFromFile(context.projectFile)
        : context.profile.productName;
    context.product.companyName = context.profile.companyName;
    context.product.version = context.profile.version.empty() ? "0.1.0" : context.profile.version;
    context.product.buildNumber = context.profile.buildNumber;
    context.product.executableName = context.profile.executableName.empty()
        ? Detail::ReadIniValue(settingsText, "executable_name", context.product.productName)
        : context.profile.executableName;
    context.product.engineVersion = NLS::Core::kEngineVersionString;
    context.product.playerId = context.product.productName + "-" + context.product.version + "-" +
        BuildTargetId(context.profile.target);

    // Output and staging roots.
    std::filesystem::path outputRoot;
    if (!request.outputDirectory.empty())
    {
        outputRoot = request.outputDirectory;
    }
    else if (!context.profile.outputDirectory.empty())
    {
        const std::filesystem::path configured(context.profile.outputDirectory);
        outputRoot = configured.is_absolute() ? configured : context.projectRoot / configured;
    }
    else
    {
        outputRoot = context.projectRoot / "Dist" / context.profile.id;
    }
    context.outputRoot = std::filesystem::absolute(outputRoot);

    const auto stagingName = context.profile.id + "-staging";
    context.stagingRoot = context.outputRoot / ".build" / stagingName;

    if (report.HasErrors())
    {
        if (error != nullptr)
            *error = "the build profile is invalid";
        return false;
    }

    // Editor-cooked artifacts are acceptable for development runs only. A
    // shipping build must be cooked for the concrete target platform.
    if (context.profile.mode == BuildMode::Shipping &&
        context.profile.target.artifactPlatform == "editor")
    {
        report.AddDiagnostic(
            DiagnosticSeverity::Error,
            "build-editor-artifacts-shipping",
            "Shipping builds cannot ship editor-platform artifacts; cook the project for the target platform first.",
            context.profile.id);
        if (error != nullptr)
            *error = "shipping requires target-platform artifacts";
        return false;
    }
    return true;
}

std::string ComputeBuildInputHash(const BuildContext& context)
{
    NLS::Core::Assets::Sha256Hasher hasher;
    const auto addText = [&hasher](const std::string& text)
    {
        hasher.Update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    };

    addText(SerializeBuildProfile(context.profile));
    addText(ReadTextFileOrEmpty(context.projectFile));
    addText(Detail::HashDirectoryListing(context.projectAssetsPath, true));
    addText(Detail::HashDirectoryListing(context.artifactDatabasePath, true));
    if (!context.playerTemplatePath.empty())
        addText(Detail::HashDirectoryListing(context.playerTemplatePath, false));
    return NLS::Core::Assets::ToHexString(hasher.Finalize());
}

bool BuildPipeline::IsCancelled(const BuildContext& context)
{
    return context.cancel != nullptr && context.cancel->load(std::memory_order_relaxed);
}
} // namespace NLS::Build
