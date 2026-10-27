#include <gtest/gtest.h>

#include "Build/BuildDependencyCollector.h"
#include "Build/BuildReport.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Guid.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
using NLS::Build::BuildDependencyCollector;
using NLS::Build::BuildProfile;
using NLS::Build::BuildReport;
using NLS::Core::Assets::ArtifactDatabase;
using NLS::Core::Assets::ArtifactManifest;
using NLS::Core::Assets::AssetDependencyKind;
using NLS::Core::Assets::AssetId;
using NLS::Core::Assets::AssetMeta;
using NLS::Core::Assets::AssetType;
using NLS::Core::Assets::ArtifactType;
using NLS::Core::Assets::ImportedArtifact;

struct SyntheticProject
{
    std::filesystem::path root;
    std::filesystem::path assets;
    std::filesystem::path artifactDatabase;
    AssetId sceneId;
    AssetId textureId;
    AssetId materialId;
    AssetId unusedMaterialId;
    std::string sceneArtifactPath;
    std::string textureArtifactPath;
    std::string materialArtifactPath;
    std::string unusedMaterialArtifactPath;

    std::filesystem::path ArtifactHostPath(const std::string& portable) const
    {
        return root / std::filesystem::path(portable);
    }
};

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string ArtifactRelativePath(const AssetId& id)
{
    // Artifact paths must be content-addressed: Library/Artifacts/<shard>/<64 hex>.
    auto name = id.ToString();
    name.erase(std::remove(name.begin(), name.end(), '-'), name.end());
    if (name.size() < 64u)
        name.resize(64u, '0');
    const auto shard = name.substr(0u, 2u);
    return "Library/Artifacts/" + shard + "/" + name;
}

void CreateArtifactFile(const SyntheticProject& project, const std::string& relative, const std::string& bytes)
{
    const auto path = project.ArtifactHostPath(relative);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
}

SyntheticProject BuildSyntheticProject(const std::string& name)
{
    SyntheticProject project;
    project.root = std::filesystem::temp_directory_path() /
        ("nullus-collector-" + name + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    project.assets = project.root / "Assets";
    project.artifactDatabase = project.root / "Library" / "ArtifactDB";
    std::filesystem::create_directories(project.assets);
    std::filesystem::create_directories(project.artifactDatabase);

    project.sceneId = AssetId(NLS::Guid::NewDeterministic("scene-" + name));
    project.textureId = AssetId(NLS::Guid::NewDeterministic("texture-" + name));
    project.materialId = AssetId(NLS::Guid::NewDeterministic("material-" + name));
    project.unusedMaterialId = AssetId(NLS::Guid::NewDeterministic("unused-material-" + name));

    // Source assets and their .meta sidecars.
    const auto scenePath = project.assets / "Scenes" / "Main.scene";
    // A valid scene document that references the paint material through a
    // PPtr property. Scenes are shipped as source documents, so dependency
    // collection must read the graph rather than require a scene artifact.
    std::ostringstream sceneJson;
    sceneJson
        << "{\n"
        << "  \"documentId\": \"" << NLS::Guid::NewDeterministic("scene-doc-" + name).ToString() << "\",\n"
        << "  \"root\": \"" << NLS::Guid::NewDeterministic("scene-root-" + name).ToString() << "\",\n"
        << "  \"format\": \"Nullus.ObjectGraph.Scene\",\n"
        << "  \"objects\": [\n"
        << "    {\n"
        << "      \"fileID\": 1,\n"
        << "      \"id\": \"" << NLS::Guid::NewDeterministic("scene-object-" + name).ToString() << "\",\n"
        << "      \"type\": \"Test.ReferenceHolder\",\n"
        << "      \"properties\": {\n"
        << "        \"material\": {\n"
        << "          \"fileID\": 12345,\n"
        << "          \"guid\": \"" << project.materialId.ToString() << "\",\n"
        << "          \"type\": 2,\n"
        << "          \"filePath\": \"Assets/Materials/Paint.mat\"\n"
        << "        }\n"
        << "      }\n"
        << "    }\n"
        << "  ]\n"
        << "}\n";
    WriteFile(scenePath, sceneJson.str());
    WriteFile(scenePath.string() + ".meta", "GUID=" + project.sceneId.ToString() + "\nASSET_TYPE=scene\n");
    const auto materialPath = project.assets / "Materials" / "Paint.mat";
    WriteFile(materialPath, "material");
    WriteFile(materialPath.string() + ".meta", "GUID=" + project.materialId.ToString() + "\nASSET_TYPE=material\n");
    const auto texturePath = project.assets / "Textures" / "Paint.png";
    WriteFile(texturePath, "texture");
    WriteFile(texturePath.string() + ".meta", "GUID=" + project.textureId.ToString() + "\nASSET_TYPE=texture\n");
    // Kept explicitly through a keep rule, not referenced by the start scene.
    const auto unusedMaterialPath = project.assets / "Materials" / "Unused.mat";
    WriteFile(unusedMaterialPath, "unused-material");
    WriteFile(
        unusedMaterialPath.string() + ".meta",
        "GUID=" + project.unusedMaterialId.ToString() + "\nASSET_TYPE=material\n");
    WriteFile(project.assets / "Scripts" / "Player.lua", "print('hello')\n");
    WriteFile(project.assets / "UI" / "Hud.ui", "{}");

    ArtifactDatabase database;

    ArtifactManifest sceneManifest;
    sceneManifest.sourceAssetId = project.sceneId;
    sceneManifest.importerId = "scene";
    sceneManifest.targetPlatform = "win64";
    sceneManifest.primarySubAssetKey = "main";
    project.sceneArtifactPath = ArtifactRelativePath(project.sceneId);
    ImportedArtifact sceneArtifact;
    sceneArtifact.sourceAssetId = project.sceneId;
    sceneArtifact.subAssetKey = "main";
    sceneArtifact.artifactType = ArtifactType::Scene;
    sceneArtifact.loaderId = "scene";
    sceneArtifact.targetPlatform = "win64";
    sceneArtifact.artifactPath = project.sceneArtifactPath;
    sceneArtifact.contentHash = "sha256:" + std::filesystem::path(project.sceneArtifactPath).filename().string();
    sceneManifest.subAssets.push_back(sceneArtifact);
    sceneManifest.dependencies.push_back({
        AssetDependencyKind::ImportedArtifact,
        project.materialId.ToString(),
        "main"});
    database.UpsertManifest(sceneManifest, "Assets/Scenes/Main.scene", NLS::Core::Assets::ArtifactRecordStatus::UpToDate);

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = project.materialId;
    materialManifest.importerId = "material";
    materialManifest.targetPlatform = "win64";
    materialManifest.primarySubAssetKey = "main";
    project.materialArtifactPath = ArtifactRelativePath(project.materialId);
    ImportedArtifact materialArtifact;
    materialArtifact.sourceAssetId = project.materialId;
    materialArtifact.subAssetKey = "main";
    materialArtifact.artifactType = ArtifactType::Material;
    materialArtifact.loaderId = "material";
    materialArtifact.targetPlatform = "win64";
    materialArtifact.artifactPath = project.materialArtifactPath;
    materialArtifact.contentHash = "sha256:" + std::filesystem::path(project.materialArtifactPath).filename().string();
    materialManifest.subAssets.push_back(materialArtifact);
    materialManifest.dependencies.push_back({
        AssetDependencyKind::ImportedArtifact,
        project.textureId.ToString(),
        "main"});
    database.UpsertManifest(materialManifest, "Assets/Materials/Paint.mat", NLS::Core::Assets::ArtifactRecordStatus::UpToDate);

    ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = project.textureId;
    textureManifest.importerId = "texture";
    textureManifest.targetPlatform = "win64";
    textureManifest.primarySubAssetKey = "main";
    project.textureArtifactPath = ArtifactRelativePath(project.textureId);
    ImportedArtifact textureArtifact;
    textureArtifact.sourceAssetId = project.textureId;
    textureArtifact.subAssetKey = "main";
    textureArtifact.artifactType = ArtifactType::Texture;
    textureArtifact.loaderId = "texture";
    textureArtifact.targetPlatform = "win64";
    textureArtifact.artifactPath = project.textureArtifactPath;
    textureArtifact.contentHash = "sha256:" + std::filesystem::path(project.textureArtifactPath).filename().string();
    textureManifest.subAssets.push_back(textureArtifact);
    database.UpsertManifest(textureManifest, "Assets/Textures/Paint.png", NLS::Core::Assets::ArtifactRecordStatus::UpToDate);

    ArtifactManifest unusedMaterialManifest;
    unusedMaterialManifest.sourceAssetId = project.unusedMaterialId;
    unusedMaterialManifest.importerId = "material";
    unusedMaterialManifest.targetPlatform = "win64";
    unusedMaterialManifest.primarySubAssetKey = "main";
    project.unusedMaterialArtifactPath = ArtifactRelativePath(project.unusedMaterialId);
    ImportedArtifact unusedMaterialArtifact;
    unusedMaterialArtifact.sourceAssetId = project.unusedMaterialId;
    unusedMaterialArtifact.subAssetKey = "main";
    unusedMaterialArtifact.artifactType = ArtifactType::Material;
    unusedMaterialArtifact.loaderId = "material";
    unusedMaterialArtifact.targetPlatform = "win64";
    unusedMaterialArtifact.artifactPath = project.unusedMaterialArtifactPath;
    unusedMaterialArtifact.contentHash =
        "sha256:" + std::filesystem::path(project.unusedMaterialArtifactPath).filename().string();
    unusedMaterialManifest.subAssets.push_back(unusedMaterialArtifact);
    database.UpsertManifest(
        unusedMaterialManifest,
        "Assets/Materials/Unused.mat",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);

    database.Save(project.artifactDatabase);
    CreateArtifactFile(project, project.sceneArtifactPath, "scene-artifact");
    CreateArtifactFile(project, project.materialArtifactPath, "material-artifact");
    CreateArtifactFile(project, project.textureArtifactPath, "texture-artifact");
    CreateArtifactFile(project, project.unusedMaterialArtifactPath, "unused-material-artifact");
    return project;
}

BuildProfile MakeProfile(const SyntheticProject& project)
{
    BuildProfile profile;
    profile.id = "windows-shipping";
    profile.displayName = "Windows Shipping";
    profile.productName = "CollectorTest";
    profile.executableName = "CollectorTest";
    profile.startScene = "Scenes/Main.scene";
    profile.mode = NLS::Build::BuildMode::Shipping;
    profile.target.platform = NLS::Build::PlatformFamily::Windows;
    profile.target.architecture = NLS::Build::Architecture::X64;
    profile.target.graphicsBackend = NLS::Build::GraphicsBackendKind::DX12;
    profile.target.artifactPlatform = "win64";
    profile.keepAssetPaths = {"Materials/Unused.mat"};
    (void)project;
    return profile;
}
} // namespace

TEST(BuildDependencyCollectorTests, CollectsNestedDependenciesAndRawAssets)
{
    const auto project = BuildSyntheticProject("nested");
    const auto profile = MakeProfile(project);

    BuildReport report;
    NLS::Build::DependencyCollectionResult result;
    ASSERT_TRUE(BuildDependencyCollector::Collect(
        project.root,
        project.assets,
        profile,
        project.artifactDatabase,
        result,
        report)) << report.FormatSummary();

    EXPECT_EQ(result.manifest.targetPlatform, "win64");
    EXPECT_EQ(result.manifest.entries.size(), 3u);
    EXPECT_EQ(result.artifacts.size(), 3u);

    const auto hasArtifact = [&result](const std::string& artifactPath)
    {
        return std::any_of(
            result.artifacts.begin(),
            result.artifacts.end(),
            [&artifactPath](const NLS::Build::CollectedArtifact& artifact)
            {
                return artifact.artifactPath == artifactPath;
            });
    };
    EXPECT_TRUE(hasArtifact(project.materialArtifactPath));
    EXPECT_TRUE(hasArtifact(project.textureArtifactPath));
    EXPECT_TRUE(hasArtifact(project.unusedMaterialArtifactPath));

    // A keep rule that names an asset by path resolves to the manifest's
    // primary sub-asset and is reported with the rule as its reason.
    const auto unusedReason = std::find_if(
        result.artifacts.begin(),
        result.artifacts.end(),
        [&project](const NLS::Build::CollectedArtifact& artifact)
        {
            return artifact.artifactPath == project.unusedMaterialArtifactPath;
        });
    ASSERT_NE(unusedReason, result.artifacts.end());
    EXPECT_EQ(unusedReason->reason, "keep-rule:Materials/Unused.mat");

    // The scene reference introduces the paint material; the material manifest
    // then introduces the texture.
    const auto materialReason = std::find_if(
        result.artifacts.begin(),
        result.artifacts.end(),
        [&project](const NLS::Build::CollectedArtifact& artifact)
        {
            return artifact.artifactPath == project.materialArtifactPath;
        });
    ASSERT_NE(materialReason, result.artifacts.end());
    EXPECT_EQ(materialReason->reason, "start-scene:reference");

    const auto textureReason = std::find_if(
        result.artifacts.begin(),
        result.artifacts.end(),
        [&project](const NLS::Build::CollectedArtifact& artifact)
        {
            return artifact.artifactPath == project.textureArtifactPath;
        });
    ASSERT_NE(textureReason, result.artifacts.end());
    EXPECT_EQ(textureReason->reason, "dependency-of:main");

    // Raw whitelisted assets and the scene file travel loose.
    const auto hasRaw = [&result](const std::string& suffix)
    {
        return std::any_of(
            result.rawFiles.begin(),
            result.rawFiles.end(),
            [&suffix](const NLS::Build::CollectedRawFile& file)
            {
                return file.hostPath.generic_string().find(suffix) != std::string::npos;
            });
    };
    EXPECT_TRUE(hasRaw("Scripts/Player.lua"));
    EXPECT_TRUE(hasRaw("UI/Hud.ui"));
    EXPECT_TRUE(hasRaw("Scenes/Main.scene"));

    std::error_code cleanupError;
    std::filesystem::remove_all(project.root, cleanupError);
}

TEST(BuildDependencyCollectorTests, MissingArtifactFailsTheBuild)
{
    auto project = BuildSyntheticProject("missing");
    std::filesystem::remove(project.ArtifactHostPath(project.textureArtifactPath));

    const auto profile = MakeProfile(project);
    BuildReport report;
    NLS::Build::DependencyCollectionResult result;
    EXPECT_FALSE(BuildDependencyCollector::Collect(
        project.root,
        project.assets,
        profile,
        project.artifactDatabase,
        result,
        report));
    EXPECT_TRUE(report.HasErrors());
    EXPECT_NE(report.FormatSummary().find("build-artifact-file-missing"), std::string::npos);

    std::error_code cleanupError;
    std::filesystem::remove_all(project.root, cleanupError);
}

TEST(BuildDependencyCollectorTests, DuplicateGuidFailsTheBuild)
{
    auto project = BuildSyntheticProject("duplicate");
    // Two meta files claiming the same GUID.
    WriteFile(
        project.assets / "Textures" / "Copy.png.meta",
        "GUID=" + project.textureId.ToString() + "\nASSET_TYPE=texture\n");

    const auto profile = MakeProfile(project);
    BuildReport report;
    NLS::Build::DependencyCollectionResult result;
    EXPECT_FALSE(BuildDependencyCollector::Collect(
        project.root,
        project.assets,
        profile,
        project.artifactDatabase,
        result,
        report));
    EXPECT_NE(report.FormatSummary().find("build-duplicate-guid"), std::string::npos);

    std::error_code cleanupError;
    std::filesystem::remove_all(project.root, cleanupError);
}

TEST(BuildDependencyCollectorTests, MissingStartSceneFailsTheBuild)
{
    const auto project = BuildSyntheticProject("scene-missing");
    auto profile = MakeProfile(project);
    profile.startScene = "Scenes/DoesNotExist.scene";

    BuildReport report;
    NLS::Build::DependencyCollectionResult result;
    EXPECT_FALSE(BuildDependencyCollector::Collect(
        project.root,
        project.assets,
        profile,
        project.artifactDatabase,
        result,
        report));
    EXPECT_NE(report.FormatSummary().find("build-start-scene-missing"), std::string::npos);

    std::error_code cleanupError;
    std::filesystem::remove_all(project.root, cleanupError);
}

TEST(BuildDependencyCollectorTests, WildcardKeepRulesAreSupported)
{
    EXPECT_TRUE(BuildDependencyCollector::MatchesWildcard("Scenes/*.scene", "Scenes/Main.scene"));
    EXPECT_TRUE(BuildDependencyCollector::MatchesWildcard("Textures/**", "Textures/UI/Icon.png"));
    EXPECT_FALSE(BuildDependencyCollector::MatchesWildcard("Scenes/*.scene", "Materials/Paint.mat"));
    EXPECT_TRUE(BuildDependencyCollector::MatchesWildcard("*", "anything"));
}
