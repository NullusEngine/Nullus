#ifndef NLS_SCENE_DEBUG_INCLUDED
#define NLS_SCENE_DEBUG_INCLUDED
// Per-view values live on a capture-owned material copy, never on an asset.
cbuffer SceneDebugConstants : register(b1, space2)
{
    float4 u_SceneDebugOptions; // mode, forward path, PBR surface, main texture
    float4 u_SceneAlbedoRange;
    float4 u_SceneSpecularRange;
    float4 u_SceneWireColor;
};

float4 NLSSceneSrgbToLinear(float4 value)
{
    value /= 255.0f;
    float4 low = value / 12.92f, high = pow((value + 0.055f) / 1.055f, 2.4f);
    return float4(value.x <= 0.04045f ? low.x : high.x, value.y <= 0.04045f ? low.y : high.y,
        value.z <= 0.04045f ? low.z : high.z, value.w <= 0.04045f ? low.w : high.w);
}

float NLSSceneTextureLod(float2 uv, float2 dimensions)
{
    float2 dx = ddx(uv) * dimensions, dy = ddy(uv) * dimensions;
    return 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12f));
}

float4 NLSSceneDebug(float3 baseColor, float metallic, float opacity, float lod)
{
    const uint mode = (uint)u_SceneDebugOptions.x;
    const float3 unavailable = float3(0.35f, 0.35f, 0.35f);
    if (mode == 11) return u_SceneWireColor;
    if (u_SceneDebugOptions.z < -0.5f && mode != 5)
    {
        if (mode == 6) return float4(-65504, -65504, -65504, 1);
        if (mode == 7) return float4(0, 0, 0, -65504);
        return float4(unavailable, 1);
    }
    if (mode >= 1 && mode <= 4) return float4(unavailable, 1);
    if (mode == 5) return float4(u_SceneDebugOptions.y > 0.5f ? float3(1, 0.85f, 0) : float3(0.12f, 0.28f, 1), 1);
    if (mode == 6) return float4(0, 0, 0, ((uint)u_SceneDebugOptions.y & 2u) != 0 ? saturate(opacity) : 1);
    if (mode == 7) return 1;
    if (mode == 8)
    {
        if (u_SceneDebugOptions.w < 0.5f || !isfinite(lod) || abs(lod) > 10000) return float4(unavailable, 1);
        float3 color = float3(0, 0.65f, 0);
        if (lod > 1) color = lerp(color, float3(1, 0, 0), saturate((lod - 1) / 3));
        if (lod < -1) color = lerp(color, float3(0, 0.2f, 1), saturate((-lod - 1) / 3));
        return float4(color, 1);
    }
    if (u_SceneDebugOptions.z < 0.5f) return float4(unavailable, 1);
    float3 value = mode == 10 ? lerp(0.04f.xxx, baseColor, saturate(metallic)) : baseColor;
    float4 range = NLSSceneSrgbToLinear(mode == 10 ? u_SceneSpecularRange : u_SceneAlbedoRange);
    float minimum = lerp(range.x, range.z, saturate(metallic));
    float maximum = lerp(range.y, range.w, saturate(metallic));
    float luminance = dot(value, float3(0.2126f, 0.7152f, 0.0722f));
    if (!all(isfinite(value)) || !isfinite(metallic)) return float4(1, 0, 1, 1);
    if (luminance < minimum) return float4(1, 0, 0, 1);
    if (luminance > maximum) return float4(0, 0.2f, 1, 1);
    return float4(lerp(luminance.xxx, baseColor, 0.15f), 1);
}
#endif
