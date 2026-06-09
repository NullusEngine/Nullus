Texture2D<float> u_HZBPreviousMip : register(t0, space1);
RWTexture2D<float> u_HZBOutputMip : register(u1, space1);

float LoadPreviousMipClamped(uint2 pixel, uint width, uint height)
{
    return u_HZBPreviousMip.Load(int3(min(pixel, uint2(width - 1u, height - 1u)), 0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint previousWidth = 0u;
    uint previousHeight = 0u;
    u_HZBPreviousMip.GetDimensions(previousWidth, previousHeight);

    uint outputWidth = 0u;
    uint outputHeight = 0u;
    u_HZBOutputMip.GetDimensions(outputWidth, outputHeight);
    if (previousWidth == 0u ||
        previousHeight == 0u ||
        dispatchThreadId.x >= outputWidth ||
        dispatchThreadId.y >= outputHeight)
    {
        return;
    }

    const bool copyPreviousMip = previousWidth == outputWidth && previousHeight == outputHeight;
    const uint2 sourcePixel = copyPreviousMip ? dispatchThreadId.xy : dispatchThreadId.xy * 2u;
    const float depth00 = LoadPreviousMipClamped(sourcePixel + uint2(0u, 0u), previousWidth, previousHeight);
    const float depth10 = LoadPreviousMipClamped(sourcePixel + uint2(1u, 0u), previousWidth, previousHeight);
    const float depth01 = LoadPreviousMipClamped(sourcePixel + uint2(0u, 1u), previousWidth, previousHeight);
    const float depth11 = LoadPreviousMipClamped(sourcePixel + uint2(1u, 1u), previousWidth, previousHeight);
    u_HZBOutputMip[dispatchThreadId.xy] = max(max(depth00, depth10), max(depth01, depth11));
}
