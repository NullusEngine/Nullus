Texture2D<float> u_HZB : register(t0, space1);

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

static const float kHZBOcclusionDepthBias = 0.0005f;
static const uint kHZBOcclusionMaxMip0ScanPixels = 64u;

float LoadHZB(uint2 pixel)
{
    return u_HZB.Load(int3(pixel, 0));
}

bool HZBSampleOccludes(uint2 pixel, float nearestOccluderDepth)
{
    return LoadHZB(pixel) < nearestOccluderDepth;
}

bool IsConservativelyOccludedByHZBMip0Coverage(
    uint width,
    uint height,
    float2 minPixel,
    float2 maxPixel,
    float nearestOccluderDepth)
{
    const float2 clampedMin = clamp(minPixel, float2(0.0f, 0.0f), float2((float)width - 1.0f, (float)height - 1.0f));
    const float2 clampedMax = clamp(maxPixel, float2(0.0f, 0.0f), float2((float)width - 1.0f, (float)height - 1.0f));
    const uint minPixelX = (uint)floor(min(clampedMin.x, clampedMax.x));
    const uint minPixelY = (uint)floor(min(clampedMin.y, clampedMax.y));
    const uint maxPixelX = (uint)ceil(max(clampedMin.x, clampedMax.x));
    const uint maxPixelY = (uint)ceil(max(clampedMin.y, clampedMax.y));
    const uint coveredPixelCount = (maxPixelX - minPixelX + 1u) * (maxPixelY - minPixelY + 1u);
    if (coveredPixelCount == 0u || coveredPixelCount > kHZBOcclusionMaxMip0ScanPixels)
        return false;

    for (uint pixelY = minPixelY; pixelY <= maxPixelY; ++pixelY)
    {
        for (uint pixelX = minPixelX; pixelX <= maxPixelX; ++pixelX)
        {
            if (!HZBSampleOccludes(uint2(pixelX, pixelY), nearestOccluderDepth))
                return false;
        }
    }

    return true;
}

[numthreads(8, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint width = 0u;
	uint height = 0u;
	u_HZB.GetDimensions(width, height);

	uint primitiveCount = 0u;
	uint primitiveStride = 0u;
    u_OcclusionPrimitiveInputs.GetDimensions(primitiveCount, primitiveStride);

    const uint primitiveIndex = dispatchThreadId.x;
    if (width == 0u || height == 0u || primitiveIndex >= primitiveCount)
        return;

    const OcclusionPrimitiveInput primitive = u_OcclusionPrimitiveInputs[primitiveIndex];
    const float2 minPixel = min(primitive.screenMin, primitive.screenMax);
    const float2 maxPixel = max(primitive.screenMin, primitive.screenMax);
    const float nearestOccluderDepth = primitive.nearestDepth - kHZBOcclusionDepthBias;
    const bool occluded = IsConservativelyOccludedByHZBMip0Coverage(
        width,
        height,
        minPixel,
        maxPixel,
        nearestOccluderDepth);
    u_OcclusionPrimitiveResults[primitiveIndex] = occluded ? 1u : 0u;
}
