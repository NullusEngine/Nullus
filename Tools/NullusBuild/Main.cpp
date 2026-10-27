#include "BuildPipeline.h"
#include "BuildProfileIO.h"
#include "BuildReport.h"
#include "BuildFileUtils.h"
#include "IBuildPlatform.h"
#include "PackageInspection.h"
#include "CookTypes.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/NpakArchive.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{
void PrintUsage()
{
    std::cout
        << "NullusBuild - headless packaging for Nullus projects\n\n"
        << "Usage:\n"
        << "  NullusBuild build --project <MyGame.nullus> [--profile <id>] [--output <dir>]\n"
        << "                    [--player <player-template-dir>] [--no-sign] [--validate-launch]\n"
        << "  NullusBuild validate --package <product-dir-or.zip>\n"
        << "  NullusBuild inspect  --package <product-dir-or.zip> [--no-verify]\n"
        << "  NullusBuild bake     --project <MyGame.nullus> [--profile <id>] [--force-bake] [--filter <asset-substring>]\n"
        << "  NullusBuild capabilities\n"
        << "  NullusBuild profiles --project <MyGame.nullus>\n"
        << "  NullusBuild assets --project <MyGame.nullus> [--filter <source-path-substring>]\n"
        << "  NullusBuild pack --filter <input-dir> --output <file.npak> [--profile <archive-prefix>]\n\n"
        << "Exit codes: 0 success, 1 build/validation failure, 2 invalid arguments.\n";
}

struct ParsedArguments
{
    std::string command;
    std::string project;
    std::string profile;
    std::string output;
    std::string player;
    std::string package;
    std::string filter;
    bool noSign = false;
    bool noVerify = false;
    bool noBake = false;
    bool forceBake = false;
    bool validateLaunch = false;
    bool help = false;
    bool valid = true;
    std::string error;
};

ParsedArguments ParseArguments(const int argc, char** argv)
{
    ParsedArguments arguments;
    if (argc < 2)
    {
        arguments.help = true;
        return arguments;
    }
    arguments.command = argv[1];

    for (int index = 2; index < argc; ++index)
    {
        const std::string argument = argv[index];
        const auto requireValue = [&](std::string& target)
        {
            if (index + 1 >= argc)
            {
                arguments.valid = false;
                arguments.error = "missing value for " + argument;
                return false;
            }
            target = argv[++index];
            return true;
        };

        if (argument == "--project")
        {
            if (!requireValue(arguments.project))
                return arguments;
        }
        else if (argument == "--profile")
        {
            if (!requireValue(arguments.profile))
                return arguments;
        }
        else if (argument == "--output")
        {
            if (!requireValue(arguments.output))
                return arguments;
        }
        else if (argument == "--player")
        {
            if (!requireValue(arguments.player))
                return arguments;
        }
        else if (argument == "--package")
        {
            if (!requireValue(arguments.package))
                return arguments;
        }
        else if (argument == "--filter")
        {
            if (!requireValue(arguments.filter))
                return arguments;
        }
        else if (argument == "--no-sign")
        {
            arguments.noSign = true;
        }
        else if (argument == "--no-verify")
        {
            arguments.noVerify = true;
        }
        else if (argument == "--no-bake")
        {
            arguments.noBake = true;
        }
        else if (argument == "--force-bake")
        {
            arguments.forceBake = true;
        }
        else if (argument == "--validate-launch")
        {
            arguments.validateLaunch = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            arguments.help = true;
        }
        else
        {
            arguments.valid = false;
            arguments.error = "unknown argument: " + argument;
            return arguments;
        }
    }
    return arguments;
}

int RunBuild(const ParsedArguments& arguments)
{
    NLS::Build::BuildRequest request;
    request.projectFile = arguments.project;
    request.profileId = arguments.profile;
    request.outputDirectory = arguments.output;
    request.playerTemplatePath = arguments.player;
    request.sign = !arguments.noSign;
    request.validateLaunch = arguments.validateLaunch;
    request.bakeAssets = !arguments.noBake;
    request.forceBake = arguments.forceBake;

    std::atomic_bool cancelled {false};
    request.cancel = &cancelled;
    request.progress = [](const std::string& phase, const std::string& message)
    {
        std::cout << "[" << phase << "] " << message << "\n";
    };

    NLS::Build::BuildReport report;
    const auto outcome = NLS::Build::BuildPipeline::Run(request, report);

    std::cout << report.FormatSummary();
    if (!outcome.succeeded)
    {
        std::cerr << "Build failed: " << outcome.error << "\n";
        return 1;
    }
    std::cout << "Output: " << outcome.outputDirectory.string() << "\n";
    std::cout << "Report: " << outcome.reportPath.string() << "\n";

    if (arguments.validateLaunch)
    {
        const auto skipped = std::any_of(
            report.diagnostics.begin(),
            report.diagnostics.end(),
            [](const NLS::Build::BuildDiagnostic& diagnostic)
            {
                return diagnostic.code == "build-launch-cross-host";
            });
        std::cout << (skipped
            ? "Final self-test skipped: the package targets another host.\n"
            : "Final self-test executed against the packaged player.\n");
    }
    return 0;
}

int RunBake(const ParsedArguments& arguments)
{
    if (arguments.project.empty())
    {
        std::cerr << "bake requires --project <MyGame.nullus> [--profile <id>]\n";
        return 2;
    }

    NLS::Build::BuildRequest request;
    request.projectFile = arguments.project;
    request.profileId = arguments.profile;
    NLS::Build::BuildContext context;
    NLS::Build::BuildReport report;
    std::string contextError;
    if (!NLS::Build::BuildPipeline::PrepareContext(request, context, report, &contextError))
    {
        std::cerr << contextError << "\n" << report.FormatSummary();
        return 1;
    }

    NLS::Cook::CookOptions options;
    options.projectRoot = context.projectRoot;
    options.assetsRoot = context.projectAssetsPath;
    options.artifactDatabasePath = context.artifactDatabasePath;
    options.artifactRoot = context.libraryPath / "Artifacts";
    options.stagingRoot = context.libraryPath / "ArtifactStaging" / "Cook";
    options.target.platform = NLS::Build::ToString(context.profile.target.platform);
    options.target.architecture = NLS::Build::ToString(context.profile.target.architecture);
    options.target.graphicsBackend = NLS::Build::ToString(context.profile.target.graphicsBackend);
    options.target.artifactPlatform = context.profile.target.artifactPlatform;
    options.target.shaderTarget = context.profile.target.shaderTarget;
    options.target.textureFormatPolicy = context.profile.target.textureFormatPolicy;
    options.maxTextureSize = context.profile.target.maxTextureSize;
    options.force = arguments.forceBake;
    options.pathFilter = arguments.filter;
    options.progress = [](const std::string& assetPath, const size_t completed, const size_t total)
    {
        if (completed == 0u || completed == total || completed % 32u == 0u)
            std::cout << "[" << completed << "/" << total << "] " << assetPath << "\n";
    };

    const auto summary = NLS::Cook::AssetCooker::CookProject(options);
    std::cout << summary.FormatSummary();
    return summary.Succeeded() ? 0 : 1;
}

int RunValidate(const ParsedArguments& arguments, const bool verifyPayloads)
{
    if (arguments.package.empty())
    {
        std::cerr << "validate requires --package <product-dir-or.zip>\n";
        return 2;
    }

    NLS::Build::PackageInspection inspection;
    const std::filesystem::path packagePath(arguments.package);
    const auto extension = packagePath.extension().string();
    const bool isArchive = extension == ".zip" || extension == ".npak";
    const bool valid = isArchive
        ? NLS::Build::ValidateArchive(packagePath, inspection)
        : NLS::Build::InspectPackage(packagePath, inspection, verifyPayloads);

    std::cout << NLS::Build::FormatInspection(inspection);
    return valid ? 0 : 1;
}
int RunProfiles(const ParsedArguments& arguments)
{
    if (arguments.project.empty())
    {
        std::cerr << "profiles requires --project <MyGame.nullus>\n";
        return 2;
    }

    const std::filesystem::path projectFile(arguments.project);
    const auto profilesPath = projectFile.parent_path() / NLS::Build::BuildProfilesFileName();
    NLS::Build::BuildProfilesDocument document;
    std::vector<NLS::Build::BuildDiagnostic> diagnostics;
    if (!NLS::Build::LoadBuildProfiles(profilesPath, document, diagnostics))
    {
        for (const auto& diagnostic : diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << " (" << diagnostic.detail << ")\n";
        return 1;
    }
    for (const auto& profile : document.profiles)
    {
        std::cout << (profile.id == document.defaultProfileId ? "* " : "  ")
                  << profile.id << " (" << profile.displayName << ")"
                  << " target=" << NLS::Build::BuildTargetId(profile.target)
                  << " mode=" << NLS::Build::ToString(profile.mode) << "\n";
    }
    return 0;
}

int RunPack(const ParsedArguments& arguments)
{
    if (arguments.filter.empty() || arguments.output.empty())
    {
        std::cerr << "pack requires --filter <input-dir> --output <file.npak> [--prefix <archive-prefix>]\n";
        return 2;
    }

    // `pack` reuses the deterministic writer for manual fixture creation and
    // incremental patch preparation. The regular build command writes packages
    // from the cook dependency graph.
    const std::filesystem::path inputDirectory(arguments.filter);
    const std::filesystem::path outputPath(arguments.output);
    std::error_code error;
    if (!std::filesystem::is_directory(inputDirectory, error))
    {
        std::cerr << "input directory does not exist: " << inputDirectory.string() << "\n";
        return 1;
    }

    NLS::Core::Assets::NpakWriter writer;
    size_t added = 0u;
    for (const auto& file : NLS::Build::EnumerateFiles(inputDirectory, true))
    {
        const auto relative = std::filesystem::relative(file, inputDirectory, error);
        if (error)
            continue;
        std::string key = relative.generic_string();
        if (!arguments.profile.empty())
        {
            std::string prefix = arguments.profile;
            while (!prefix.empty() && prefix.back() == '/')
                prefix.pop_back();
            if (!prefix.empty())
                key = prefix + "/" + key;
        }
        std::string addError;
        if (!writer.AddFileFromDisk(key, file, &addError))
        {
            std::cerr << addError << "\n";
            return 1;
        }
        ++added;
    }
    if (added == 0u)
    {
        std::cerr << "input directory contains no files\n";
        return 1;
    }

    std::string writeError;
    if (!writer.Write(outputPath, {}, &writeError))
    {
        std::cerr << writeError << "\n";
        return 1;
    }
    std::cout << "Packed " << added << " file(s) into " << outputPath.string() << "\n";
    return 0;
}

int RunCapabilities()
{
    NLS::Build::RegisterDefaultBuildPlatforms();
    std::cout << NLS::Build::BuildPlatformRegistry::Get().FormatCapabilities();
    return 0;
}

int RunAssets(const ParsedArguments& arguments)
{
    if (arguments.project.empty())
    {
        std::cerr << "assets requires --project <MyGame.nullus>\n";
        return 2;
    }
    const std::filesystem::path projectRoot = std::filesystem::path(arguments.project).parent_path();
    NLS::Core::Assets::ArtifactDatabase database;
    if (!database.Load(projectRoot / "Library" / "ArtifactDB"))
    {
        std::cerr << "cannot load ArtifactDB: " << database.GetLastError() << "\n";
        return 1;
    }

    std::map<std::string, size_t> byPlatform;
    std::map<std::string, size_t> byStatus;
    for (const auto& record : database.GetRecords())
    {
        ++byPlatform[record.targetPlatform];
        byStatus[record.status == NLS::Core::Assets::ArtifactRecordStatus::UpToDate ? "up-to-date" :
            record.status == NLS::Core::Assets::ArtifactRecordStatus::Stale ? "stale" :
            record.status == NLS::Core::Assets::ArtifactRecordStatus::Failed ? "failed" : "other"];
    }
    std::cout << "ArtifactDB: " << (projectRoot / "Library" / "ArtifactDB").string()
              << " (" << database.GetRecords().size() << " records)\n";
    for (const auto& [platform, count] : byPlatform)
        std::cout << "  platform " << (platform.empty() ? "<empty>" : platform) << ": " << count << "\n";
    for (const auto& [status, count] : byStatus)
        std::cout << "  status " << status << ": " << count << "\n";

    if (!arguments.filter.empty())
    {
        std::cout << "Records for source matching '" << arguments.filter << "':\n";
        for (const auto& record : database.GetRecords())
        {
            if (record.sourcePath.find(arguments.filter) == std::string::npos)
                continue;
            std::cout << "  " << record.sourcePath
                      << " [" << record.subAssetKey << "]"
                      << " type=" << static_cast<int>(record.artifactType)
                      << " target=" << record.targetPlatform
                      << " status=" << static_cast<int>(record.status)
                      << " path=" << record.artifactPath << "\n";
        }
    }
    return 0;
}
} // namespace

int main(const int argc, char** argv)
{
    const auto arguments = ParseArguments(argc, argv);
    if (arguments.help)
    {
        PrintUsage();
        return 0;
    }
    if (!arguments.valid)
    {
        std::cerr << arguments.error << "\n\n";
        PrintUsage();
        return 2;
    }

    try
    {
        if (arguments.command == "build")
            return RunBuild(arguments);
        if (arguments.command == "validate")
            return RunValidate(arguments, true);
        if (arguments.command == "inspect")
            return RunValidate(arguments, !arguments.noVerify);
        if (arguments.command == "profiles")
            return RunProfiles(arguments);
        if (arguments.command == "capabilities")
            return RunCapabilities();
        if (arguments.command == "assets")
            return RunAssets(arguments);
        if (arguments.command == "bake")
            return RunBake(arguments);
        if (arguments.command == "pack")
            return RunPack(arguments);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "NullusBuild: " << exception.what() << "\n";
        return 1;
    }

    std::cerr << "unknown command: " << arguments.command << "\n\n";
    PrintUsage();
    return 2;
}
