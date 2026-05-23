#include <gtest/gtest.h>

#include "Rendering/Tooling/MaterialVisualEvidence.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    void FillRect(
        std::vector<uint8_t>& pixels,
        const uint32_t width,
        const uint32_t channels,
        const uint32_t x0,
        const uint32_t y0,
        const uint32_t x1,
        const uint32_t y1,
        const uint8_t r,
        const uint8_t g,
        const uint8_t b)
    {
        for (uint32_t y = y0; y < y1; ++y)
        {
            for (uint32_t x = x0; x < x1; ++x)
            {
                const size_t offset = (static_cast<size_t>(y) * width + x) * channels;
                pixels[offset + 0u] = r;
                pixels[offset + 1u] = g;
                pixels[offset + 2u] = b;
                pixels[offset + 3u] = 255u;
            }
        }
    }
}

TEST(MaterialVisualEvidenceTests, SyntheticImportedMaterialFixturePassesChannelDominance)
{
    constexpr uint32_t width = 120u;
    constexpr uint32_t height = 40u;
    constexpr uint32_t channels = 4u;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * channels, 8u);

    for (size_t i = 3u; i < pixels.size(); i += channels)
        pixels[i] = 255u;

    FillRect(pixels, width, channels, 10u, 10u, 30u, 30u, 230u, 30u, 20u);
    FillRect(pixels, width, channels, 50u, 10u, 70u, 30u, 35u, 210u, 45u);
    FillRect(pixels, width, channels, 90u, 10u, 110u, 30u, 25u, 55u, 235u);

    const std::vector<NLS::Render::Tooling::MaterialVisualProbe> probes = {
        {"glTF PBR", {0.05f, 0.2f, 0.30f, 0.8f}, NLS::Render::Tooling::MaterialVisualChannel::Red},
        {"FBX parser", {0.38f, 0.2f, 0.62f, 0.8f}, NLS::Render::Tooling::MaterialVisualChannel::Green},
        {"OBJ MTL", {0.72f, 0.2f, 0.95f, 0.8f}, NLS::Render::Tooling::MaterialVisualChannel::Blue}
    };

    const auto evidence = NLS::Render::Tooling::AnalyzeMaterialVisualEvidence(
        pixels.data(),
        width,
        height,
        channels,
        width * channels,
        probes);

    ASSERT_TRUE(evidence.passed) << NLS::Render::Tooling::BuildMaterialVisualEvidenceReport(evidence);
    ASSERT_EQ(evidence.probes.size(), 3u);
    EXPECT_GT(evidence.probes[0].dominantCoverage, 0.25f);
    EXPECT_GT(evidence.probes[1].dominantCoverage, 0.25f);
    EXPECT_GT(evidence.probes[2].dominantCoverage, 0.25f);
}

TEST(MaterialVisualEvidenceTests, MissingExpectedDominantChannelFailsWithDiagnostic)
{
    constexpr uint32_t width = 16u;
    constexpr uint32_t height = 16u;
    constexpr uint32_t channels = 4u;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * channels, 0u);

    for (size_t i = 0u; i < pixels.size(); i += channels)
    {
        pixels[i + 0u] = 40u;
        pixels[i + 1u] = 200u;
        pixels[i + 2u] = 40u;
        pixels[i + 3u] = 255u;
    }

    const std::vector<NLS::Render::Tooling::MaterialVisualProbe> probes = {
        {"expected red fixture", {0.0f, 0.0f, 1.0f, 1.0f}, NLS::Render::Tooling::MaterialVisualChannel::Red}
    };

    const auto evidence = NLS::Render::Tooling::AnalyzeMaterialVisualEvidence(
        pixels.data(),
        width,
        height,
        channels,
        width * channels,
        probes);

    EXPECT_FALSE(evidence.passed);
    ASSERT_EQ(evidence.probes.size(), 1u);
    EXPECT_FALSE(evidence.probes.front().passed);
    EXPECT_NE(
        NLS::Render::Tooling::BuildMaterialVisualEvidenceReport(evidence).find("expected red fixture"),
        std::string::npos);
}
