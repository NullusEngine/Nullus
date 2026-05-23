#include "Rendering/Tooling/MaterialVisualEvidence.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#define STBIWDEF static
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define stbiw__linear_to_rgbe nls_material_visual_evidence_stbiw__linear_to_rgbe
#define stbiw__write_run_data nls_material_visual_evidence_stbiw__write_run_data
#define stbiw__write_dump_data nls_material_visual_evidence_stbiw__write_dump_data
#define stbiw__write_hdr_scanline nls_material_visual_evidence_stbiw__write_hdr_scanline
#define stbi_zlib_compress nls_material_visual_evidence_stbi_zlib_compress
#define stbi_write_png_to_mem nls_material_visual_evidence_stbi_write_png_to_mem
#include <stb/stb_image_write.h>
#undef stbi_write_png_to_mem
#undef stbi_zlib_compress
#undef stbiw__write_hdr_scanline
#undef stbiw__write_dump_data
#undef stbiw__write_run_data
#undef stbiw__linear_to_rgbe

namespace NLS::Render::Tooling
{
    namespace
    {
        constexpr float kMinimumDominantCoverage = 0.02f;
        constexpr uint8_t kMinimumDominantValue = 20u;
        constexpr int kMinimumDominanceDelta = 12;

        const char* ToString(const MaterialVisualChannel channel)
        {
            switch (channel)
            {
            case MaterialVisualChannel::Red: return "red";
            case MaterialVisualChannel::Green: return "green";
            case MaterialVisualChannel::Blue: return "blue";
            default: return "unknown";
            }
        }

        uint8_t ExpectedChannelValue(
            const uint8_t r,
            const uint8_t g,
            const uint8_t b,
            const MaterialVisualChannel channel)
        {
            switch (channel)
            {
            case MaterialVisualChannel::Red: return r;
            case MaterialVisualChannel::Green: return g;
            case MaterialVisualChannel::Blue: return b;
            default: return 0u;
            }
        }

        uint8_t MaxOtherChannelValue(
            const uint8_t r,
            const uint8_t g,
            const uint8_t b,
            const MaterialVisualChannel channel)
        {
            switch (channel)
            {
            case MaterialVisualChannel::Red: return std::max(g, b);
            case MaterialVisualChannel::Green: return std::max(r, b);
            case MaterialVisualChannel::Blue: return std::max(r, g);
            default: return std::max({r, g, b});
            }
        }

        uint32_t ClampPixelFloor(const float normalized, const uint32_t size)
        {
            const float clamped = std::clamp(normalized, 0.0f, 1.0f);
            return static_cast<uint32_t>(std::floor(clamped * static_cast<float>(size)));
        }

        uint32_t ClampPixelCeil(const float normalized, const uint32_t size)
        {
            const float clamped = std::clamp(normalized, 0.0f, 1.0f);
            return static_cast<uint32_t>(std::ceil(clamped * static_cast<float>(size)));
        }

        bool EnsureParentDirectory(const std::string& path, std::string* errorMessage)
        {
            std::error_code error;
            const std::filesystem::path outputPath(path);
            const auto parent = outputPath.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, error);

            if (!error)
                return true;

            if (errorMessage != nullptr)
                *errorMessage = error.message();
            return false;
        }
    }

    std::vector<MaterialVisualProbe> BuildDefaultImportedMaterialVisualProbes()
    {
        return {
            {"glTF PBR fixture", {0.17f, 0.34f, 0.33f, 0.66f}, MaterialVisualChannel::Red},
            {"FBX parser fixture", {0.42f, 0.34f, 0.58f, 0.66f}, MaterialVisualChannel::Green},
            {"OBJ MTL fixture", {0.67f, 0.34f, 0.83f, 0.66f}, MaterialVisualChannel::Blue}
        };
    }

    MaterialVisualEvidenceResult AnalyzeMaterialVisualEvidence(
        const uint8_t* pixels,
        const uint32_t width,
        const uint32_t height,
        const uint32_t channels,
        const uint32_t rowStrideBytes,
        const std::vector<MaterialVisualProbe>& probes)
    {
        MaterialVisualEvidenceResult evidence;
        evidence.width = width;
        evidence.height = height;

        if (pixels == nullptr)
            evidence.diagnostics.push_back("pixel buffer is null");
        if (width == 0u || height == 0u)
            evidence.diagnostics.push_back("image dimensions are empty");
        if (channels < 3u)
            evidence.diagnostics.push_back("image must contain at least RGB channels");
        if (rowStrideBytes < width * channels)
            evidence.diagnostics.push_back("row stride is smaller than the image row size");
        if (probes.empty())
            evidence.diagnostics.push_back("no material visual probes were provided");

        if (!evidence.diagnostics.empty())
            return evidence;

        evidence.probes.reserve(probes.size());
        bool allPassed = true;

        for (const auto& probe : probes)
        {
            MaterialVisualProbeResult result;
            result.fixtureName = probe.fixtureName;
            result.expectedDominantChannel = probe.expectedDominantChannel;

            const uint32_t minX = std::min(ClampPixelFloor(probe.normalizedBounds.minX, width), width);
            const uint32_t minY = std::min(ClampPixelFloor(probe.normalizedBounds.minY, height), height);
            const uint32_t maxX = std::min(ClampPixelCeil(probe.normalizedBounds.maxX, width), width);
            const uint32_t maxY = std::min(ClampPixelCeil(probe.normalizedBounds.maxY, height), height);

            if (minX >= maxX || minY >= maxY)
            {
                result.diagnostic = "probe rectangle is empty after normalization";
                allPassed = false;
                evidence.probes.push_back(std::move(result));
                continue;
            }

            uint64_t redSum = 0u;
            uint64_t greenSum = 0u;
            uint64_t blueSum = 0u;
            uint32_t dominantPixels = 0u;

            for (uint32_t y = minY; y < maxY; ++y)
            {
                const uint8_t* row = pixels + static_cast<size_t>(y) * rowStrideBytes;
                for (uint32_t x = minX; x < maxX; ++x)
                {
                    const uint8_t* pixel = row + static_cast<size_t>(x) * channels;
                    const uint8_t r = pixel[0u];
                    const uint8_t g = pixel[1u];
                    const uint8_t b = pixel[2u];
                    redSum += r;
                    greenSum += g;
                    blueSum += b;
                    ++result.sampledPixels;

                    const uint8_t expected = ExpectedChannelValue(r, g, b, probe.expectedDominantChannel);
                    const uint8_t other = MaxOtherChannelValue(r, g, b, probe.expectedDominantChannel);
                    if (expected >= kMinimumDominantValue &&
                        static_cast<int>(expected) - static_cast<int>(other) >= kMinimumDominanceDelta)
                    {
                        ++dominantPixels;
                    }
                }
            }

            if (result.sampledPixels > 0u)
            {
                const float inv = 1.0f / (static_cast<float>(result.sampledPixels) * 255.0f);
                result.averageRed = static_cast<float>(redSum) * inv;
                result.averageGreen = static_cast<float>(greenSum) * inv;
                result.averageBlue = static_cast<float>(blueSum) * inv;
                result.dominantCoverage = static_cast<float>(dominantPixels) / static_cast<float>(result.sampledPixels);
            }

            result.passed = result.dominantCoverage >= kMinimumDominantCoverage;
            if (!result.passed)
            {
                std::ostringstream diagnostic;
                diagnostic << "expected " << ToString(probe.expectedDominantChannel)
                    << " dominance coverage >= " << kMinimumDominantCoverage
                    << ", observed " << result.dominantCoverage;
                result.diagnostic = diagnostic.str();
                allPassed = false;
            }

            evidence.probes.push_back(std::move(result));
        }

        evidence.passed = allPassed && evidence.probes.size() == probes.size();
        return evidence;
    }

    std::string BuildMaterialVisualEvidenceReport(const MaterialVisualEvidenceResult& evidence)
    {
        std::ostringstream report;
        report << "Material Visual Evidence: " << (evidence.passed ? "PASS" : "FAIL") << "\n";
        report << "Image: " << evidence.width << "x" << evidence.height << "\n";

        for (const auto& diagnostic : evidence.diagnostics)
            report << "Diagnostic: " << diagnostic << "\n";

        report << std::fixed << std::setprecision(4);
        for (const auto& probe : evidence.probes)
        {
            report << "Probe: " << probe.fixtureName
                << " expected=" << ToString(probe.expectedDominantChannel)
                << " samples=" << probe.sampledPixels
                << " avg=(" << probe.averageRed << ", " << probe.averageGreen << ", " << probe.averageBlue << ")"
                << " dominantCoverage=" << probe.dominantCoverage
                << " result=" << (probe.passed ? "PASS" : "FAIL");
            if (!probe.diagnostic.empty())
                report << " diagnostic=\"" << probe.diagnostic << "\"";
            report << "\n";
        }

        return report.str();
    }

    bool WriteMaterialVisualEvidencePng(
        const std::string& path,
        const uint8_t* pixels,
        const uint32_t width,
        const uint32_t height,
        const uint32_t channels,
        const uint32_t rowStrideBytes,
        std::string* errorMessage)
    {
        if (pixels == nullptr || width == 0u || height == 0u || channels < 3u)
        {
            if (errorMessage != nullptr)
                *errorMessage = "invalid image data";
            return false;
        }

        if (!EnsureParentDirectory(path, errorMessage))
            return false;

        const int written = stbi_write_png(
            path.c_str(),
            static_cast<int>(width),
            static_cast<int>(height),
            static_cast<int>(channels),
            pixels,
            static_cast<int>(rowStrideBytes));
        if (written != 0)
            return true;

        if (errorMessage != nullptr)
            *errorMessage = "stbi_write_png failed";
        return false;
    }

    bool WriteMaterialVisualEvidenceReport(
        const std::string& path,
        const MaterialVisualEvidenceResult& evidence,
        std::string* errorMessage)
    {
        if (!EnsureParentDirectory(path, errorMessage))
            return false;

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            if (errorMessage != nullptr)
                *errorMessage = "failed to open report file";
            return false;
        }

        output << BuildMaterialVisualEvidenceReport(evidence);
        return true;
    }
}
