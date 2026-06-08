Texture2D<float> u_OpaqueDepth : register(t0, space1);
RWTexture2D<float> u_HZBOutput : register(u1, space1);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width = 0u;
    uint height = 0u;
    u_HZBOutput.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    const float depth = u_OpaqueDepth.Load(int3(dispatchThreadId.xy, 0));
    u_HZBOutput[dispatchThreadId.xy] = depth;
}
