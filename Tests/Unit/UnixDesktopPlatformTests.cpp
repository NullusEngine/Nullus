#include <gtest/gtest.h>

#include "Build/BuildPipeline.h"
#include "Build/ArchiveWriter.h"
#include "Build/Unix/UnixDesktopPlatform.h"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
using NLS::Build::BuildContext;
using NLS::Build::BuildReport;
using NLS::Build::BuildTarget;
using NLS::Build::PlatformFamily;
using NLS::Build::Architecture;
using NLS::Build::GraphicsBackendKind;
using NLS::Build::UnixDesktopPlatform;

std::filesystem::path MakeTemporaryDirectory(const std::string& name)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("nullus-unix-adapter-" + name + "-" + std::to_string(
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

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

uint32_t ReadLe32(const std::vector<uint8_t>& bytes, const size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

uint16_t ReadLe16(const std::vector<uint8_t>& bytes, const size_t offset)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[offset]) |
        (static_cast<uint16_t>(bytes[offset + 1u]) << 8u));
}
} // namespace

TEST(UnixDesktopPlatformTests, ResolvesAndAssemblesLinuxTemplate)
{
    const auto root = MakeTemporaryDirectory("linux");
    const auto templateDirectory = root / "Linux_Release_Shared";
    WriteFile(templateDirectory / "Game", "#!/bin/sh\nexit 0\n");
    WriteFile(templateDirectory / "libNLS_Render.so", "shared-library");
    WriteFile(templateDirectory / "Assets" / "Engine" / "Shaders" / "Standard.hlsl", "shader");
    WriteFile(templateDirectory / "Scripts" / "GameScripts.dll", "managed");
    WriteFile(templateDirectory / "Editor", "editor");
    WriteFile(templateDirectory / "Editor.vulkan-gdb", "editor-debug");
    WriteFile(templateDirectory / "Editor.vulkan-smoke", "editor-smoke");
    WriteFile(templateDirectory / "Launcher", "launcher");
    WriteFile(templateDirectory / "Player.info", "{}");

    BuildContext context;
    context.projectRoot = root;
    context.playerTemplatePath = templateDirectory;
    context.stagingRoot = root / "stage";
    context.profile.productName = "LinuxGame";
    context.profile.executableName = "LinuxGame";
    context.profile.mode = NLS::Build::BuildMode::Shipping;
    context.profile.target.platform = PlatformFamily::Linux;
    context.profile.target.architecture = Architecture::X64;
    context.profile.target.graphicsBackend = GraphicsBackendKind::Vulkan;
    context.product.executableName = "LinuxGame";

    BuildTarget target = context.profile.target;
    UnixDesktopPlatform platform(PlatformFamily::Linux);
    const auto capability = platform.GetCapability(target);
    EXPECT_TRUE(capability.adapterAvailable);
    EXPECT_TRUE(capability.toolchainAvailable);

    BuildReport report;
    std::string error;
    const auto player = platform.ResolvePlayerTemplate(context, report, &error);
    ASSERT_TRUE(player.has_value()) << error;
    EXPECT_TRUE(std::any_of(
        player->runtimeFiles.begin(),
        player->runtimeFiles.end(),
        [](const std::filesystem::path& path)
        {
            return path.generic_string() == "Game";
        }));
    EXPECT_FALSE(std::any_of(
        player->runtimeFiles.begin(),
        player->runtimeFiles.end(),
        [](const std::filesystem::path& path)
        {
            const auto name = path.filename().string();
            return name.rfind("Editor", 0u) == 0u || name.rfind("Launcher", 0u) == 0u;
        }));

    ASSERT_TRUE(platform.AssemblePlayer(context, *player, report, &error)) << error;
    EXPECT_TRUE(std::filesystem::is_regular_file(context.stagingRoot / "LinuxGame"));
    EXPECT_TRUE(std::filesystem::is_regular_file(context.stagingRoot / "libNLS_Render.so"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        context.stagingRoot / "Assets" / "Engine" / "Shaders" / "Standard.hlsl"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        context.stagingRoot / "Scripts" / "GameScripts.dll"));
    EXPECT_FALSE(std::filesystem::exists(context.stagingRoot / "Editor"));
    EXPECT_FALSE(std::filesystem::exists(context.stagingRoot / "Editor.vulkan-gdb"));
    EXPECT_FALSE(std::filesystem::exists(context.stagingRoot / "Launcher"));

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

TEST(UnixDesktopPlatformTests, RejectsUnsupportedLinuxBackend)
{
    BuildTarget target;
    target.platform = PlatformFamily::Linux;
    target.architecture = Architecture::X64;
    target.graphicsBackend = GraphicsBackendKind::Metal;

    UnixDesktopPlatform platform(PlatformFamily::Linux);
    const auto capability = platform.GetCapability(target);
    EXPECT_FALSE(capability.adapterAvailable);
    EXPECT_FALSE(capability.toolchainAvailable);
    EXPECT_FALSE(capability.reason.empty());
}

TEST(UnixDesktopPlatformTests, ResolvesMacOSBundleAndUpdatesExecutableName)
{
    const auto root = MakeTemporaryDirectory("macos");
    const auto templateDirectory = root / "MacOS_Release_Shared";
    const auto bundle = templateDirectory / "Game.app";
    WriteFile(bundle / "Contents" / "MacOS" / "Game", "binary");
    WriteFile(
        bundle / "Contents" / "Info.plist",
        "<?xml version=\"1.0\"?><plist><dict>"
        "<key>CFBundleExecutable</key><string>Game</string>"
        "</dict></plist>");
    WriteFile(templateDirectory / "Assets" / "Engine" / "Shaders" / "Standard.hlsl", "shader");
    WriteFile(templateDirectory / "Editor", "editor");

    BuildContext context;
    context.projectRoot = root;
    context.playerTemplatePath = templateDirectory;
    context.stagingRoot = root / "stage";
    context.profile.productName = "MacGame";
    context.profile.executableName = "MacGame";
    context.profile.mode = NLS::Build::BuildMode::Shipping;
    context.profile.target.platform = PlatformFamily::MacOS;
    context.profile.target.architecture = Architecture::Arm64;
    context.profile.target.graphicsBackend = GraphicsBackendKind::Metal;
    context.product.executableName = "MacGame";

    UnixDesktopPlatform platform(PlatformFamily::MacOS);
    BuildReport report;
    std::string error;
    const auto player = platform.ResolvePlayerTemplate(context, report, &error);
    ASSERT_TRUE(player.has_value()) << error;
    ASSERT_TRUE(platform.AssemblePlayer(context, *player, report, &error)) << error;

    const auto stagedExecutable = context.stagingRoot / "Game.app" / "Contents" / "MacOS" / "MacGame";
    const auto plistPath = context.stagingRoot / "Game.app" / "Contents" / "Info.plist";
    EXPECT_TRUE(std::filesystem::is_regular_file(stagedExecutable));
    EXPECT_FALSE(std::filesystem::exists(context.stagingRoot / "Editor"));
    const auto plistBytes = ReadWholeFile(plistPath);
    const std::string plist(plistBytes.begin(), plistBytes.end());
    EXPECT_NE(plist.find("<string>MacGame</string>"), std::string::npos);
    EXPECT_EQ(plist.find("<string>Game</string>"), std::string::npos);

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

TEST(UnixDesktopPlatformTests, ZipMarksExecutableEntriesForPosixExtraction)
{
    const auto root = MakeTemporaryDirectory("zip-mode");
    const auto payload = root / "payload";
    WriteFile(payload / "Game", "binary");
    WriteFile(payload / "data.txt", "data");

    NLS::Build::ZipArchiveWriter archive(root / "package.zip");
    std::string error;
    ASSERT_TRUE(archive.AddDirectory(
        "Package",
        payload,
        &error,
        [](const std::string& path) { return path.ends_with("/Game"); }))
        << error;
    ASSERT_TRUE(archive.Write(&error)) << error;

    const auto bytes = ReadWholeFile(root / "package.zip");
    size_t offset = 0u;
    size_t checked = 0u;
    while (offset + 46u <= bytes.size())
    {
        const auto signature = ReadLe32(bytes, offset);
        if (signature != 0x02014b50u)
        {
            ++offset;
            continue;
        }
        const auto nameLength = ReadLe16(bytes, offset + 28u);
        const auto extraLength = ReadLe16(bytes, offset + 30u);
        const auto commentLength = ReadLe16(bytes, offset + 32u);
        ASSERT_LE(offset + 46u + nameLength, bytes.size());
        const std::string name(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + 46u),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + 46u + nameLength));
        const auto mode = (ReadLe32(bytes, offset + 38u) >> 16u) & 0xFFFFu;
        if (name == "Package/Game")
        {
            EXPECT_EQ(mode, 0100755u);
            ++checked;
        }
        else if (name == "Package/data.txt")
        {
            EXPECT_EQ(mode, 0100644u);
            ++checked;
        }
        offset += 46u + nameLength + extraLength + commentLength;
    }
    EXPECT_EQ(checked, 2u);

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}
