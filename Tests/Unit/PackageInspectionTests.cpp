#include <gtest/gtest.h>

#include "Build/ArchiveWriter.h"
#include "Build/BuildFileUtils.h"
#include "Build/PackageInspection.h"

#include "Assets/ArtifactManifest.h"
#include "Assets/NpakArchive.h"
#include "Engine/Assets/PlayerDescriptor.h"
#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Engine/Assets/RuntimeManifestSerialization.h"
#include "Guid.h"

#include <filesystem>
#include <fstream>

namespace
{
using NLS::Build::PackageInspection;
using NLS::Core::Assets::NpakWriter;
using NLS::Engine::Assets::PlayerDescriptor;
using NLS::Engine::Assets::RuntimeAssetManifest;
using NLS::Engine::Assets::RuntimeAssetManifestEntry;

std::filesystem::path MakeTemporaryDirectory(const std::string& name)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("nullus-inspect-" + name + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

struct StagedProduct
{
    std::filesystem::path root;
    std::string artifactKey;
};

StagedProduct BuildStagedProduct(const std::string& name)
{
    StagedProduct product;
    product.root = MakeTemporaryDirectory(name);
    std::filesystem::create_directories(product.root / "Data" / "User" / "Packages");
    std::filesystem::create_directories(product.root / "Data" / "User" / "Assets" / "Scenes");

    const auto sceneId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("inspect-scene-" + name));
    const auto artifactPath = "Library/Artifacts/ab/" + std::string(64u, 'a');
    product.artifactKey = artifactPath;

    NpakWriter writer;
    const std::vector<uint8_t> payload {'a', 'r', 't', 'i', 'f', 'a', 'c', 't'};
    EXPECT_TRUE(writer.AddFile(artifactPath, payload));
    std::string error;
    EXPECT_TRUE(writer.Write(
        product.root / "Data" / "User" / "Packages" / "base.npak",
        {},
        &error)) << error;

    RuntimeAssetManifest manifest;
    manifest.targetPlatform = "win64";
    RuntimeAssetManifestEntry entry;
    entry.assetId = sceneId;
    entry.subAssetKey = "main";
    entry.artifactType = NLS::Core::Assets::ArtifactType::Scene;
    entry.loaderId = "scene";
    entry.artifactPath = artifactPath;
    entry.contentHash = "sha256:" + std::string(64u, 'a');
    manifest.entries.push_back(entry);
    manifest.roots.push_back({sceneId, "main"});
    std::ofstream manifestOutput(
        product.root / "Data" / "User" / "RuntimeAssetManifest.json",
        std::ios::binary | std::ios::trunc);
    manifestOutput << NLS::Engine::Assets::SerializeRuntimeAssetManifest(manifest);
    manifestOutput.close();

    std::ofstream gameIni(product.root / "Data" / "User" / "Game.ini", std::ios::binary | std::ios::trunc);
    gameIni << "start_scene=Scenes/Main.scene\n";
    gameIni.close();

    std::ofstream scene(product.root / "Data" / "User" / "Assets" / "Scenes" / "Main.scene");
    scene << "scene";
    scene.close();

    PlayerDescriptor descriptor;
    descriptor.playerId = "sample-player";
    descriptor.engineVersion = "1.0.0";
    descriptor.targetId = "windows-x64-dx12";
    descriptor.artifactPlatform = "win64";
    descriptor.buildMode = "Shipping";
    descriptor.productName = "Sample";
    descriptor.productVersion = "0.1.0";
    descriptor.startScene = "Scenes/Main.scene";
    descriptor.runtimeManifestPath = "Data/User/RuntimeAssetManifest.json";
    descriptor.contentPackages = {"Data/User/Packages/base.npak"};
    EXPECT_TRUE(descriptor.Save(product.root / PlayerDescriptor::FileName(), &error)) << error;
    return product;
}
} // namespace

TEST(PackageInspectionTests, ValidatesACompleteProduct)
{
    const auto product = BuildStagedProduct("valid");
    PackageInspection inspection;
    EXPECT_TRUE(NLS::Build::InspectPackage(product.root, inspection)) << NLS::Build::FormatInspection(inspection);
    EXPECT_TRUE(inspection.hasPlayerDescriptor);
    EXPECT_TRUE(inspection.hasRuntimeManifest);
    EXPECT_EQ(inspection.targetId, "windows-x64-dx12");
    EXPECT_EQ(inspection.contentPackages.size(), 1u);
    EXPECT_GT(inspection.totalContentBytes, 0u);
    EXPECT_TRUE(inspection.issues.empty());
    std::filesystem::remove_all(product.root);
}

TEST(PackageInspectionTests, MissingPackageIsReported)
{
    const auto product = BuildStagedProduct("missing-package");
    std::filesystem::remove(product.root / "Data" / "User" / "Packages" / "base.npak");

    PackageInspection inspection;
    EXPECT_FALSE(NLS::Build::InspectPackage(product.root, inspection));
    const auto hasCode = [&inspection](const std::string& code)
    {
        return std::any_of(
            inspection.issues.begin(),
            inspection.issues.end(),
            [&code](const NLS::Build::PackageValidationIssue& issue) { return issue.code == code; });
    };
    EXPECT_TRUE(hasCode("package-content-missing"));
    std::filesystem::remove_all(product.root);
}

TEST(PackageInspectionTests, UnresolvedManifestArtifactIsReported)
{
    auto product = BuildStagedProduct("unresolved");
    // Add a manifest entry that no package contains.
    RuntimeAssetManifest manifest;
    manifest.targetPlatform = "win64";
    RuntimeAssetManifestEntry entry;
    entry.assetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("orphan"));
    entry.subAssetKey = "main";
    entry.artifactPath = "Library/Artifacts/cc/" + std::string(64u, 'c');
    manifest.entries.push_back(entry);
    std::ofstream output(
        product.root / "Data" / "User" / "RuntimeAssetManifest.json",
        std::ios::binary | std::ios::trunc);
    output << NLS::Engine::Assets::SerializeRuntimeAssetManifest(manifest);
    output.close();

    PackageInspection inspection;
    EXPECT_FALSE(NLS::Build::InspectPackage(product.root, inspection));
    const auto hasCode = [&inspection](const std::string& code)
    {
        return std::any_of(
            inspection.issues.begin(),
            inspection.issues.end(),
            [&code](const NLS::Build::PackageValidationIssue& issue) { return issue.code == code; });
    };
    EXPECT_TRUE(hasCode("package-artifact-unresolved"));
    std::filesystem::remove_all(product.root);
}

TEST(PackageInspectionTests, CorruptPackagePayloadIsReported)
{
    auto product = BuildStagedProduct("corrupt");
    const auto packagePath = product.root / "Data" / "User" / "Packages" / "base.npak";
    std::vector<uint8_t> bytes;
    {
        std::ifstream input(packagePath, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(input.good());
        bytes.resize(static_cast<size_t>(input.tellg()));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_GT(bytes.size(), 200u);
    bytes[bytes.size() / 2u] ^= 0xFFu;
    {
        std::ofstream output(packagePath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    PackageInspection inspection;
    EXPECT_FALSE(NLS::Build::InspectPackage(product.root, inspection));
    const auto hasCode = [&inspection](const std::string& code)
    {
        return std::any_of(
            inspection.issues.begin(),
            inspection.issues.end(),
            [&code](const NLS::Build::PackageValidationIssue& issue) { return issue.code == code; });
    };
    EXPECT_TRUE(hasCode("package-content-corrupt") || hasCode("package-content-invalid"));
    std::filesystem::remove_all(product.root);
}

TEST(PackageInspectionTests, ValidatesZipArchives)
{
    const auto product = BuildStagedProduct("zip");
    const auto archivePath = product.root.parent_path() / (product.root.filename().string() + ".zip");

    NLS::Build::ZipArchiveWriter writer(archivePath);
    std::string error;
    ASSERT_TRUE(writer.AddDirectory(product.root.filename().string(), product.root, &error)) << error;
    ASSERT_TRUE(writer.Write(&error)) << error;

    PackageInspection inspection;
    EXPECT_TRUE(NLS::Build::ValidateArchive(archivePath, inspection))
        << NLS::Build::FormatInspection(inspection);

    std::filesystem::remove_all(product.root);
    std::filesystem::remove(archivePath);
}
