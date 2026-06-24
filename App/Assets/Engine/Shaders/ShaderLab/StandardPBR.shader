Shader "Nullus/StandardPBR"
{
    Properties
    {
        _BaseMap("Base Map", Texture2D) = "white"
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
        _Metallic("Metallic", Range(0, 1)) = 0
        _Roughness("Roughness", Range(0, 1)) = 1
        _AmbientOcclusion("Ambient Occlusion", Range(0, 1)) = 1
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
            #pragma shader_feature _NORMALMAP
            #pragma multi_compile _ MAIN_LIGHT_SHADOWS

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

            cbuffer MaterialProperties : register(b0, space2)
            {
                float4 _BaseColor;
                float _Metallic;
                float _Roughness;
                float _AmbientOcclusion;
                float _NormalScale;
                float4 _EmissiveColor;
                float4 _SpecularColor;
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
                float4 color = _BaseMap.Sample(sampler_BaseMap, input.uv) * _BaseColor;
                float metallic = _Metallic * _MetallicMap.Sample(sampler_MetallicMap, input.uv).r;
                float roughness = _Roughness * _RoughnessMap.Sample(sampler_RoughnessMap, input.uv).r;
                float occlusion = _AmbientOcclusion * _OcclusionMap.Sample(sampler_OcclusionMap, input.uv).r;
                float opacity = _OpacityMap.Sample(sampler_OpacityMap, input.uv).r;
                float3 emissive = _EmissiveColor.rgb * _EmissiveMap.Sample(sampler_EmissiveMap, input.uv).rgb;
                float3 specular = _SpecularColor.rgb * _SpecularMap.Sample(sampler_SpecularMap, input.uv).rgb;
            #if defined(_NORMALMAP)
                float3 tangentNormal = _NormalMap.Sample(sampler_NormalMap, input.uv).xyz * 2.0f - 1.0f;
                color.rgb *= lerp(1.0f, saturate(tangentNormal.z), saturate(_NormalScale));
            #endif
                color.rgb = color.rgb * occlusion * (1.0f + metallic * 0.04f - roughness * 0.02f) + emissive + specular * 0.02f;
                color.a *= opacity;
            #if defined(_ALPHATEST_ON)
                clip(color.a - _Cutoff);
            #endif
                return color;
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
