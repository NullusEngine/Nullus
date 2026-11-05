#include <gtest/gtest.h>
#include "Build/PluginBuildInputs.h"
#include "Build/Windows/WindowsDesktopPlatform.h"
#include "Build/Unix/UnixDesktopPlatform.h"
#include "Cook/CookTypes.h"
#include "Core/Assets/ArtifactDatabase.h"
#include "Core/Assets/ContentHash.h"
#include "Core/Assets/PluginAssetPath.h"
#include "Core/Assets/NpakArchive.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Guid.h"
#include <Json/json.hpp>
#include <array>
#include <fstream>
namespace
{
struct AssetFixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("NullusPluginCook-" + NLS::Guid::New().ToString());
    std::filesystem::path content = root / "Plugins/Images/Content";
    AssetFixture()
    {
        std::filesystem::create_directories(content); std::filesystem::create_directories(root / "Packages");
        std::ofstream(root / "Packages/plugins.json") << R"({"schemaVersion":1,"enabled":{"org.fixture.images":true}})";
        std::ofstream(content.parent_path() / "plugin.json") << R"({"schemaVersion":1,"id":"org.fixture.images","name":"Images","version":"1.0.0","engineVersion":"^1.0.0","pluginApiVersion":1,"modules":[],"content":[{"name":"content","path":"Content","runtimeKeep":["icon.tga"]}]})";
        Pixel(255);
    }
    void Pixel(uint8_t blue)
    {
        std::array<uint8_t,21> tga{}; tga[2]=2; tga[12]=1; tga[14]=1; tga[16]=24; tga[17]=32; tga[18]=blue;
        std::ofstream file(content / "icon.tga",std::ios::binary); file.write(reinterpret_cast<const char*>(tga.data()),tga.size());
    }
    ~AssetFixture() { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
};
struct Cooked { NLS::Core::Assets::AssetId id; std::string artifactHash; size_t cooked,skipped; };
Cooked Cook(AssetFixture& fixture,const std::string& relative="icon.tga")
{
    NLS::Build::PluginBuildInputs inputs(fixture.root,"Windows");
    const auto asset = inputs.CookRoot() / "Assets/Plugins/org.fixture.images/content" / std::filesystem::u8path(relative);
    const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(asset));
    if (!meta) throw std::runtime_error("virtual metadata was not created");
    NLS::Cook::CookOptions options; options.projectRoot=inputs.CookRoot(); options.assetsRoot=options.projectRoot / "Assets";
    options.artifactRoot=fixture.root / "Library/Artifacts"; options.artifactDatabasePath=fixture.root / "Library/ArtifactDB";
    options.stagingRoot=fixture.root / "Library/Staging"; options.target.artifactPlatform="win64-dx12";
    const auto summary=NLS::Cook::AssetCooker::CookProject(options);
    if (!summary.Succeeded() || summary.cooked+summary.skipped != 1) throw std::runtime_error(summary.FormatSummary());
    NLS::Core::Assets::ArtifactDatabase database; if (!database.Load(options.artifactDatabasePath)) throw std::runtime_error("Cook did not publish ArtifactDB");
    const auto manifest=database.BuildManifestForSource(meta->id,options.target.artifactPlatform);
    if (!manifest || manifest->subAssets.empty()) throw std::runtime_error("Cook did not publish texture artifacts");
    const auto file=fixture.root / manifest->subAssets.front().artifactPath;
    // Read the actual persisted payload and include explicit dynamic resources
    // through the real NPAK writer, independently of stable asset identity.
    const auto hash=NLS::Core::Assets::HashContentFile(file);
    NLS::Core::Assets::NpakWriter writer; std::string error;
    for (const auto& raw : inputs.RawFiles())
        if (!writer.AddFileFromDisk(raw.relativePath,raw.hostPath,&error)) throw std::runtime_error(error);
    const auto pack=fixture.root / "retained.npak";
    if (!writer.Write(pack,{},&error)) throw std::runtime_error(error);
    auto archive=NLS::Core::Assets::NpakArchive::Open(pack,&error);
    if (!archive || !archive->VerifyPayload("Assets/Plugins/org.fixture.images/content/"+relative,&error)) throw std::runtime_error(error);
    return {meta->id,hash,summary.cooked,summary.skipped};
}
}
TEST(PluginAssetCook, StableGuidContentAndSettingsInvalidateRealArtifactsAndRetainDynamicResource)
{
    AssetFixture fixture;
    const auto first=Cook(fixture); EXPECT_EQ(first.cooked,1u);
    EXPECT_EQ(first.id.GetGuid(),NLS::Core::Assets::PluginAssetGuid("org.fixture.images","content","icon.tga"));
    EXPECT_FALSE(std::filesystem::exists(fixture.content / "icon.tga.meta"));
    const auto cached=Cook(fixture); EXPECT_EQ(cached.id,first.id); EXPECT_EQ(cached.skipped,1u); EXPECT_EQ(cached.artifactHash,first.artifactHash);
    fixture.Pixel(0); const auto changed=Cook(fixture); EXPECT_EQ(changed.id,first.id); EXPECT_EQ(changed.cooked,1u); EXPECT_NE(changed.artifactHash,first.artifactHash);
    auto meta=NLS::Core::Assets::AssetMeta::CreateForAsset(fixture.content / "icon.tga"); meta.id=first.id; meta.settings["TEXTURE_SRGB"]="false";
    ASSERT_TRUE(meta.Save(fixture.content / "icon.tga.meta"));
    const auto configured=Cook(fixture); EXPECT_EQ(configured.id,first.id); EXPECT_EQ(configured.cooked,1u);
    NLS::Core::Assets::ArtifactDatabase database; const auto path=fixture.root / "Library/ArtifactDB"; ASSERT_TRUE(database.Load(path));
    auto old=database.BuildManifestForSource(first.id,"win64-dx12"); ASSERT_TRUE(old); old->importerVersion=0;
    database.UpsertManifest(*old,"Assets/Plugins/org.fixture.images/content/icon.tga",NLS::Core::Assets::ArtifactRecordStatus::UpToDate); ASSERT_TRUE(database.Save(path));
    const auto importerChanged=Cook(fixture); EXPECT_EQ(importerChanged.id,first.id); EXPECT_EQ(importerChanged.cooked,1u); EXPECT_EQ(importerChanged.skipped,0u);
}
TEST(PlayerPluginIsolation, RejectsNativePackageToolsAndManagedEditorAssemblies)
{
    for (const auto* file : {"NLS_Packagesd.dll","NLS_Buildd.dll","NullusPlugins.exe","Scripts/Nullus.Editor.dll",
        "Scripts/GameScriptsEditor.dll","Scripts/Nullus.ScriptGenerator.dll","Assets/Scripts/Editor/Tool.cs"})
    {
        EXPECT_FALSE(NLS::Build::WindowsDesktopPlatform::IsRuntimeTemplateFile(file)) << file;
        EXPECT_FALSE(NLS::Build::UnixDesktopPlatform::IsRuntimeTemplateFile(file)) << file;
    }
    EXPECT_TRUE(NLS::Build::WindowsDesktopPlatform::IsRuntimeTemplateFile("NLS_Plugins.dll"));
    EXPECT_TRUE(NLS::Build::UnixDesktopPlatform::IsRuntimeTemplateFile("libNLS_Plugins.so"));
}
TEST(PluginAssetCook, NormalizedDynamicKeepUsesCanonicalPathInActualNpak)
{
    AssetFixture fixture;
    std::ofstream(fixture.content / std::filesystem::u8path("Cafe\xcc\x81.txt")) << "retained unicode data";
    const auto path=fixture.content.parent_path() / "plugin.json";
    std::ifstream input(path); auto manifest=nlohmann::json::parse(input); input.close();
    manifest["content"][0]["runtimeKeep"].push_back("Caf\xc3\xa9.txt"); std::ofstream(path) << manifest.dump(2);
    NLS::Build::PluginBuildInputs inputs(fixture.root,"Windows");
    NLS::Core::Assets::NpakWriter writer; std::string error;
    for (const auto& raw : inputs.RawFiles()) ASSERT_TRUE(writer.AddFileFromDisk(raw.relativePath,raw.hostPath,&error)) << error;
    const auto pack=fixture.root / "unicode-retained.npak"; ASSERT_TRUE(writer.Write(pack,{},&error)) << error;
    const auto archive=NLS::Core::Assets::NpakArchive::Open(pack,&error); ASSERT_TRUE(archive) << error;
    EXPECT_TRUE(archive->VerifyPayload("Assets/Plugins/org.fixture.images/content/Caf\xc3\xa9.txt",&error)) << error;
}
TEST(PluginAssetCook, DecomposedTextureFilenameCooksWithCanonicalStableIdentity)
{
    AssetFixture fixture; const std::string canonical="Caf\xc3\xa9.tga";
    std::filesystem::rename(fixture.content / "icon.tga",fixture.content / std::filesystem::u8path("Cafe\xcc\x81.tga"));
    const auto path=fixture.content.parent_path() / "plugin.json";
    std::ifstream input(path); auto manifest=nlohmann::json::parse(input); input.close();
    manifest["content"][0]["runtimeKeep"]=nlohmann::json::array({canonical}); std::ofstream(path) << manifest.dump(2);
    const auto first=Cook(fixture,canonical), cached=Cook(fixture,canonical);
    EXPECT_EQ(first.cooked,1u); EXPECT_EQ(cached.skipped,1u);
    EXPECT_EQ(first.id.GetGuid(),NLS::Core::Assets::PluginAssetGuid("org.fixture.images","content",canonical));
    EXPECT_EQ(first.id,cached.id); EXPECT_EQ(first.artifactHash,cached.artifactHash);
}
