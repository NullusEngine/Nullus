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
    int u_IsBall;
    int u_IsPickable;
    int u_HighlightedAxis;
    float u_Padding0;
};

struct GizmoVSOutput
{
    float4 PositionCS : SV_Position;
    float3 Color : TEXCOORD0;
};

float4x4 RotationMatrix(float3 axis, float angle)
{
    axis = normalize(axis);
    const float s = sin(angle);
    const float c = cos(angle);
    const float oc = 1.0f - c;

    return float4x4(
        oc * axis.x * axis.x + c,           oc * axis.x * axis.y - axis.z * s,  oc * axis.z * axis.x + axis.y * s,  0.0f,
        oc * axis.x * axis.y + axis.z * s,  oc * axis.y * axis.y + c,           oc * axis.y * axis.z - axis.x * s,  0.0f,
        oc * axis.z * axis.x - axis.y * s,  oc * axis.y * axis.z + axis.x * s,  oc * axis.z * axis.z + c,           0.0f,
        0.0f,                                0.0f,                                0.0f,                                1.0f);
}

GizmoVSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    GizmoVSOutput output;

    float4x4 instanceModel = u_Model;
    if (instanceId == 1)
        instanceModel = mul(instanceModel, RotationMatrix(float3(0.0f, 1.0f, 0.0f), radians(-90.0f)));
    else if (instanceId == 2)
        instanceModel = mul(instanceModel, RotationMatrix(float3(1.0f, 0.0f, 0.0f), radians(-90.0f)));

    const float3 gizmoCenter = mul(instanceModel, float4(0.0f, 0.0f, 0.0f, 1.0f)).xyz;
    const float distanceToCamera = distance(u_CameraWorldPos, gizmoCenter);
    const float3 worldPosition = mul(instanceModel, float4(input.Position * distanceToCamera * 0.1f, 1.0f)).xyz;
    output.PositionCS = mul(u_ViewProjection, float4(worldPosition, 1.0f));

    if (u_IsPickable != 0)
    {
        int blueComponent = 254;
        if (instanceId == 1)
            blueComponent = 252;
        else if (instanceId == 2)
            blueComponent = 253;

        output.Color = float3(1.0f, 1.0f, blueComponent / 255.0f);
        return output;
    }

    if (u_IsBall != 0)
    {
        output.Color = float3(1.0f, 1.0f, 1.0f);
        return output;
    }

    const bool isHighlighted =
        (instanceId == 1 && u_HighlightedAxis == 0) ||
        (instanceId == 2 && u_HighlightedAxis == 1) ||
        (instanceId == 0 && u_HighlightedAxis == 2);

    output.Color = isHighlighted
        ? float3(1.0f, 1.0f, 0.0f)
        : float3(instanceId == 1 ? 1.0f : 0.0f, instanceId == 2 ? 1.0f : 0.0f, instanceId == 0 ? 1.0f : 0.0f);
    return output;
}

float4 PSMain(GizmoVSOutput input) : SV_Target0
{
    return float4(input.Color, 1.0f);
}
