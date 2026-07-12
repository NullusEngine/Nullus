Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseMap("Base Map", Texture2D) = "white"
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
        _Metallic("Metallic", Range(0, 1)) = 0
        _Roughness("Roughness", Range(0, 1)) = 1
        _AmbientOcclusion("Ambient Occlusion", Range(0, 1)) = 1
        _MetallicMapChannel("Metallic Map Channel", Vector) = (1, 0, 0, 0)
        _RoughnessMapChannel("Roughness Map Channel", Vector) = (1, 0, 0, 0)
        _EmissiveColor("Emissive Color", Color) = (0, 0, 0, 1)
        _SpecularColor("Specular Color", Color) = (0, 0, 0, 1)
        _MetallicMap("Metallic Map", Texture2D) = "white"
        _RoughnessMap("Roughness Map", Texture2D) = "white"
        _OcclusionMap("Occlusion Map", Texture2D) = "white"
        _NormalMap("Normal Map", Texture2D) = "bump"
        _OpacityMap("Opacity Map", Texture2D) = "white"
        _EmissiveMap("Emissive Map", Texture2D) = "black"
        _SpecularMap("Specular Map", Texture2D) = "black"
        _NormalScale("Normal Scale", Float) = 1
        _Cutoff("Alpha Cutoff", Range(0, 1)) = 0.5
    }

    SubShader
    {
        Tags
        {
            "RenderPipeline" = "Nullus"
            "RenderType" = "Opaque"
            "Queue" = "Geometry"
        }

        Pass
        {
            Name "Forward"

            Tags
            {
                "LightMode" = "Forward"
            }

            Cull Back
            ZWrite On
            ZTest LessEqual
            Blend Off

            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma shader_feature _ALPHATEST_ON
            #pragma multi_compile _ _NORMALMAP
            #pragma multi_compile _ MAIN_LIGHT_SHADOWS

            #include "NullusShaderLibrary/Core.hlsl"
            #include "LightGridCommon.hlsli"

            struct Attributes
            {
                float3 positionOS : POSITION;
                float2 uv : TEXCOORD0;
                float3 normalOS : NORMAL;
                float3 tangentOS : TEXCOORD1;
                float3 bitangentOS : TEXCOORD2;
            };

            struct Varyings
            {
                float4 positionCS : SV_POSITION;
                float2 uv : TEXCOORD0;
                float3 positionWS : TEXCOORD1;
                float3 normalWS : TEXCOORD2;
                float3 tangentWS : TEXCOORD3;
                float3 bitangentWS : TEXCOORD4;
            };

            Texture2D _BaseMap : register(t0, space2);
            Texture2D _MetallicMap : register(t1, space2);
            Texture2D _RoughnessMap : register(t2, space2);
            Texture2D _OcclusionMap : register(t3, space2);
            Texture2D _NormalMap : register(t4, space2);
            Texture2D _OpacityMap : register(t5, space2);
            Texture2D _EmissiveMap : register(t6, space2);
            Texture2D _SpecularMap : register(t7, space2);
            SamplerState sampler_BaseMap : register(s0, space2);
            SamplerState sampler_MetallicMap : register(s1, space2);
            SamplerState sampler_RoughnessMap : register(s2, space2);
            SamplerState sampler_OcclusionMap : register(s3, space2);
            SamplerState sampler_NormalMap : register(s4, space2);
            SamplerState sampler_OpacityMap : register(s5, space2);
            SamplerState sampler_EmissiveMap : register(s6, space2);
            SamplerState sampler_SpecularMap : register(s7, space2);
            StructuredBuffer<uint> u_ForwardLocalLightBuffer : register(t0, space1);
            StructuredBuffer<uint> u_NumCulledLightsGrid : register(t1, space1);
            StructuredBuffer<uint> u_CulledLightDataGrid : register(t2, space1);

            cbuffer MaterialProperties : register(b0, space2)
            {
                float4 _BaseColor;
                float _Metallic;
                float _Roughness;
                float _AmbientOcclusion;
                float _NormalScale;
                float4 _MetallicMapChannel;
                float4 _RoughnessMapChannel;
                float4 _EmissiveColor;
                float4 _SpecularColor;
                float _Cutoff;
            };

            struct StandardPbrTangentFrame
            {
                float3 normalWS;
                float3 tangentWS;
                float3 bitangentWS;
            };

            float3 TransformStandardPbrNormal(float3 normalOS)
            {
                const float3x3 model = (float3x3)u_Model;
                const float3 fallback = mul(model, normalOS);
                const float3 row0 = float3(model._11, model._12, model._13);
                const float3 row1 = float3(model._21, model._22, model._23);
                const float3 row2 = float3(model._31, model._32, model._33);
                const float determinant = dot(row0, cross(row1, row2));
                const float3x3 cofactors = float3x3(
                    cross(row1, row2),
                    cross(row2, row0),
                    cross(row0, row1));
                const float orientation = determinant < 0.0f ? -1.0f : 1.0f;
                const float3 transformed = abs(determinant) > NLS_SAFE_EPSILON
                    ? mul(cofactors, normalOS) * orientation
                    : fallback;
                return NLSSafeNormalize(transformed, NLSSafeNormalize(fallback, float3(0.0f, 0.0f, 1.0f)));
            }

            float3 StandardPbrPerpendicular(float3 normalWS)
            {
                const float3 reference = abs(normalWS.z) < 0.999f
                    ? float3(0.0f, 0.0f, 1.0f)
                    : float3(0.0f, 1.0f, 0.0f);
                return NLSSafeNormalize(cross(reference, normalWS), float3(1.0f, 0.0f, 0.0f));
            }

            StandardPbrTangentFrame BuildStandardPbrTangentFrame(
                float3 normalOS,
                float3 tangentOS,
                float3 bitangentOS)
            {
                StandardPbrTangentFrame frame;
                frame.normalWS = TransformStandardPbrNormal(normalOS);

                const float3x3 model = (float3x3)u_Model;
                const float3 transformedTangent = mul(model, tangentOS);
                const float3 tangentCandidate = transformedTangent -
                    frame.normalWS * dot(transformedTangent, frame.normalWS);
                frame.tangentWS = NLSSafeNormalize(
                    tangentCandidate,
                    StandardPbrPerpendicular(frame.normalWS));

                const float3 transformedBitangent = mul(model, bitangentOS);
                const float3 bitangentCandidate = transformedBitangent -
                    frame.normalWS * dot(transformedBitangent, frame.normalWS) -
                    frame.tangentWS * dot(transformedBitangent, frame.tangentWS);
                frame.bitangentWS = NLSSafeNormalize(
                    bitangentCandidate,
                    NLSSafeNormalize(
                        cross(frame.normalWS, frame.tangentWS),
                        float3(0.0f, 1.0f, 0.0f)));
                return frame;
            }

            float3 DecodeStandardPbrNormal(float4 normalSample)
            {
                const float2 xy = normalSample.xy * 2.0f - 1.0f;
                const float rgbZ = normalSample.z * 2.0f - 1.0f;
                const float reconstructedZ = sqrt(saturate(1.0f - dot(xy, xy)));
                const float useRgbZ = step(0.0039f, normalSample.z);
                const float3 decoded = float3(xy * _NormalScale, lerp(reconstructedZ, rgbZ, useRgbZ));
                return NLSSafeNormalize(decoded, float3(0.0f, 0.0f, 1.0f));
            }

            float3 ComputeStandardPbrNormal(Varyings input)
            {
                const float3 geometryNormal = NLSSafeNormalize(input.normalWS, float3(0.0f, 0.0f, 1.0f));
            #if defined(_NORMALMAP)
                const float3 tangentNormal = DecodeStandardPbrNormal(
                    _NormalMap.Sample(sampler_NormalMap, input.uv));
                const float3x3 tangentToWorld = float3x3(
                    NLSSafeNormalize(input.tangentWS, StandardPbrPerpendicular(geometryNormal)),
                    NLSSafeNormalize(input.bitangentWS, cross(geometryNormal, input.tangentWS)),
                    geometryNormal);
                return NLSSafeNormalize(mul(tangentNormal, tangentToWorld), geometryNormal);
            #else
                return geometryNormal;
            #endif
            }

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionWS = TransformObjectToWorld(input.positionOS);
                output.positionCS = TransformWorldToHClip(output.positionWS);
                output.uv = input.uv;
                const StandardPbrTangentFrame tangentFrame = BuildStandardPbrTangentFrame(
                    input.normalOS,
                    input.tangentOS,
                    input.bitangentOS);
                output.normalWS = tangentFrame.normalWS;
                output.tangentWS = tangentFrame.tangentWS;
                output.bitangentWS = tangentFrame.bitangentWS;
                return output;
            }

            float4 PSMain(Varyings input) : SV_Target0
            {
                const float4 baseSample = _BaseMap.Sample(sampler_BaseMap, input.uv);
                const float3 albedo = baseSample.rgb * _BaseColor.rgb;
                const float metallic = saturate(
                    _Metallic * dot(_MetallicMap.Sample(sampler_MetallicMap, input.uv), _MetallicMapChannel));
                const float roughness = saturate(
                    _Roughness * dot(_RoughnessMap.Sample(sampler_RoughnessMap, input.uv), _RoughnessMapChannel));
                const float occlusion = saturate(
                    _AmbientOcclusion * _OcclusionMap.Sample(sampler_OcclusionMap, input.uv).r);
                const float opacity = _OpacityMap.Sample(sampler_OpacityMap, input.uv).r;
                const float3 emissive =
                    _EmissiveColor.rgb * _EmissiveMap.Sample(sampler_EmissiveMap, input.uv).rgb;
                const float3 normalWS = ComputeStandardPbrNormal(input);
                const float3 lighting = NLSAccumulateClusteredLightingPBR(
                    u_ForwardLocalLightBuffer,
                    u_NumCulledLightsGrid,
                    u_CulledLightDataGrid,
                    input.positionWS,
                    normalWS,
                    albedo,
                    metallic,
                    roughness,
                    occlusion);
                const float alpha = _BaseColor.a * baseSample.a * opacity;
            #if defined(_ALPHATEST_ON)
                clip(alpha - _Cutoff);
            #endif
                return float4(
                    lighting + emissive,
                    alpha);
            }
            ENDHLSL
        }

        Pass
        {
            Name "DepthOnly"

            Tags
            {
                "LightMode" = "DepthOnly"
            }

            Cull Back
            ZWrite On
            ZTest LessEqual
            Blend Off

            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma shader_feature _ALPHATEST_ON

            #include "NullusShaderLibrary/Core.hlsl"

            struct Attributes
            {
                float3 positionOS : POSITION;
                float2 uv : TEXCOORD0;
            };

            struct Varyings
            {
                float4 positionCS : SV_POSITION;
                float2 uv : TEXCOORD0;
            };

            Texture2D _BaseMap : register(t0, space2);
            SamplerState sampler_BaseMap : register(s0, space2);

            cbuffer MaterialProperties : register(b0, space2)
            {
                float4 _BaseColor;
                float _Cutoff;
            };

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = TransformObjectToHClip(input.positionOS);
                output.uv = input.uv;
                return output;
            }

            float4 PSMain(Varyings input) : SV_Target0
            {
            #if defined(_ALPHATEST_ON)
                float4 color = _BaseMap.Sample(sampler_BaseMap, input.uv) * _BaseColor;
                clip(color.a - _Cutoff);
            #endif
                return 0;
            }
            ENDHLSL
        }
    }
}
