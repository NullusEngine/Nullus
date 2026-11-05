#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace NLS::Detail
{
struct SvgPixels
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// A zero target size uses the SVG's intrinsic dimensions. A positive size
// scales the longest edge, preserving the aspect ratio.
bool RasterizeSvg(const uint8_t* encodedData, size_t encodedSize,
    SvgPixels& result, int targetSize = 0);
}
