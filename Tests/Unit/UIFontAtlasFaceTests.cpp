#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "Rendering/UI/UIFontAtlas.h"

namespace
{
    std::vector<uint8_t> ReadFontBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return {};
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
    }

    std::filesystem::path EngineFontPath()
    {
        return std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Fonts/Ruda-Bold.ttf";
    }
}

class UIFontAtlasFaceTests : public ::testing::Test
{
protected:
    void TearDown() override
    {
        NLS::Render::UI::ClearUIFontFaceProvider();
    }
};

TEST_F(UIFontAtlasFaceTests, RejectsNonFontPayloadInsteadOfParsingIt)
{
    NLS::Render::UI::UIFontAtlas atlas;
    const std::vector<uint8_t> descriptor {
        't', 'y', 'p', 'e', '=', 'F', 'o', 'n', 't', '\n' };
    EXPECT_FALSE(atlas.SetFaceData(9u, descriptor.data(), descriptor.size()));
    EXPECT_FALSE(atlas.HasFace(9u));
    EXPECT_EQ(atlas.GetFaceCount(), 0u);
}

TEST_F(UIFontAtlasFaceTests, ProviderLoadsProjectFaceAndKeepsFacesIsolated)
{
    const auto faceBytes = ReadFontBytes(EngineFontPath());
    ASSERT_FALSE(faceBytes.empty());

    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(512u, 512u, 4u);
    ASSERT_TRUE(atlas.LoadFontFromFile(EngineFontPath()));
    EXPECT_TRUE(atlas.HasFace(1u));
    EXPECT_EQ(atlas.GetFaceCount(), 1u);

    std::atomic<int> providerCalls { 0 };
    NLS::Render::UI::SetUIFontFaceProvider(
        [&](const uint64_t fontAssetId, std::vector<uint8_t>& bytes)
        {
            ++providerCalls;
            if (fontAssetId != 2u)
                return false;
            bytes = faceBytes;
            return true;
        });

    NLS::Render::UI::UIFontGlyph defaultGlyph;
    ASSERT_TRUE(atlas.EnsureGlyph({ 1u, 'A', 24u }, 1u, defaultGlyph));
    EXPECT_FALSE(defaultGlyph.fallback);
    EXPECT_EQ(providerCalls.load(), 0);

    NLS::Render::UI::UIFontGlyph projectGlyph;
    ASSERT_TRUE(atlas.EnsureGlyph({ 2u, 'A', 24u }, 2u, projectGlyph));
    EXPECT_FALSE(projectGlyph.fallback);
    EXPECT_TRUE(atlas.HasFace(2u));
    EXPECT_EQ(atlas.GetFaceCount(), 2u);
    EXPECT_EQ(providerCalls.load(), 1);
    EXPECT_EQ(atlas.GetDiagnostics().fallbackFaceGlyphs, 0u);

    EXPECT_TRUE(atlas.Find({ 1u, 'A', 24u }).has_value());
    EXPECT_TRUE(atlas.Find({ 2u, 'A', 24u }).has_value());

    // The face is cached; a second codepoint must not hit the provider again.
    NLS::Render::UI::UIFontGlyph projectB;
    ASSERT_TRUE(atlas.EnsureGlyph({ 2u, 'B', 24u }, 3u, projectB));
    EXPECT_EQ(providerCalls.load(), 1);
}

TEST_F(UIFontAtlasFaceTests, MissingFaceFallsBackToDefaultAndCachesFailure)
{
    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(256u, 256u, 4u);
    ASSERT_TRUE(atlas.LoadFontFromFile(EngineFontPath()));

    std::atomic<int> providerCalls { 0 };
    NLS::Render::UI::SetUIFontFaceProvider(
        [&](const uint64_t, std::vector<uint8_t>&)
        {
            ++providerCalls;
            return false;
        });

    NLS::Render::UI::UIFontGlyph glyph;
    ASSERT_TRUE(atlas.EnsureGlyph({ 7u, 'A', 24u }, 1u, glyph));
    EXPECT_FALSE(glyph.fallback) << "missing project face should still use the default glyph";
    EXPECT_FALSE(atlas.HasFace(7u));
    EXPECT_EQ(providerCalls.load(), 1);
    EXPECT_EQ(atlas.GetDiagnostics().fallbackFaceGlyphs, 1u);

    NLS::Render::UI::UIFontGlyph next;
    ASSERT_TRUE(atlas.EnsureGlyph({ 7u, 'B', 24u }, 2u, next));
    EXPECT_EQ(providerCalls.load(), 1) << "failed provider lookups must be cached";
    EXPECT_EQ(atlas.GetDiagnostics().fallbackFaceGlyphs, 2u);

    // Clearing failures allows a later retry (for example after the font is
    // imported) without discarding cached glyphs from other faces.
    atlas.ClearFaceFailures();
    NLS::Render::UI::UIFontGlyph retry;
    ASSERT_TRUE(atlas.EnsureGlyph({ 7u, 'C', 24u }, 3u, retry));
    EXPECT_EQ(providerCalls.load(), 2);
}

TEST_F(UIFontAtlasFaceTests, InvalidateFontDropsFaceAndReloadsItOnDemand)
{
    const auto faceBytes = ReadFontBytes(EngineFontPath());
    ASSERT_FALSE(faceBytes.empty());

    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(256u, 256u, 4u);
    ASSERT_TRUE(atlas.LoadFontFromFile(EngineFontPath()));

    std::atomic<int> providerCalls { 0 };
    NLS::Render::UI::SetUIFontFaceProvider(
        [&](const uint64_t fontAssetId, std::vector<uint8_t>& bytes)
        {
            if (fontAssetId != 2u)
                return false;
            ++providerCalls;
            bytes = faceBytes;
            return true;
        });

    NLS::Render::UI::UIFontGlyph glyph;
    ASSERT_TRUE(atlas.EnsureGlyph({ 2u, 'A', 24u }, 1u, glyph));
    EXPECT_EQ(providerCalls.load(), 1);
    EXPECT_TRUE(atlas.HasFace(2u));

    atlas.InvalidateFont(2u, 100u);
    atlas.CollectRetiredPages(100u);
    EXPECT_FALSE(atlas.HasFace(2u));
    EXPECT_TRUE(atlas.HasFace(1u));

    NLS::Render::UI::UIFontGlyph reloaded;
    ASSERT_TRUE(atlas.EnsureGlyph({ 2u, 'B', 24u }, 101u, reloaded));
    EXPECT_FALSE(reloaded.fallback);
    EXPECT_EQ(providerCalls.load(), 2);
    EXPECT_TRUE(atlas.HasFace(2u));
}

TEST_F(UIFontAtlasFaceTests, FaceEvictionKeepsDefaultAndNewestFace)
{
    const auto faceBytes = ReadFontBytes(EngineFontPath());
    ASSERT_FALSE(faceBytes.empty());

    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(256u, 256u, 4u);
    ASSERT_TRUE(atlas.LoadFontFromFile(EngineFontPath()));
    atlas.SetMaxFaces(2u);

    NLS::Render::UI::SetUIFontFaceProvider(
        [&](const uint64_t fontAssetId, std::vector<uint8_t>& bytes)
        {
            if (fontAssetId != 2u && fontAssetId != 3u)
                return false;
            bytes = faceBytes;
            return true;
        });

    NLS::Render::UI::UIFontGlyph second;
    ASSERT_TRUE(atlas.EnsureGlyph({ 2u, 'A', 16u }, 1u, second));
    EXPECT_TRUE(atlas.HasFace(2u));
    EXPECT_EQ(atlas.GetFaceCount(), 2u);

    NLS::Render::UI::UIFontGlyph third;
    ASSERT_TRUE(atlas.EnsureGlyph({ 3u, 'A', 16u }, 2u, third));
    EXPECT_TRUE(atlas.HasFace(3u));
    EXPECT_TRUE(atlas.HasFace(1u));
    EXPECT_FALSE(atlas.HasFace(2u));
    EXPECT_EQ(atlas.GetFaceCount(), 2u);
}

TEST_F(UIFontAtlasFaceTests, NoFontDataAndUnavailableFontStillProduceTofu)
{
    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(128u, 128u, 2u);

    NLS::Render::UI::UIFontGlyph noData;
    ASSERT_TRUE(atlas.EnsureGlyph({ 1u, 'A', 16u }, 1u, noData, true));
    EXPECT_TRUE(noData.fallback);
    EXPECT_FALSE(atlas.HasFontData());

    ASSERT_TRUE(atlas.LoadFontFromFile(EngineFontPath()));
    ASSERT_TRUE(atlas.HasFontData());

    // fontAvailable=false is the explicit "do not use font data" path used by
    // diagnostics/preview callers and must keep the old tofu behavior.
    NLS::Render::UI::UIFontGlyph unavailable;
    ASSERT_TRUE(atlas.EnsureGlyph({ 1u, 'B', 16u }, 2u, unavailable, false));
    EXPECT_TRUE(unavailable.fallback);
}

TEST_F(UIFontAtlasFaceTests, ExplicitFaceDataOverridesProvider)
{
    const auto faceBytes = ReadFontBytes(EngineFontPath());
    ASSERT_FALSE(faceBytes.empty());

    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(256u, 256u, 4u);
    std::atomic<int> providerCalls { 0 };
    NLS::Render::UI::SetUIFontFaceProvider(
        [&](const uint64_t, std::vector<uint8_t>&)
        {
            ++providerCalls;
            return false;
        });

    ASSERT_TRUE(atlas.SetFaceData(5u, faceBytes.data(), faceBytes.size()));
    EXPECT_TRUE(atlas.HasFace(5u));

    NLS::Render::UI::UIFontGlyph glyph;
    ASSERT_TRUE(atlas.EnsureGlyph({ 5u, 'A', 20u }, 1u, glyph));
    EXPECT_FALSE(glyph.fallback);
    EXPECT_EQ(providerCalls.load(), 0);
}
