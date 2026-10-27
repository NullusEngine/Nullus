#include <gtest/gtest.h>

#include "Build/BuildProfileIO.h"
#include "Build/IBuildPlatform.h"

#include <filesystem>
#include <fstream>

namespace
{
using NLS::Build::Architecture;
using NLS::Build::BuildDiagnostic;
using NLS::Build::BuildMode;
using NLS::Build::BuildProfile;
using NLS::Build::BuildProfilesDocument;
using NLS::Build::BuildTarget;
using NLS::Build::GraphicsBackendKind;
using NLS::Build::PlatformFamily;

std::filesystem::path MakeTemporaryDirectory(const std::string& name)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("nullus-build-profile-" + name + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

BuildProfile MakeProfile()
{
    BuildProfile profile;
    profile.id = "windows-shipping";
    profile.displayName = "Windows Shipping";
    profile.productName = "Sample";
    profile.executableName = "Sample";
    profile.startScene = "Scenes/Main.scene";
    profile.mode = BuildMode::Shipping;
    profile.target.platform = PlatformFamily::Windows;
    profile.target.architecture = Architecture::X64;
    profile.target.graphicsBackend = GraphicsBackendKind::DX12;
    profile.target.artifactPlatform = "win64";
    profile.target.minimumOSVersion = "10.0.19045";
    profile.target.shaderTarget = "dxil";
    return profile;
}
} // namespace

TEST(BuildProfileTests, RoundTripsThroughProjectDocument)
{
    const auto directory = MakeTemporaryDirectory("roundtrip");
    const auto path = directory / NLS::Build::BuildProfilesFileName();

    BuildProfilesDocument document;
    document.schemaVersion = 1u;
    document.defaultProfileId = "windows-shipping";
    auto profile = MakeProfile();
    profile.keepAssetPaths = {"Scenes/Extra", "0123456789abcdef0123456789abcdef"};
    profile.assetPackSelections = {"Levels:high"};
    profile.customNativeModules = {"MyNative"};
    document.profiles.push_back(profile);

    std::vector<BuildDiagnostic> diagnostics;
    ASSERT_TRUE(NLS::Build::SaveBuildProfiles(path, document, diagnostics));

    BuildProfilesDocument loaded;
    ASSERT_TRUE(NLS::Build::LoadBuildProfiles(path, loaded, diagnostics));
    ASSERT_EQ(loaded.profiles.size(), 1u);
    EXPECT_EQ(loaded.defaultProfileId, "windows-shipping");
    EXPECT_EQ(loaded.profiles.front().id, "windows-shipping");
    EXPECT_EQ(loaded.profiles.front().mode, BuildMode::Shipping);
    EXPECT_EQ(loaded.profiles.front().target.artifactPlatform, "win64");
    EXPECT_EQ(loaded.profiles.front().keepAssetPaths.size(), 2u);
    EXPECT_EQ(loaded.profiles.front().assetPackSelections.front(), "Levels:high");
    EXPECT_EQ(loaded.profiles.front().customNativeModules.front(), "MyNative");

    std::filesystem::remove_all(directory);
}

TEST(BuildProfileTests, DefaultProfileTargetsTheHost)
{
    const auto profile = NLS::Build::MakeDefaultBuildProfile(
        "Sample",
        "Sample",
        "Scenes/Main.scene",
        PlatformFamily::Windows,
        Architecture::X64,
        GraphicsBackendKind::DX12);
    EXPECT_EQ(profile.id, "windows-shipping");
    EXPECT_EQ(profile.target.artifactPlatform, "win64");
    EXPECT_EQ(profile.executableName, "Sample");
    EXPECT_EQ(profile.startScene, "Scenes/Main.scene");
    EXPECT_FALSE(profile.target.shaderTarget.empty());
}

TEST(BuildProfileTests, ValidationRejectsMissingSceneAndAppleBackend)
{
    auto profile = MakeProfile();
    profile.startScene.clear();
    profile.target.platform = PlatformFamily::MacOS;
    profile.target.graphicsBackend = GraphicsBackendKind::Vulkan;

    std::vector<BuildDiagnostic> diagnostics;
    EXPECT_FALSE(NLS::Build::ValidateBuildProfile(profile, diagnostics));
    const auto hasCode = [&diagnostics](const std::string& code)
    {
        return std::any_of(
            diagnostics.begin(),
            diagnostics.end(),
            [&code](const BuildDiagnostic& diagnostic) { return diagnostic.code == code; });
    };
    EXPECT_TRUE(hasCode("build-profile-scene"));
    EXPECT_TRUE(hasCode("build-profile-apple-backend"));
}

TEST(BuildProfileTests, ShippingWithNativeModulesRequiresSourceBuild)
{
    auto profile = MakeProfile();
    profile.customNativeModules = {"MyNative"};
    std::vector<BuildDiagnostic> diagnostics;
    EXPECT_FALSE(NLS::Build::ValidateBuildProfile(profile, diagnostics));
}

TEST(BuildProfileTests, TargetIdsAndArtifactPlatformsAreStable)
{
    auto profile = MakeProfile();
    EXPECT_EQ(NLS::Build::BuildTargetId(profile.target), "windows-x64-dx12");
    profile.target.platform = PlatformFamily::Linux;
    profile.target.graphicsBackend = GraphicsBackendKind::Vulkan;
    EXPECT_EQ(NLS::Build::BuildTargetId(profile.target), "linux-x64-vulkan");
    EXPECT_EQ(NLS::Build::DefaultArtifactPlatform(PlatformFamily::MacOS), "macos");
    EXPECT_EQ(NLS::Build::BuildModeConfiguration(BuildMode::Development), "RelWithDebInfo");
    EXPECT_EQ(NLS::Build::BuildModeConfiguration(BuildMode::Shipping), "Release");
}

TEST(BuildPlatformRegistryTests, ReportsImplementedAndDeclaredTargets)
{
    NLS::Build::RegisterDefaultBuildPlatforms();
    const auto& registry = NLS::Build::BuildPlatformRegistry::Get();

    BuildTarget windows;
    windows.platform = PlatformFamily::Windows;
    windows.architecture = Architecture::X64;
    windows.graphicsBackend = GraphicsBackendKind::DX12;
    EXPECT_NE(registry.Find(windows), nullptr);

    BuildTarget android;
    android.platform = PlatformFamily::Android;
    android.architecture = Architecture::Arm64;
    android.graphicsBackend = GraphicsBackendKind::Vulkan;
    EXPECT_EQ(registry.Find(android), nullptr);

    const auto capabilities = registry.DescribeAll();
    ASSERT_FALSE(capabilities.empty());
    const auto androidCapability = std::find_if(
        capabilities.begin(),
        capabilities.end(),
        [](const NLS::Build::PlatformCapability& capability)
        {
            return capability.platform == PlatformFamily::Android;
        });
    ASSERT_NE(androidCapability, capabilities.end());
    EXPECT_FALSE(androidCapability->adapterAvailable);
    EXPECT_FALSE(androidCapability->reason.empty());

    const auto formatted = registry.FormatCapabilities();
    EXPECT_NE(formatted.find("android"), std::string::npos);
    EXPECT_NE(formatted.find("windows-x64-dx12"), std::string::npos);
}
