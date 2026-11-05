#include "SvgRasterizer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#define NANOSVG_IMPLEMENTATION
#include <nanosvg/nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg/nanosvgrast.h>

namespace NLS::Detail
{
bool RasterizeSvg(const uint8_t* encodedData, size_t encodedSize,
    SvgPixels& result, int targetSize)
{
    result = {};
    constexpr size_t maxEncodedSize = 16u * 1024u * 1024u;
    constexpr int maxDimension = 4096;
    if (encodedData == nullptr || encodedSize == 0 || encodedSize > maxEncodedSize ||
        targetSize < 0 || targetSize > maxDimension)
        return false;

    // NanoSVG modifies its input and expects a terminating zero. Do not pass
    // the caller's buffer, which may be read-only or shared with another load.
    std::string source(reinterpret_cast<const char*>(encodedData), encodedSize);
    const size_t prefix = source.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    const auto first = source.find_first_not_of(" \t\r\n", prefix);
    if (first == std::string::npos || source[first] != '<' ||
        source.find("<svg", first) == std::string::npos)
        return false;

    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(
        nsvgParse(source.data() + prefix, "px", 96.0f), nsvgDelete);
    if (!image || !std::isfinite(image->width) || !std::isfinite(image->height) ||
        image->width <= 0.0f || image->height <= 0.0f ||
        image->width > maxDimension || image->height > maxDimension)
        return false;

    const float scale = targetSize > 0 ?
        static_cast<float>(targetSize) / std::max(image->width, image->height) : 1.0f;
    const int width = std::clamp(static_cast<int>(std::ceil(image->width * scale)), 1, maxDimension);
    const int height = std::clamp(static_cast<int>(std::ceil(image->height * scale)), 1, maxDimension);
    std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> rasterizer(
        nsvgCreateRasterizer(), nsvgDeleteRasterizer);
    if (!rasterizer)
        return false;

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u, 0u);
    nsvgRasterize(rasterizer.get(), image.get(), 0.0f, 0.0f, scale,
        rgba.data(), width, height, width * 4);
    result.width = width;
    result.height = height;
    result.rgba = std::move(rgba);
    return true;
}
}
