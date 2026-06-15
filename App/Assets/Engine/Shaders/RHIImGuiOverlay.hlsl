struct VSInput
{
    float2 Position : POSITION0;
    float2 UV : TEXCOORD0;
    float4 Color : TEXCOORD1;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float4 Color : TEXCOORD1;
};

cbuffer OverlayProjection : register(b0, space0)
{
    float2 ProjectionScale;
    float2 ProjectionTranslate;
};

Texture2D FontAtlasTexture : register(t0, space0);
SamplerState FontAtlasSampler : register(s1, space0);

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.Position = float4(input.Position * ProjectionScale + ProjectionTranslate, 0.0f, 1.0f);
    output.UV = input.UV;
    output.Color = input.Color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return input.Color * FontAtlasTexture.Sample(FontAtlasSampler, input.UV);
}
