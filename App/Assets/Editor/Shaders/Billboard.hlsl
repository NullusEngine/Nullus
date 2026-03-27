#include "../../Engine/Shaders/CommonTypes.hlsli"

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};

cbuffer ObjectConstants : register(b0, space3)
{
    float4x4 u_Model;
};

cbuffer MaterialConstants : register(b0, space2)
{
    float4 u_Diffuse;
    float2 u_TextureTiling;
    float2 u_TextureOffset;
    float3 u_Padding0;
    float u_Scale;
};

Texture2D u_DiffuseMap : register(t0, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);

struct BillboardVSOutput
{
    float4 PositionCS : SV_Position;
    float2 TexCoords : TEXCOORD0;
};

BillboardVSOutput VSMain(VSInput input)
{
    BillboardVSOutput output;
    output.TexCoords = input.TexCoord;

    const float3 billboardCenter = mul(u_Model, float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz;
    const float distanceToCamera = distance(u_CameraWorldPos, billboardCenter);
    const float iconScale = distanceToCamera * u_Scale;

    float3 forward = normalize(u_CameraWorldPos - billboardCenter);
    float3 worldUp = float3(0.0f, 1.0f, 0.0f);
    float3 right = cross(worldUp, forward);

    if (dot(right, right) < 1e-5f)
    {
        worldUp = float3(0.0f, 0.0f, 1.0f);
        right = cross(worldUp, forward);
    }

    right = normalize(right);
    const float3 up = normalize(cross(forward, right));

    const float3 localOffset = input.Position * iconScale;
    const float3 worldPosition =
        billboardCenter +
        right * localOffset.x +
        up * localOffset.y +
        forward * localOffset.z;

    output.PositionCS = mul(u_ViewProjection, float4(worldPosition, 1.0f));
    return output;
}

float4 PSMain(BillboardVSOutput input) : SV_Target0
{
    const float2 texCoord = u_TextureOffset + frac(input.TexCoords * u_TextureTiling);
    const float4 color = u_DiffuseMap.Sample(u_LinearWrapSampler, texCoord) * u_Diffuse;
    clip(color.a - 0.01f);
    return color;
}
