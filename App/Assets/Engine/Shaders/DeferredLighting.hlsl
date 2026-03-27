cbuffer PassConstants : register(b0, space1)
{
    float4x4 u_InverseViewProjection;
    float3 u_CameraWorldPosition;
    float u_AmbientIntensity;
    float3 u_LightDirection;
    float u_LightIntensity;
    float3 u_LightColor;
    float u_HasSkyboxTexture;
    float3 u_SkyFallbackColor;
    float u_DepthFogFactor;
};

Texture2D u_GBufferAlbedo : register(t0, space2);
Texture2D u_GBufferNormal : register(t1, space2);
Texture2D u_GBufferMaterial : register(t2, space2);
Texture2D u_GBufferDepth : register(t3, space2);
TextureCube u_SkyboxCube : register(t4, space2);
SamplerState u_LinearWrapSampler : register(s0, space2);

struct VSInput
{
    float3 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput
{
    float4 PositionCS : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.PositionCS = float4(input.Position.xy, 0.0f, 1.0f);
    output.TexCoord = input.TexCoord;
    return output;
}

float3 ReconstructFarWorldDirection(float2 texCoord)
{
    const float2 clipXY = texCoord * 2.0f - 1.0f;
    const float4 worldPosition = mul(u_InverseViewProjection, float4(clipXY, 1.0f, 1.0f));
    const float3 world = worldPosition.xyz / max(abs(worldPosition.w), 1e-5f);
    return normalize(world - u_CameraWorldPosition);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float depth01 = u_GBufferDepth.Sample(u_LinearWrapSampler, input.TexCoord).r;
    if (depth01 >= 0.9995f)
    {
        const float3 skyDirection = ReconstructFarWorldDirection(input.TexCoord);
        if (u_HasSkyboxTexture > 0.5f)
            return u_SkyboxCube.Sample(u_LinearWrapSampler, skyDirection);

        return float4(u_SkyFallbackColor, 1.0f);
    }

    const float4 albedo = u_GBufferAlbedo.Sample(u_LinearWrapSampler, input.TexCoord);
    const float3 encodedNormal = u_GBufferNormal.Sample(u_LinearWrapSampler, input.TexCoord).xyz;
    const float3 normalWS = normalize(encodedNormal * 2.0f - 1.0f);
    const float3 materialParams = u_GBufferMaterial.Sample(u_LinearWrapSampler, input.TexCoord).xyz;

    const float metallic = materialParams.x;
    const float roughness = materialParams.y;
    const float ao = materialParams.z;
    const float3 lightDirection = normalize(-u_LightDirection);
    const float ndotl = saturate(dot(normalWS, lightDirection));
    const float3 diffuse = albedo.rgb * u_LightColor * (ndotl * u_LightIntensity);
    const float3 ambient = albedo.rgb * (u_AmbientIntensity * ao);
    const float3 specHint = u_LightColor * metallic * (1.0f - roughness) * (ndotl * 0.12f * u_LightIntensity);
    const float depthVisibility = saturate(1.0f - depth01 * u_DepthFogFactor);
    const float3 litColor = lerp(ambient, diffuse + ambient + specHint, depthVisibility);

    return float4(litColor, albedo.a);
}
