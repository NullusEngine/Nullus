#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RenderDef.h"

namespace NLS::Render::Tooling
{
    enum class MaterialVisualChannel : uint8_t
    {
        Red,
        Green,
        Blue
    };

    struct NLS_RENDER_API MaterialVisualRect
    {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 1.0f;
        float maxY = 1.0f;
    };

    struct NLS_RENDER_API MaterialVisualProbe
    {
        std::string fixtureName;
        MaterialVisualRect normalizedBounds;
        MaterialVisualChannel expectedDominantChannel = MaterialVisualChannel::Red;
    };

    struct NLS_RENDER_API MaterialVisualProbeResult
    {
        std::string fixtureName;
        MaterialVisualChannel expectedDominantChannel = MaterialVisualChannel::Red;
        uint32_t sampledPixels = 0u;
        float averageRed = 0.0f;
        float averageGreen = 0.0f;
        float averageBlue = 0.0f;
        float dominantCoverage = 0.0f;
        bool passed = false;
        std::string diagnostic;
    };

    struct NLS_RENDER_API MaterialVisualEvidenceResult
    {
        uint32_t width = 0u;
        uint32_t height = 0u;
        bool passed = false;
        std::vector<MaterialVisualProbeResult> probes;
        std::vector<std::string> diagnostics;
    };

    NLS_RENDER_API std::vector<MaterialVisualProbe> BuildDefaultImportedMaterialVisualProbes();

    NLS_RENDER_API MaterialVisualEvidenceResult AnalyzeMaterialVisualEvidence(
        const uint8_t* pixels,
        uint32_t width,
        uint32_t height,
        uint32_t channels,
        uint32_t rowStrideBytes,
        const std::vector<MaterialVisualProbe>& probes);

    NLS_RENDER_API std::string BuildMaterialVisualEvidenceReport(
        const MaterialVisualEvidenceResult& evidence);

    NLS_RENDER_API bool WriteMaterialVisualEvidencePng(
        const std::string& path,
        const uint8_t* pixels,
        uint32_t width,
        uint32_t height,
        uint32_t channels,
        uint32_t rowStrideBytes,
        std::string* errorMessage = nullptr);

    NLS_RENDER_API bool WriteMaterialVisualEvidenceReport(
        const std::string& path,
        const MaterialVisualEvidenceResult& evidence,
        std::string* errorMessage = nullptr);
}
