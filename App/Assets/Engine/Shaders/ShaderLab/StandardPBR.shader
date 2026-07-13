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
            #define NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP
            #include "CommonTypes.hlsli"
            #undef NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP
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

            NLSTangentFrame BuildStandardPbrTangentFrame(
                float3 normalOS,
                float3 tangentOS,
                float3 bitangentOS)
            {
                const float3x3 model = (float3x3)u_Model;
                return NLSBuildSafeTangentFrame(
                    TransformStandardPbrNormal(normalOS),
                    mul(model, tangentOS),
                    mul(model, bitangentOS));
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

            float3 ComputeStandardPbrNormal(Varyings input, bool isFrontFace)
            {
            #if !defined(_NORMALMAP)
                const float faceSign = isFrontFace ? 1.0f : -1.0f;
                const float3 normalWS = NLSSafeNormalize(input.normalWS, float3(0.0f, 0.0f, 1.0f));
                return normalWS * faceSign;
            #else
                NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(
                    input.normalWS,
                    input.tangentWS,
                    input.bitangentWS);
                tangentFrame = NLSOrientTangentFrameForFace(tangentFrame, isFrontFace);
                const float3 tangentNormal = DecodeStandardPbrNormal(
                    _NormalMap.Sample(sampler_NormalMap, input.uv));
                return NLSApplyTangentNormal(tangentNormal, tangentFrame);
            #endif
            }

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionWS = TransformObjectToWorld(input.positionOS);
                output.positionCS = TransformWorldToHClip(output.positionWS);
                output.uv = input.uv;
                const NLSTangentFrame tangentFrame = BuildStandardPbrTangentFrame(
                    input.normalOS,
                    input.tangentOS,
                    input.bitangentOS);
                output.normalWS = tangentFrame.normalWS;
                output.tangentWS = tangentFrame.tangentWS;
                output.bitangentWS = tangentFrame.bitangentWS;
                return output;
            }

            float4 PSMain(Varyings input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
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
                const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(input.normalWS, float3(0.0f, 0.0f, 1.0f));
                const float3 geometryNormalWS = NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace);
                float3 shadingNormalWS = geometryNormalWS;
            #if defined(_NORMALMAP)
                shadingNormalWS = NLSConstrainShadingNormalToGeometryHemisphere(
                    ComputeStandardPbrNormal(input, isFrontFace),
                    geometryNormalWS);
            #endif
                const float3 lighting = NLSAccumulateClusteredLightingPBR(
                    u_ForwardLocalLightBuffer,
                    u_NumCulledLightsGrid,
                    u_CulledLightDataGrid,
                    input.positionWS,
                    geometryNormalWS,
                    shadingNormalWS,
                    albedo,
                    metallic,
                    roughness,
                    occlusion);
                const float alpha = _BaseColor.a * baseSample.a * opacity;
            #if defined(_ALPHATEST_ON)
                clip(alpha - _Cutoff);
            #endif
                // The shared LDR transform preserves highlight hue and softens isolated specular peaks.
                return float4(
                    NLSToneMapACES(lighting + emissive),
                    alpha);
            }
            ENDHLSL
        }

        Pass
        {
            Name "GBuffer"

            Tags
            {
                "LightMode" = "GBuffer"
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

            #include "NullusShaderLibrary/Core.hlsl"
            #define NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP
            #include "CommonTypes.hlsli"
            #undef NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP
            #include "NullusShaderLibrary/Instancing.hlsl"
            #include "NullusShaderLibrary/PBRNormals.hlsl"

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
                float3 normalWS : TEXCOORD1;
                float3 tangentWS : TEXCOORD2;
                float3 bitangentWS : TEXCOORD3;
            };

            struct GBufferOutput
            {
                float4 Albedo : SV_Target0;
                float4 Normal : SV_Target1;
                float4 Material : SV_Target2;
            };

            Texture2D _BaseMap : register(t0, space2);
            Texture2D _MetallicMap : register(t1, space2);
            Texture2D _RoughnessMap : register(t2, space2);
            Texture2D _OcclusionMap : register(t3, space2);
            Texture2D _NormalMap : register(t4, space2);
            Texture2D _OpacityMap : register(t5, space2);
            SamplerState sampler_BaseMap : register(s0, space2);
            SamplerState sampler_MetallicMap : register(s1, space2);
            SamplerState sampler_RoughnessMap : register(s2, space2);
            SamplerState sampler_OcclusionMap : register(s3, space2);
            SamplerState sampler_NormalMap : register(s4, space2);
            SamplerState sampler_OpacityMap : register(s5, space2);

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

            float3 TransformStandardPbrGBufferNormal(float3 normalOS)
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
                return NLSSafeNormalize(
                    transformed,
                    NLSSafeNormalize(fallback, float3(0.0f, 0.0f, 1.0f)));
            }

            NLSTangentFrame BuildStandardPbrGBufferTangentFrame(
                float3 normalOS,
                float3 tangentOS,
                float3 bitangentOS)
            {
                const float3x3 model = (float3x3)u_Model;
                return NLSBuildSafeTangentFrame(
                    TransformStandardPbrGBufferNormal(normalOS),
                    mul(model, tangentOS),
                    mul(model, bitangentOS));
            }

            float3 DecodeStandardPbrGBufferNormal(float4 normalSample)
            {
                const float2 xy = normalSample.xy * 2.0f - 1.0f;
                const float rgbZ = normalSample.z * 2.0f - 1.0f;
                const float reconstructedZ = sqrt(saturate(1.0f - dot(xy, xy)));
                const float useRgbZ = step(0.0039f, normalSample.z);
                const float3 decoded = float3(
                    xy * _NormalScale,
                    lerp(reconstructedZ, rgbZ, useRgbZ));
                return NLSSafeNormalize(decoded, float3(0.0f, 0.0f, 1.0f));
            }

            float3 ComputeStandardPbrGBufferShadingNormal(
                Varyings input,
                bool isFrontFace)
            {
                NLSTangentFrame tangentFrame = NLSBuildSafeTangentFrame(
                    input.normalWS,
                    input.tangentWS,
                    input.bitangentWS);
                tangentFrame = NLSOrientTangentFrameForFace(tangentFrame, isFrontFace);
                const float3 tangentNormal = DecodeStandardPbrGBufferNormal(
                    _NormalMap.Sample(sampler_NormalMap, input.uv));
                return NLSApplyTangentNormal(tangentNormal, tangentFrame);
            }

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = TransformObjectToHClip(input.positionOS);
                output.uv = input.uv;
                const NLSTangentFrame tangentFrame = BuildStandardPbrGBufferTangentFrame(
                    input.normalOS,
                    input.tangentOS,
                    input.bitangentOS);
                output.normalWS = tangentFrame.normalWS;
                output.tangentWS = tangentFrame.tangentWS;
                output.bitangentWS = tangentFrame.bitangentWS;
                return output;
            }

            GBufferOutput PSMain(Varyings input, bool isFrontFace : SV_IsFrontFace)
            {
                GBufferOutput output;
                const float4 baseSample = _BaseMap.Sample(sampler_BaseMap, input.uv);
                const float3 albedo = baseSample.rgb * _BaseColor.rgb;
                const float metallic = saturate(
                    _Metallic * dot(
                        _MetallicMap.Sample(sampler_MetallicMap, input.uv),
                        _MetallicMapChannel));
                const float roughness = saturate(
                    _Roughness * dot(
                        _RoughnessMap.Sample(sampler_RoughnessMap, input.uv),
                        _RoughnessMapChannel));
                const float ao = saturate(
                    _AmbientOcclusion * _OcclusionMap.Sample(sampler_OcclusionMap, input.uv).r);
                const float opacity = _OpacityMap.Sample(sampler_OpacityMap, input.uv).r;
                const float surfaceAlpha = _BaseColor.a * baseSample.a * opacity;
            #if defined(_ALPHATEST_ON)
                clip(surfaceAlpha - _Cutoff);
            #endif

                const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(input.normalWS, float3(0.0f, 0.0f, 1.0f));
                const float3 geometryNormalWS = NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace);
                float3 shadingNormalWS = geometryNormalWS;
            #if defined(_NORMALMAP)
                shadingNormalWS = NLSConstrainShadingNormalToGeometryHemisphere(
                    ComputeStandardPbrGBufferShadingNormal(input, isFrontFace),
                    geometryNormalWS);
            #endif
                const float2 geometryNormalOct = NLSOctEncodeNormal(geometryNormalWS);
                const float receiveShadows =
                    (u_ObjectFlags & NLS_OBJECT_FLAG_RECEIVE_SHADOWS) != 0u ? 1.0f : 0.0f;

                output.Albedo = float4(albedo, geometryNormalOct.x);
                output.Normal = float4(shadingNormalWS * 0.5f + 0.5f, geometryNormalOct.y);
                output.Material = float4(metallic, roughness, ao, receiveShadows);
                return output;
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
