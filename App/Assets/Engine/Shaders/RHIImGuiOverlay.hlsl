struct VSInput
{
#if defined(NLS_SPIRV)
    [[vk::location(0)]]
#endif
    float2 Position : POSITION0;
#if defined(NLS_SPIRV)
    [[vk::location(1)]]
#endif
    float2 UV : TEXCOORD0;
#if defined(NLS_SPIRV)
    [[vk::location(3)]]
#endif
    float4 Color : TEXCOORD1;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float4 Color : TEXCOORD1;
};

#if defined(NLS_SPIRV)
struct OverlayProjection
{
    float2 ProjectionScale;
    float2 ProjectionTranslate;
};

[[vk::push_constant]] ConstantBuffer<OverlayProjection> g_OverlayProjection;
#else
cbuffer OverlayProjection : register(b0, space0)
{
    float2 ProjectionScale;
    float2 ProjectionTranslate;
};
#endif

Texture2D FontAtlasTexture : register(t0, space0);
SamplerState FontAtlasSampler : register(s1, space0);

VSOutput VSMain(VSInput input)
{
    VSOutput output;
#if defined(NLS_SPIRV)
    output.Position = float4(
        input.Position * g_OverlayProjection.ProjectionScale +
        g_OverlayProjection.ProjectionTranslate,
        0.0f,
        1.0f);
#else
    output.Position = float4(input.Position * ProjectionScale + ProjectionTranslate, 0.0f, 1.0f);
#endif
    output.UV = input.UV;
    output.Color = input.Color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return input.Color * FontAtlasTexture.Sample(FontAtlasSampler, input.UV);
}
