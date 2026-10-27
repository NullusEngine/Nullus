#include <gtest/gtest.h>

#include "Core/EditorResourceCatalog.h"
#include "Core/EditorResources.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NLS_ROOT_DIR
#error "NLS_ROOT_DIR must be defined for editor icon consistency tests"
#endif

namespace
{
using NLS::Editor::Core::EditorResourceCatalog;
using NLS::Editor::Core::EditorResourceRecord;
using NLS::Editor::Core::EditorResourceType;

std::pair<std::uint32_t, std::uint32_t> ReadPngDimensions(const std::filesystem::path& p_path)
{
    std::ifstream input(p_path, std::ios::binary);
    if (!input)
        return {};

    std::array<unsigned char, 24> header {};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()))
        return {};

    constexpr std::array<unsigned char, 8> pngSignature { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!std::equal(pngSignature.begin(), pngSignature.end(), header.begin()))
        return {};
    if (header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R')
        return {};

    const auto readBigEndian = [&header](const std::size_t offset)
    {
        return
            (static_cast<std::uint32_t>(header[offset]) << 24u) |
            (static_cast<std::uint32_t>(header[offset + 1u]) << 16u) |
            (static_cast<std::uint32_t>(header[offset + 2u]) << 8u) |
            static_cast<std::uint32_t>(header[offset + 3u]);
    };
    return { readBigEndian(16u), readBigEndian(20u) };
}

std::vector<const EditorResourceRecord*> CollectIconRecords()
{
    std::vector<const EditorResourceRecord*> records;
    for (const auto& record : EditorResourceCatalog::DefaultRecords())
    {
        if (record.type == EditorResourceType::Icon)
            records.push_back(&record);
    }
    return records;
}

std::filesystem::path RepoAssetsRoot()
{
    return std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets";
}

// editor.icon.asset.script.csharp -> asset-script-csharp.png
std::string ExpectedIconFileName(std::string_view p_iconId)
{
    constexpr std::string_view prefix = "editor.icon.";
    std::string stem(p_iconId.substr(prefix.size()));
    std::replace(stem.begin(), stem.end(), '.', '-');
    return stem + ".png";
}

// Generated records point at the largest tier (name@128.png); compare the
// logical stem so the invariant stays independent of the declared sizes.
std::string LogicalFileName(const std::filesystem::path& p_path)
{
    auto name = p_path.filename().generic_string();
    if (const auto at = name.find('@'); at != std::string::npos)
    {
        const auto dot = name.find('.', at);
        if (dot != std::string::npos)
            name.erase(at, dot - at);
    }
    return name;
}
} // namespace

TEST(EditorIconConsistencyTests, IconRecordsResolveToFilesNamedAfterTheirIds)
{
    const auto iconRecords = CollectIconRecords();
    ASSERT_FALSE(iconRecords.empty());

    for (const auto* record : iconRecords)
    {
        ASSERT_TRUE(record->id.starts_with("editor.icon.")) << record->id;
        const auto expectedFileName = ExpectedIconFileName(record->id);
        EXPECT_EQ(LogicalFileName(record->developmentPath), expectedFileName)
            << record->id << " -> " << record->developmentPath.generic_string();
        EXPECT_EQ(LogicalFileName(record->packagedPath), expectedFileName)
            << record->id << " -> " << record->packagedPath.generic_string();
    }
}

TEST(EditorIconConsistencyTests, IconRecordsExistOnDiskAsPngAssets)
{
    const auto assetsRoot = RepoAssetsRoot();
    const auto iconRecords = CollectIconRecords();
    ASSERT_FALSE(iconRecords.empty());

    for (const auto* record : iconRecords)
    {
        const auto iconPath = assetsRoot / record->developmentPath;
        ASSERT_TRUE(std::filesystem::is_regular_file(iconPath)) << iconPath.generic_string();

        const auto [width, height] = ReadPngDimensions(iconPath);
        EXPECT_GT(width, 0u) << iconPath.generic_string();
        EXPECT_GT(height, 0u) << iconPath.generic_string();
    }
}

TEST(EditorIconConsistencyTests, IconRecordsUseDistinctDevelopmentPaths)
{
    std::map<std::string, std::string> pathToId;
    for (const auto* record : CollectIconRecords())
    {
        const auto path = record->developmentPath.generic_string();
        const auto [iterator, inserted] = pathToId.emplace(path, record->id);
        EXPECT_TRUE(inserted)
            << record->id << " reuses " << path << " already owned by " << iterator->second;
    }
}

TEST(EditorIconConsistencyTests, PackagedPathsAreLowercaseRepositoryRelativeKeys)
{
    for (const auto* record : CollectIconRecords())
    {
        const auto packaged = record->packagedPath.generic_string();
        EXPECT_FALSE(record->packagedPath.is_absolute()) << packaged;
        EXPECT_FALSE(record->packagedPath.has_root_name()) << packaged;

        const bool lowercaseKey = !packaged.empty() && std::all_of(
            packaged.begin(),
            packaged.end(),
            [](const char character)
            {
                return (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9') ||
                    character == '/' || character == '-' || character == '_' ||
                    character == '.' || character == '@';
            });
        EXPECT_TRUE(lowercaseKey) << packaged;
    }
}

TEST(EditorIconConsistencyTests, FileIconLookupsOnlyReturnRegisteredIconIds)
{
    std::vector<std::string> registeredIcons;
    for (const auto* record : CollectIconRecords())
        registeredIcons.push_back(record->id);

    const std::vector<std::string> sampleFiles {
        "Hero.scene",
        "Hero.prefab",
        "Hero.mat",
        "Hero.vfx",
        "Hero.ui",
        "Hero.style",
        "Hero.loc",
        "Player.cs",
        "Player.CS",
        "Player.lua",
        "Player.py",
        "Mesh.fbx",
        "Mesh.obj",
        "Texture.png",
        "Texture.dds",
        "Sound.wav",
        "Font.ttf",
        "Font.otf",
        "Level.objectgraph.json",
        "Notes.txt",
        "NoExtension"
    };

    for (const auto& sample : sampleFiles)
    {
        const auto iconId = NLS::Editor::Core::EditorResources::GetFileIconId(sample);
        ASSERT_NE(iconId, nullptr) << sample;
        EXPECT_NE(
            std::find(registeredIcons.begin(), registeredIcons.end(), std::string(iconId)),
            registeredIcons.end())
            << sample << " -> " << iconId;
    }
}
