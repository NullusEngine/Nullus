Texture2D<float> u_HZB : register(t0, space1);

cbuffer HZBOcclusionConstants : register(b1, space1)
{
    uint u_OcclusionPrimitiveCount;
    uint3 u_OcclusionConstantsPadding;
};

struct OcclusionPrimitiveInput
{
    // Must match NLS::Engine::Rendering::SceneOcclusionPrimitivePacket.
    float2 screenMin;
    float2 screenMax;
    float nearestDepth;
    uint flags;
};

StructuredBuffer<OcclusionPrimitiveInput> u_OcclusionPrimitiveInputs : register(t2, space1);
RWStructuredBuffer<uint> u_OcclusionPrimitiveResults : register(u3, space1);

static const float kHZBOcclusionDepthBias = 0.00001f;
static const uint kHZBOcclusionCoverageGridDimension = 8u;

uint SelectHZBMipLevel(uint footprintWidth, uint footprintHeight, uint mipCount)
{
    const uint largestFootprint = max(footprintWidth, footprintHeight);
    uint mipLevel = 0u;
    uint coveredPixels = kHZBOcclusionCoverageGridDimension;
    while (mipLevel + 1u < mipCount && largestFootprint > coveredPixels)
    {
        ++mipLevel;
        coveredPixels <<= 1u;
    }
    return mipLevel;
}

float LoadHZB(uint2 pixel, uint mipLevel)
{
    return u_HZB.Load(int3(pixel, mipLevel));
}

bool HZBSampleOccludes(uint2 pixel, uint mipLevel, float nearestOccluderDepth)
{
    return LoadHZB(pixel, mipLevel) < nearestOccluderDepth;
}

bool HZBCoverageSampleOccludes(
    uint width,
    uint height,
    uint pixelX,
    uint pixelY,
    uint mipLevel,
    float nearestOccluderDepth)
{
    return HZBSampleOccludes(
        uint2(min(pixelX, width - 1u), min(pixelY, height - 1u)),
        mipLevel,
        nearestOccluderDepth);
}

bool IsConservativelyOccludedByHZBCoverage(
    uint width,
    uint height,
    float2 minPixel,
    float2 maxPixel,
    uint mipCount,
    float nearestOccluderDepth)
{
    const float2 clampedMin = clamp(minPixel, float2(0.0f, 0.0f), float2((float)width - 1.0f, (float)height - 1.0f));
    const float2 clampedMax = clamp(maxPixel, float2(0.0f, 0.0f), float2((float)width - 1.0f, (float)height - 1.0f));
    const uint minPixelX = (uint)floor(min(clampedMin.x, clampedMax.x));
    const uint minPixelY = (uint)floor(min(clampedMin.y, clampedMax.y));
    const uint maxPixelX = (uint)ceil(max(clampedMin.x, clampedMax.x));
    const uint maxPixelY = (uint)ceil(max(clampedMin.y, clampedMax.y));
    if (maxPixelX < minPixelX || maxPixelY < minPixelY)
        return false;

    const uint footprintWidth = maxPixelX - minPixelX + 1u;
    const uint footprintHeight = maxPixelY - minPixelY + 1u;
    const uint mipLevel = SelectHZBMipLevel(footprintWidth, footprintHeight, mipCount);
    const uint mipScale = 1u << mipLevel;
    const uint mipWidth = max(1u, (width + mipScale - 1u) / mipScale);
    const uint mipHeight = max(1u, (height + mipScale - 1u) / mipScale);
    const uint mipMinPixelX = minPixelX >> mipLevel;
    const uint mipMinPixelY = minPixelY >> mipLevel;
    const uint mipMaxPixelX = min(maxPixelX >> mipLevel, mipWidth - 1u);
    const uint mipMaxPixelY = min(maxPixelY >> mipLevel, mipHeight - 1u);
    const uint mipFootprintWidth = mipMaxPixelX - mipMinPixelX + 1u;
    const uint mipFootprintHeight = mipMaxPixelY - mipMinPixelY + 1u;
    const uint stepX = max(1u, mipFootprintWidth / kHZBOcclusionCoverageGridDimension);
    const uint stepY = max(1u, mipFootprintHeight / kHZBOcclusionCoverageGridDimension);

    for (uint pixelY = mipMinPixelY; pixelY <= mipMaxPixelY; pixelY += stepY)
    {
        for (uint pixelX = mipMinPixelX; pixelX <= mipMaxPixelX; pixelX += stepX)
        {
            if (!HZBCoverageSampleOccludes(mipWidth, mipHeight, pixelX, pixelY, mipLevel, nearestOccluderDepth))
                return false;
        }
    }
    for (uint pixelX = mipMinPixelX; pixelX <= mipMaxPixelX; pixelX += stepX)
    {
        if (!HZBCoverageSampleOccludes(mipWidth, mipHeight, pixelX, mipMaxPixelY, mipLevel, nearestOccluderDepth))
            return false;
    }
    for (uint pixelY = mipMinPixelY; pixelY <= mipMaxPixelY; pixelY += stepY)
    {
        if (!HZBCoverageSampleOccludes(mipWidth, mipHeight, mipMaxPixelX, pixelY, mipLevel, nearestOccluderDepth))
            return false;
    }
    if (!HZBCoverageSampleOccludes(mipWidth, mipHeight, mipMaxPixelX, mipMaxPixelY, mipLevel, nearestOccluderDepth))
        return false;

    return true;
}

[numthreads(8, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint width = 0u;
	uint height = 0u;
    uint mipCount = 0u;
	u_HZB.GetDimensions(0u, width, height, mipCount);

    const uint primitiveIndex = dispatchThreadId.x;
    if (width == 0u || height == 0u || primitiveIndex >= u_OcclusionPrimitiveCount)
        return;

    const OcclusionPrimitiveInput primitive = u_OcclusionPrimitiveInputs[primitiveIndex];
    const float2 minPixel = min(primitive.screenMin, primitive.screenMax);
    const float2 maxPixel = max(primitive.screenMin, primitive.screenMax);
    const float nearestOccluderDepth = primitive.nearestDepth - kHZBOcclusionDepthBias;
    const bool occluded = IsConservativelyOccludedByHZBCoverage(
        width,
        height,
        minPixel,
        maxPixel,
        mipCount,
        nearestOccluderDepth);
    u_OcclusionPrimitiveResults[primitiveIndex] = occluded ? 1u : 0u;
}
