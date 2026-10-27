#pragma once

#include "BuildDef.h"
#include "BuildProfileIO.h"
#include "BuildReport.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Build
{
struct IBuildPlatform;

// Everything the pipeline and the platform adapters need for one build.
struct BuildContext
{
    std::filesystem::path projectFile;
    std::filesystem::path projectRoot;
    std::filesystem::path projectAssetsPath;
    std::filesystem::path projectScriptsPath;
    std::filesystem::path engineAssetsPath;
    std::filesystem::path libraryPath;
    std::filesystem::path artifactDatabasePath;
    std::filesystem::path buildProfilesPath;
    std::filesystem::path stagingRoot;
    std::filesystem::path outputRoot;
    std::filesystem::path playerTemplatePath;
    std::filesystem::path editorExecutablePath;

    BuildProfile profile;
    BuildProductInfo product;

    bool sign = true;
    bool validateLaunch = false;
    bool verifyPackages = true;
    bool cleanStaging = true;
    std::atomic_bool* cancel = nullptr;

    std::function<void(const std::string& phase, const std::string& message)> progress;
};

struct BuildRequest
{
    std::filesystem::path projectFile;
    std::string profileId;
    std::filesystem::path outputDirectory;
    std::filesystem::path playerTemplatePath;
    std::filesystem::path editorExecutablePath;
    bool sign = true;
    bool validateLaunch = false;
    bool verifyPackages = true;
    bool cleanStaging = true;
    bool bakeAssets = true;
    bool forceBake = false;
    std::atomic_bool* cancel = nullptr;
    std::function<void(const std::string& phase, const std::string& message)> progress;
};

struct BuildOutcome
{
    bool succeeded = false;
    std::string error;
    std::filesystem::path stagingDirectory;
    std::filesystem::path outputDirectory;
    std::filesystem::path reportPath;
};

// The end-to-end packaging pipeline:
//   validate inputs -> freeze inputs -> collect dependencies -> bake content
//   -> prepare programs -> package content -> assemble -> validate -> sign
//   -> produce the distributable output.
//
// Only a fully successful build commits its staging directory; failures and
// cancellations remove only the staging tree owned by this build.
class NLS_BUILD_API BuildPipeline
{
public:
    static BuildOutcome Run(const BuildRequest& request, BuildReport& report);

    // Resolves the profile the request should use and fills the context.
    static bool PrepareContext(const BuildRequest& request, BuildContext& context, BuildReport& report, std::string* error);

    // Cancellation helpers so hosts (editor, CI) can abort a long build.
    static bool IsCancelled(const BuildContext& context);
};

// Computes the deterministic input hash recorded in the build report. The hash
// covers the profile, project settings, asset listing, artifact database, and
// player template so mixed-version builds fail instead of shipping.
NLS_BUILD_API std::string ComputeBuildInputHash(const BuildContext& context);
} // namespace NLS::Build
