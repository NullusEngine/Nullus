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
            #include "NullusShaderLibrary/StandardPBRSurface.hlsl"
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

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionWS = TransformObjectToWorld(input.positionOS);
                output.positionCS = TransformWorldToHClip(output.positionWS);
                output.uv = input.uv;
                const NLSTangentFrame tangentFrame = NLSBuildStandardPbrTangentFrame(
                    (float3x3)u_Model,
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
                const float opacity = _OpacityMap.Sample(sampler_OpacityMap, input.uv).r;
                const float4 surface = NLSEvaluateStandardPbrBaseColorAndOpacity(
                    baseSample,
                    _BaseColor,
                    opacity);
                const float3 albedo = surface.rgb;
                const float metallic = saturate(
                    _Metallic * dot(_MetallicMap.Sample(sampler_MetallicMap, input.uv), _MetallicMapChannel));
                const float roughness = saturate(
                    _Roughness * dot(_RoughnessMap.Sample(sampler_RoughnessMap, input.uv), _RoughnessMapChannel));
                const float occlusion = saturate(
                    _AmbientOcclusion * _OcclusionMap.Sample(sampler_OcclusionMap, input.uv).r);
                const float3 emissive =
                    _EmissiveColor.rgb * _EmissiveMap.Sample(sampler_EmissiveMap, input.uv).rgb;
                const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(input.normalWS, float3(0.0f, 0.0f, 1.0f));
                const float3 geometryNormalWS = NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace);
                float3 shadingNormalWS = geometryNormalWS;
            #if defined(_NORMALMAP)
                shadingNormalWS = NLSConstrainShadingNormalToGeometryHemisphere(
                    NLSApplyStandardPbrNormalMap(
                        input.normalWS,
                        input.tangentWS,
                        input.bitangentWS,
                        isFrontFace,
                        _NormalMap.Sample(sampler_NormalMap, input.uv),
                        _NormalScale),
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
            #if defined(_ALPHATEST_ON)
                clip(surface.a - _Cutoff);
            #endif
                // The shared LDR transform preserves highlight hue and softens isolated specular peaks.
                return float4(
                    NLSToneMapACES(lighting + emissive),
                    surface.a);
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
            #include "NullusShaderLibrary/StandardPBRSurface.hlsl"

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

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = TransformObjectToHClip(input.positionOS);
                output.uv = input.uv;
                const NLSTangentFrame tangentFrame = NLSBuildStandardPbrTangentFrame(
                    (float3x3)u_Model,
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
                const float opacity = _OpacityMap.Sample(sampler_OpacityMap, input.uv).r;
                const float4 surface = NLSEvaluateStandardPbrBaseColorAndOpacity(
                    baseSample,
                    _BaseColor,
                    opacity);
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
            #if defined(_ALPHATEST_ON)
                clip(surface.a - _Cutoff);
            #endif

                const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(input.normalWS, float3(0.0f, 0.0f, 1.0f));
                const float3 geometryNormalWS = NLSOrientGeometryNormal(interpolatedGeometryNormalWS, isFrontFace);
                float3 shadingNormalWS = geometryNormalWS;
            #if defined(_NORMALMAP)
                shadingNormalWS = NLSConstrainShadingNormalToGeometryHemisphere(
                    NLSApplyStandardPbrNormalMap(
                        input.normalWS,
                        input.tangentWS,
                        input.bitangentWS,
                        isFrontFace,
                        _NormalMap.Sample(sampler_NormalMap, input.uv),
                        _NormalScale),
                    geometryNormalWS);
            #endif
                const float receiveShadows =
                    (u_ObjectFlags & NLS_OBJECT_FLAG_RECEIVE_SHADOWS) != 0u ? 1.0f : 0.0f;

                NLSPackStandardPbrGBuffer(
                    surface.rgb,
                    geometryNormalWS,
                    shadingNormalWS,
                    metallic,
                    roughness,
                    ao,
                    receiveShadows,
                    output.Albedo,
                    output.Normal,
                    output.Material);
                return output;
            }
            ENDHLSL
        }

        Pass
        {
            Name "DeferredDecal"

            Tags
            {
                "LightMode" = "DeferredDecal"
            }

            Cull Back
            ZWrite Off
            ZTest LessEqual
            Blend SrcAlpha OneMinusSrcAlpha

            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma shader_feature _ALPHATEST_ON

            #include "NullusShaderLibrary/Core.hlsl"
            #define NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP
            #include "CommonTypes.hlsli"
            #undef NLS_COMMON_TYPES_SHADER_LIBRARY_INTEROP
            #include "NullusShaderLibrary/StandardPBRSurface.hlsl"

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
            Texture2D _OpacityMap : register(t5, space2);
            SamplerState sampler_BaseMap : register(s0, space2);
            SamplerState sampler_OpacityMap : register(s5, space2);

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
                const float4 baseSample = _BaseMap.Sample(sampler_BaseMap, input.uv);
                const float opacity = _OpacityMap.Sample(sampler_OpacityMap, input.uv).r;
                const float4 surface = NLSEvaluateStandardPbrBaseColorAndOpacity(
                    baseSample,
                    _BaseColor,
                    opacity);
            #if defined(_ALPHATEST_ON)
                clip(surface.a - _Cutoff);
            #endif
                return surface;
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
