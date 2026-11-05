#include <gtest/gtest.h>

#include <Base/Image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::string_view asymmetricSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="3" viewBox="0 0 4 3"><rect x="0" y="0" width="2" height="1" fill="#ff0000"/></svg>)";

NLS::Image DecodeSvg(std::string_view source, bool flipped = false)
{
    return NLS::Image(reinterpret_cast<const uint8_t*>(source.data()), source.size(), flipped);
}

uint8_t AlphaAt(const NLS::Image& image, int x, int y)
{
    return image.GetData()[(static_cast<size_t>(y) * image.GetWidth() + x) * 4u + 3u];
}
}

TEST(ImageTests, DecodesSvgFromReadOnlyMemoryWithTransparentBackground)
{
    const auto image = DecodeSvg(asymmetricSvg);
    ASSERT_NE(image.GetData(), nullptr);
    ASSERT_EQ(image.GetWidth(), 4);
    ASSERT_EQ(image.GetHeight(), 3);
    ASSERT_EQ(image.GetChannels(), 4);
    EXPECT_EQ(image.GetData()[0], 255);
    EXPECT_EQ(image.GetData()[1], 0);
    EXPECT_EQ(image.GetData()[2], 0);
    EXPECT_EQ(AlphaAt(image, 0, 0), 255);
    EXPECT_EQ(AlphaAt(image, 3, 0), 0);
    EXPECT_EQ(AlphaAt(image, 0, 2), 0);
}

TEST(ImageTests, AcceptsSvgWithUtf8BomAndXmlDeclaration)
{
    const std::string source = std::string("\xEF\xBB\xBF") +
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + std::string(asymmetricSvg);
    const auto image = DecodeSvg(source);
    ASSERT_NE(image.GetData(), nullptr);
    EXPECT_EQ(image.GetWidth(), 4);
    EXPECT_EQ(image.GetHeight(), 3);
    EXPECT_EQ(AlphaAt(image, 0, 0), 255);
}

TEST(ImageTests, FlipsSvgRowsForTextureUploads)
{
    const auto upright = DecodeSvg(asymmetricSvg);
    const auto flipped = DecodeSvg(asymmetricSvg, true);
    ASSERT_NE(upright.GetData(), nullptr);
    ASSERT_NE(flipped.GetData(), nullptr);
    ASSERT_EQ(upright.GetChannels(), 4);
    ASSERT_EQ(flipped.GetChannels(), 4);
    for (int y = 0; y < upright.GetHeight(); ++y)
        for (int x = 0; x < upright.GetWidth() * 4; ++x)
            EXPECT_EQ(upright.GetData()[y * upright.GetWidth() * 4 + x],
                flipped.GetData()[(upright.GetHeight() - 1 - y) * upright.GetWidth() * 4 + x]);
}

TEST(ImageTests, ConcurrentSvgLoadsKeepOrientationIndependent)
{
    auto load = [](bool flipped)
    {
        for (int i = 0; i < 20; ++i)
        {
            const auto image = DecodeSvg(asymmetricSvg, flipped);
            if (image.GetData() == nullptr || image.GetChannels() != 4 ||
                AlphaAt(image, 0, flipped ? 2 : 0) != 255 ||
                AlphaAt(image, 0, flipped ? 0 : 2) != 0)
                return false;
        }
        return true;
    };
    auto upright = std::async(std::launch::async, load, false);
    auto flipped = std::async(std::launch::async, load, true);
    EXPECT_TRUE(upright.get());
    EXPECT_TRUE(flipped.get());
}

TEST(ImageTests, RejectsInvalidAndOversizedSvgWithoutKeepingDimensions)
{
    constexpr std::array<std::string_view, 5> invalid {
        "not an image", "<svg/>",
        "<svg width=\"0\" height=\"8\"></svg>",
        "<svg width=\"500000\" height=\"500000\"></svg>",
        "<svg width=\"nan\" height=\"4\"></svg>"
    };
    for (const auto source : invalid)
    {
        const auto image = DecodeSvg(source);
        EXPECT_EQ(image.GetData(), nullptr) << source;
        EXPECT_EQ(image.GetWidth(), 0) << source;
        EXPECT_EQ(image.GetHeight(), 0) << source;
        EXPECT_EQ(image.GetChannels(), 0) << source;
    }
    NLS::Image empty(nullptr, 0);
    EXPECT_EQ(empty.GetData(), nullptr);
    NLS::Image tooLarge(reinterpret_cast<const uint8_t*>(asymmetricSvg.data()),
        std::numeric_limits<size_t>::max());
    EXPECT_EQ(tooLarge.GetData(), nullptr);
}

TEST(ImageTests, LoadsEveryLogoSvgAndPreservesTheDiamondCutout)
{
    const std::filesystem::path root(NLS_ROOT_DIR);
    for (const auto scope : { "Editor", "Engine" })
    {
        const auto filename = root / "App" / "Assets" / scope / "Brand" / "NullusLogoMark.svg";
        const NLS::Image image(filename.string());
        ASSERT_NE(image.GetData(), nullptr) << filename;
        ASSERT_EQ(image.GetWidth(), 576);
        ASSERT_EQ(image.GetHeight(), 576);
        ASSERT_EQ(image.GetChannels(), 4);
        EXPECT_EQ(AlphaAt(image, 0, 0), 0);
        EXPECT_EQ(AlphaAt(image, 288, 129), 0);
        EXPECT_EQ(AlphaAt(image, 100, 250), 255);
        EXPECT_EQ(AlphaAt(image, 288, 503), 255);
        EXPECT_EQ(image.GetData()[(250 * 576 + 100) * 4], 8);
    }
    const NLS::Image logo((root / "NullusLogo.svg").string());
    ASSERT_NE(logo.GetData(), nullptr);
    EXPECT_EQ(logo.GetWidth(), 1254);
    EXPECT_EQ(logo.GetHeight(), 1254);
    EXPECT_EQ(logo.GetChannels(), 4);
    EXPECT_EQ(AlphaAt(logo, 240, 910), 255); // Outlined wordmark.
}

TEST(ImageTests, PreservesPngFileAndMemoryDecoding)
{
    struct TemporaryPng
    {
        std::filesystem::path path;
        ~TemporaryPng()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    };
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const TemporaryPng temporary { std::filesystem::temp_directory_path() /
        ("nullus_image_roundtrip_" + std::to_string(suffix) + ".png") };
    constexpr std::array<unsigned char, 8> pixels { 255, 0, 0, 255, 0, 128, 255, 64 };
    NLS::Image source(2, 1, 4);
    source.SetData(pixels.data());
    source.Save(temporary.path.string());
    std::ifstream file(temporary.path, std::ios::binary);
    const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)), {});
    const NLS::Image fromFile(temporary.path.string());
    const NLS::Image fromMemory(encoded.data(), encoded.size());
    for (const auto* image : { &fromFile, &fromMemory })
    {
        ASSERT_NE(image->GetData(), nullptr);
        ASSERT_EQ(image->GetWidth(), 2);
        ASSERT_EQ(image->GetHeight(), 1);
        ASSERT_EQ(image->GetChannels(), 4);
        EXPECT_TRUE(std::equal(pixels.begin(), pixels.end(), image->GetData()));
    }
}
