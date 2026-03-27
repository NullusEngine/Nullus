#ifndef NLS_ENGINE_COMMON_TYPES_HLSLI
#define NLS_ENGINE_COMMON_TYPES_HLSLI

struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float3 Normal : NORMAL;
    float3 Tangent : TEXCOORD1;
    float3 Bitangent : TEXCOORD2;
};

struct VSOutput
{
    float4 PositionCS : SV_Position;
    float3 PositionWS : TEXCOORD0;
    float3 NormalWS : TEXCOORD1;
    float3 TangentWS : TEXCOORD2;
    float3 BitangentWS : TEXCOORD3;
    float2 TexCoord : TEXCOORD4;
};

#endif
