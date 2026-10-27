#include <gtest/gtest.h>

#include "Build/BuildFileUtils.h"
#include "Build/BuildPipeline.h"
#include "Build/BuildProfileIO.h"
#include "Build/IBuildPlatform.h"
#include "Build/PackageInspection.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Guid.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
using NLS::Build::BuildContext;
using NLS::Build::BuildOutcome;
using NLS::Build::BuildPipeline;
using NLS::Build::BuildProfile;
using NLS::Build::BuildProfilesDocument;
using NLS::Build::BuildReport;
using NLS::Build::BuildRequest;
using NLS::Build::PlayerTemplateInfo;
using NLS::Core::Assets::ArtifactDatabase;
using NLS::Core::Assets::ArtifactManifest;
using NLS::Core::Assets::ArtifactType;
using NLS::Core::Assets::AssetDependencyKind;
using NLS::Core::Assets::AssetId;
using NLS::Core::Assets::ImportedArtifact;

std::filesystem::path MakeTemporaryDirectory(const std::string& name)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("nullus-pipeline-" + name + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string ArtifactRelativePath(const AssetId& id)
{
    auto name = id.ToString();
    name.erase(std::remove(name.begin(), name.end(), '-'), name.end());
    if (name.size() < 64u)
        name.resize(64u, '0');
    return "Library/Artifacts/" + name.substr(0u, 2u) + "/" + name;
}

// Minimal platform adapter: exercises the pipeline's phase ordering and
// commit/rollback rules without depending on a real player template.
class FakePlatform final : public NLS::Build::IBuildPlatform
{
public:
    explicit FakePlatform(std::filesystem::path playerDirectory)
        : m_playerDirectory(std::move(playerDirectory))
    {
    }

    std::string GetAdapterId() const override { return "test-windows-desktop"; }

    NLS::Build::PlatformCapability GetCapability(const NLS::Build::BuildTarget& target) const override
    {
        NLS::Build::PlatformCapability capability;
        capability.platform = NLS::Build::PlatformFamily::Windows;
        capability.architecture = NLS::Build::Architecture::X64;
        capability.graphicsBackend = target.graphicsBackend;
        capability.id = "windows-x64-" + std::string(NLS::Build::ToString(target.graphicsBackend));
        capability.adapterAvailable =
            target.platform == NLS::Build::PlatformFamily::Windows &&
            target.architecture == NLS::Build::Architecture::X64;
        capability.toolchainAvailable = capability.adapterAvailable;
        capability.reason = capability.adapterAvailable ? "" : "fake adapter only builds Windows x64";
        return capability;
    }

    bool CheckToolchain(const BuildContext&, BuildReport&, std::string*) override { return true; }

    std::optional<PlayerTemplateInfo> ResolvePlayerTemplate(
        const BuildContext& context,
        BuildReport&,
        std::string* error) override
    {
        (void)context;
        PlayerTemplateInfo info;
        info.directory = m_playerDirectory;
        info.executablePath = m_playerDirectory / "Game.exe";
        info.executableName = "SmokeGame.exe";
        info.engineVersion = "1.0.0";
        info.playerId = "smoke-player";
        info.targetId = "windows-x64-dx12";
        info.runtimeFiles = {"Game.exe", "NLS_Fake.dll"};
        if (!std::filesystem::is_regular_file(info.executablePath))
        {
            if (error != nullptr)
                *error = "fake player missing";
            return std::nullopt;
        }
        return info;
    }

    bool AssemblePlayer(
        const BuildContext& context,
        const PlayerTemplateInfo& player,
        BuildReport&,
        std::string* error) override
    {
        for (const auto& relative : player.runtimeFiles)
        {
            auto destination = context.stagingRoot / relative;
            if (relative == "Game.exe")
                destination = context.stagingRoot / player.executableName;
            NLS::Build::CopyFileEnsuringDirectory(player.directory / relative, destination, error);
            if (error != nullptr && !error->empty())
                return false;
        }
        return true;
    }

    bool FinalizeStagingPlayer(const BuildContext&, BuildReport&, std::string*) override { return true; }

    bool SignStaging(const BuildContext&, BuildReport& report, std::string*) override
    {
        report.signingDescription = "unsigned (test adapter)";
        return true;
    }

    bool PackageOutput(
        const BuildContext& context,
        const std::filesystem::path& stagingDirectory,
        BuildReport& report,
        std::filesystem::path& outputRoot,
        std::string* error) override
    {
        const auto packageDirectory = context.outputRoot / "SmokeGame-0.1.0-windows-x64-dx12";
        if (!NLS::Build::EnsureDirectory(packageDirectory, error))
            return false;
        size_t copied = 0u;
        if (!NLS::Build::CopyTreeFiltered(
                stagingDirectory,
                packageDirectory,
                [](const std::string&) { return true; },
                &copied,
                error))
        {
            return false;
        }
        report.packageDirectory = packageDirectory.string();
        outputRoot = context.outputRoot;
        return true;
    }

private:
    std::filesystem::path m_playerDirectory;
};

struct SmokeFixture
{
    std::filesystem::path root;
    std::filesystem::path projectFile;
    std::filesystem::path assets;
    std::filesystem::path artifactDatabase;
    std::filesystem::path player;
    std::filesystem::path output;
    NLS::Core::Assets::AssetId textureId;
};

const std::array<uint8_t, 70u> kTinyPng {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,
    0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,
    0x00,0x1F,0x15,0xC4,0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,
    0xDA,0x63,0x64,0xF8,0xCF,0x50,0x0F,0x00,0x03,0x86,0x01,0x80,0x5A,0x34,
    0x7D,0x6B,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
};

SmokeFixture MakeSmokeFixture(const std::string& name)
{
    SmokeFixture fixture;
    fixture.root = MakeTemporaryDirectory(name);
    fixture.projectFile = fixture.root / "SmokeGame.nullus";
    fixture.assets = fixture.root / "Assets";
    fixture.artifactDatabase = fixture.root / "Library" / "ArtifactDB";
    fixture.player = fixture.root / "Player";
    fixture.output = fixture.root / "Dist";
    std::filesystem::create_directories(fixture.artifactDatabase);
    std::filesystem::create_directories(fixture.player);

    WriteFile(fixture.projectFile, "executable_name=SmokeGame\nstart_scene=Scenes/Main.scene\ndev_build=true\n");
    const auto sceneId = AssetId(NLS::Guid::NewDeterministic("smoke-scene-" + name));
    fixture.textureId = AssetId(NLS::Guid::NewDeterministic("smoke-texture-" + name));
    const auto scenePath = fixture.assets / "Scenes" / "Main.scene";
    std::ostringstream sceneJson;
    sceneJson
        << "{\n"
        << "  \"documentId\": \"" << NLS::Guid::NewDeterministic("smoke-scene-doc-" + name).ToString() << "\",\n"
        << "  \"root\": \"" << NLS::Guid::NewDeterministic("smoke-scene-root-" + name).ToString() << "\",\n"
        << "  \"format\": \"Nullus.ObjectGraph.Scene\",\n"
        << "  \"objects\": [\n"
        << "    {\n"
        << "      \"fileID\": 1,\n"
        << "      \"id\": \"" << NLS::Guid::NewDeterministic("smoke-object-" + name).ToString() << "\",\n"
        << "      \"type\": \"Test.ReferenceHolder\",\n"
        << "      \"properties\": {\n"
        << "        \"texture\": {\n"
        << "          \"fileID\": 12345,\n"
        << "          \"guid\": \"" << fixture.textureId.ToString() << "\",\n"
        << "          \"type\": 2,\n"
        << "          \"filePath\": \"Assets/Textures/White.png\"\n"
        << "        }\n"
        << "      }\n"
        << "    }\n"
        << "  ]\n"
        << "}\n";
    WriteFile(scenePath, sceneJson.str());
    WriteFile(scenePath.string() + ".meta", "GUID=" + sceneId.ToString() + "\nASSET_TYPE=scene\n");

    const auto texturePath = fixture.assets / "Textures" / "White.png";
    std::filesystem::create_directories(texturePath.parent_path());
    {
        std::ofstream texture(texturePath, std::ios::binary | std::ios::trunc);
        texture.write(reinterpret_cast<const char*>(kTinyPng.data()), static_cast<std::streamsize>(kTinyPng.size()));
    }
    WriteFile(
        texturePath.string() + ".meta",
        "GUID=" + fixture.textureId.ToString() + "\nASSET_TYPE=texture\nIMPORTER_ID=texture\nIMPORTER_VERSION=1\n");
    WriteFile(fixture.assets / "Scripts" / "Player.lua", "print('smoke')\n");

    // The texture is intentionally not cooked here: the packaging pipeline's
    // bake phase must produce its target artifact from the source PNG.
    ArtifactDatabase database;
    database.Save(fixture.artifactDatabase);

    WriteFile(fixture.player / "Game.exe", "fake-player-executable");
    WriteFile(fixture.player / "NLS_Fake.dll", "fake-runtime-dll");
    WriteFile(fixture.player / ".nullus-runtime-RelWithDebInfo.manifest", "Game.exe\nNLS_Fake.dll\n");

    BuildProfile profile;
    profile.id = "smoke-development";
    profile.displayName = "Smoke Development";
    profile.productName = "SmokeGame";
    profile.executableName = "SmokeGame";
    profile.version = "0.1.0";
    profile.startScene = "Scenes/Main.scene";
    profile.mode = NLS::Build::BuildMode::Development;
    profile.target.platform = NLS::Build::PlatformFamily::Windows;
    profile.target.architecture = NLS::Build::Architecture::X64;
    profile.target.graphicsBackend = NLS::Build::GraphicsBackendKind::DX12;
    profile.target.artifactPlatform = "win64";
    profile.target.minimumOSVersion = "10.0.19045";
    profile.target.shaderTarget = "dxil";
    profile.outputFormat = NLS::Build::OutputFormat::Directory;
    profile.target.scriptBackends = {NLS::Build::ScriptBackend::Lua};

    BuildProfilesDocument document;
    document.defaultProfileId = profile.id;
    document.profiles.push_back(profile);
    std::vector<NLS::Build::BuildDiagnostic> diagnostics;
    NLS::Build::SaveBuildProfiles(
        fixture.root / NLS::Build::BuildProfilesFileName(),
        document,
        diagnostics);
    return fixture;
}

BuildRequest MakeRequest(const SmokeFixture& fixture)
{
    BuildRequest request;
    request.projectFile = fixture.projectFile;
    request.profileId = "smoke-development";
    request.outputDirectory = fixture.output;
    request.sign = false;
    request.validateLaunch = false;
    return request;
}
} // namespace

TEST(BuildPipelineTests, RunsAllPhasesAndProducesAValidPackage)
{
    auto fixture = MakeSmokeFixture("success");
    NLS::Build::RegisterDefaultBuildPlatforms();
    auto& registry = NLS::Build::BuildPlatformRegistry::Get();
    registry.Unregister("windows-x64-desktop");
    registry.Register(std::make_shared<FakePlatform>(fixture.player));

    BuildRequest request = MakeRequest(fixture);
    BuildReport report;
    const auto outcome = BuildPipeline::Run(request, report);

    ASSERT_TRUE(outcome.succeeded) << report.FormatSummary() << "\nerror: " << outcome.error;
    EXPECT_TRUE(report.succeeded);
    EXPECT_EQ(report.targetId, "windows-x64-dx12");
    EXPECT_EQ(report.artifactPlatform, "win64");
    EXPECT_EQ(report.contentPackages.size(), 1u);
    EXPECT_FALSE(report.inputHash.empty());
    EXPECT_FALSE(report.phases.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(outcome.reportPath));
    EXPECT_FALSE(std::filesystem::exists(outcome.stagingDirectory));

    // Build report persists every phase and included asset reason.
    std::string loadError;
    const auto loaded = BuildReport::Load(outcome.reportPath, &loadError);
    ASSERT_TRUE(loaded.has_value()) << loadError;
    EXPECT_TRUE(loaded->succeeded);
    const auto sceneAsset = std::find_if(
        loaded->includedAssets.begin(),
        loaded->includedAssets.end(),
        [](const NLS::Build::BuildIncludedAsset& asset) { return asset.reason == "start-scene:reference"; });
    EXPECT_NE(sceneAsset, loaded->includedAssets.end());

    NLS::Build::PackageInspection inspection;
    EXPECT_TRUE(NLS::Build::InspectPackage(report.packageDirectory, inspection))
        << NLS::Build::FormatInspection(inspection);
    EXPECT_EQ(inspection.artifactPlatform, "win64");
    EXPECT_TRUE(inspection.contentPackages.size() == 1u);

    // A second build with identical inputs produces the same package hash.
    BuildReport secondReport;
    const auto secondOutcome = BuildPipeline::Run(request, secondReport);
    ASSERT_TRUE(secondOutcome.succeeded) << secondReport.FormatSummary();
    std::string firstHash;
    std::string secondHash;
    for (const auto& output : report.outputs)
    {
        if (output.kind == "content-package")
            firstHash = output.sha256;
    }
    for (const auto& output : secondReport.outputs)
    {
        if (output.kind == "content-package")
            secondHash = output.sha256;
    }
    ASSERT_FALSE(firstHash.empty());
    EXPECT_EQ(firstHash, secondHash);

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}

TEST(BuildPipelineTests, FailedBuildKeepsExistingProductsAndRemovesStaging)
{
    auto fixture = MakeSmokeFixture("failure");
    NLS::Build::RegisterDefaultBuildPlatforms();
    auto& registry = NLS::Build::BuildPlatformRegistry::Get();
    registry.Unregister("windows-x64-desktop");
    registry.Register(std::make_shared<FakePlatform>(fixture.player));

    BuildRequest request = MakeRequest(fixture);
    BuildReport report;
    const auto firstOutcome = BuildPipeline::Run(request, report);
    ASSERT_TRUE(firstOutcome.succeeded) << report.FormatSummary();
    const auto previousPackage = std::filesystem::path(report.packageDirectory);
    ASSERT_TRUE(std::filesystem::is_regular_file(previousPackage / "BuildReport.json"));

    // Break the cooked artifact and disable baking for the retry, so the
    // collector observes an up-to-date record whose file vanished.
    NLS::Build::RemoveDirectoryIfExists(fixture.root / "Library" / "Artifacts");
    BuildRequest failedRequest = request;
    failedRequest.bakeAssets = false;
    BuildReport failedReport;
    const auto failedOutcome = BuildPipeline::Run(failedRequest, failedReport);
    EXPECT_FALSE(failedOutcome.succeeded);
    EXPECT_TRUE(failedReport.HasErrors());
    EXPECT_FALSE(std::filesystem::exists(failedOutcome.stagingDirectory));
    // The previous product and its report stay untouched.
    EXPECT_TRUE(std::filesystem::is_regular_file(previousPackage / "BuildReport.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(previousPackage / "SmokeGame.exe"));
    EXPECT_TRUE(std::filesystem::is_regular_file(failedOutcome.reportPath));

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}

#if defined(_WIN32)
TEST(BuildPipelineTests, WindowsAdapterProducesAndSelfTestsAZip)
{
    const std::filesystem::path playerTemplate =
        std::filesystem::path(NLS_ROOT_DIR) / "App" / "Win64_RelWithDebInfo_Runtime_Shared";
    if (!std::filesystem::is_regular_file(playerTemplate / "Game.exe"))
    {
        GTEST_SKIP() << "prebuilt player template is not available at " << playerTemplate;
    }

    auto fixture = MakeSmokeFixture("windows-adapter");
    NLS::Build::RegisterDefaultBuildPlatforms();
    auto& registry = NLS::Build::BuildPlatformRegistry::Get();
    registry.Unregister("test-windows-desktop");
    // Re-register the real adapter explicitly: earlier tests may have swapped
    // it out and RegisterDefaultBuildPlatforms is idempotent by design.
    NLS::Build::RegisterWindowsDesktopPlatform();

    BuildRequest request = MakeRequest(fixture);
    request.playerTemplatePath = playerTemplate;
    request.validateLaunch = true;
    request.sign = false;

    // The real adapter only accepts the target its template was produced for.
    std::vector<NLS::Build::BuildDiagnostic> diagnostics;
    NLS::Build::BuildProfilesDocument document;
    ASSERT_TRUE(NLS::Build::LoadBuildProfiles(
        fixture.root / NLS::Build::BuildProfilesFileName(), document, diagnostics));
    document.profiles.front().outputFormat = NLS::Build::OutputFormat::Zip;
    document.profiles.front().executableName = "SmokeGame";
    document.profiles.front().validateLaunchOnBuild = true;
    ASSERT_TRUE(NLS::Build::SaveBuildProfiles(
        fixture.root / NLS::Build::BuildProfilesFileName(), document, diagnostics));

    BuildReport report;
    const auto outcome = BuildPipeline::Run(request, report);
    ASSERT_TRUE(outcome.succeeded) << report.FormatSummary() << "\nerror: " << outcome.error;
    EXPECT_TRUE(report.signed_ == false);
    EXPECT_NE(report.signingDescription.find("unsigned"), std::string::npos);

    const auto packageDirectory = std::filesystem::path(report.packageDirectory);
    EXPECT_TRUE(std::filesystem::is_regular_file(packageDirectory / "SmokeGame.exe"));
    EXPECT_TRUE(std::filesystem::is_regular_file(packageDirectory / "Player.info"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        packageDirectory / "Data" / "User" / "Packages" / "base.npak"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        packageDirectory / "Assets" / "Engine" / "Shaders" / "StandardPBR.hlsl"));

    // The delivered ZIP must contain exactly one product directory and pass
    // the same inspection as an installed tree.
    bool foundArchive = false;
    for (const auto& output : report.outputs)
    {
        if (output.kind != "archive")
            continue;
        foundArchive = true;
        NLS::Build::PackageInspection inspection;
        EXPECT_TRUE(NLS::Build::ValidateArchive(
            outcome.outputDirectory / output.path, inspection))
            << NLS::Build::FormatInspection(inspection);
    }
    EXPECT_TRUE(foundArchive);
    const auto hasLaunchValidation = std::any_of(
        report.diagnostics.begin(),
        report.diagnostics.end(),
        [](const NLS::Build::BuildDiagnostic& diagnostic)
        {
            return diagnostic.code == "build-launch-validated";
        });
    EXPECT_TRUE(hasLaunchValidation);

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}
#endif
