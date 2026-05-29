#include "SelectionOutlineMaskChannels.hlsli"
#include "../../Engine/Shaders/CommonTypes.hlsli"

Texture2D u_SelectionOutlineMask : register(t0, space2);
Texture2D u_MainTexture : register(t1, space2);
SamplerState u_LinearClampSampler : register(s0, space2);

cbuffer FrameConstants : register(b0, space0)
{
    float4x4 u_ViewProjection;
    float3 u_CameraWorldPos;
    float u_Time;
    float4x4 u_ViewProjectionNoTranslation;
};

StructuredBuffer<float4x4> ObjectData : register(t0, space3);

cbuffer MaterialConstants : register(b0, space2)
{
    float4 u_OutlineColor;
    float4 u_ChildOutlineColor;
    float4 u_TexelSize;
    float u_ObjectId;
    float u_SelectionClassification;
    float u_AlphaCutoff;
    float u_AlphaClip;
    int u_SelectionOutlinePassMode;
};

#define NLS_SELECTION_OUTLINE_MASK_PASS_MODE(name, value) static const int SelectionOutlinePassMode##name = value;
#include "SelectionOutlineMaskPassModes.def"
#undef NLS_SELECTION_OUTLINE_MASK_PASS_MODE

VSOutput BuildObjectMaskVertex(VSInput input, uint instanceId)
{
    VSOutput output;
    const float4x4 model = ObjectData[u_ObjectIndex + instanceId];
    const float4 worldPosition = mul(model, float4(input.Position, 1.0f));
    output.PositionCS = mul(u_ViewProjection, worldPosition);
    output.PositionWS = worldPosition.xyz;
    output.NormalWS = normalize(mul((float3x3)model, input.Normal));
    output.TangentWS = normalize(mul((float3x3)model, input.Tangent));
    output.BitangentWS = normalize(mul((float3x3)model, input.Bitangent));
    output.TexCoord = input.TexCoord;
    return output;
}

VSOutput BuildFullscreenVertex(VSInput input)
{
    VSOutput output;
    output.PositionCS = float4(input.Position.xy, 0.0f, 1.0f);
    output.PositionWS = float3(input.Position.xy, 0.0f);
    output.NormalWS = float3(0.0f, 0.0f, 1.0f);
    output.TangentWS = float3(1.0f, 0.0f, 0.0f);
    output.BitangentWS = float3(0.0f, 1.0f, 0.0f);
    output.TexCoord = input.TexCoord;
    return output;
}

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    if (u_SelectionOutlinePassMode == SelectionOutlinePassModeCaptureVisible ||
        u_SelectionOutlinePassMode == SelectionOutlinePassModeCaptureOccluded)
    {
        return BuildObjectMaskVertex(input, instanceId);
    }

    return BuildFullscreenVertex(input);
}

float4 CaptureMaskVisible(VSOutput input) : SV_Target0
{
    if (u_AlphaClip > 0.5f)
    {
        const float alpha = u_MainTexture.Sample(u_LinearClampSampler, input.TexCoord).a;
        clip(alpha - u_AlphaCutoff);
    }

    float4 mask = 0.0f;
    SelectionOutlineMaskSetGroupId(mask, u_ObjectId);
    SelectionOutlineMaskSetVisible(mask, 1.0f);
    SelectionOutlineMaskSetSelected(mask, 1.0f);
    SelectionOutlineMaskSetClassification(mask, u_SelectionClassification);
    return mask;
}

float4 CaptureMaskOccluded(VSOutput input) : SV_Target0
{
    if (u_AlphaClip > 0.5f)
    {
        const float alpha = u_MainTexture.Sample(u_LinearClampSampler, input.TexCoord).a;
        clip(alpha - u_AlphaCutoff);
    }

    float4 mask = 0.0f;
    SelectionOutlineMaskSetGroupId(mask, u_ObjectId);
    SelectionOutlineMaskSetSelected(mask, 1.0f);
    SelectionOutlineMaskSetClassification(mask, u_SelectionClassification);
    return mask;
}

#include "SelectionOutlineCompositeCore.hlsli"

float4 PSMain(VSOutput input) : SV_Target0
{
    if (u_SelectionOutlinePassMode == SelectionOutlinePassModeCaptureVisible)
        return CaptureMaskVisible(input);
    if (u_SelectionOutlinePassMode == SelectionOutlinePassModeCaptureOccluded)
        return CaptureMaskOccluded(input);
    return Composite(input);
}
