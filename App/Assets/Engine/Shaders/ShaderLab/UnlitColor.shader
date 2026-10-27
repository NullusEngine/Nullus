Shader "Nullus/ShaderLab/UnlitColor"
{
    Properties
    {
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
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
            Tags { "LightMode" = "Forward" "ShadingModel" = "Unlit" "MaterialRenderPath" = "ForwardOpaque" }
            Cull Back
            ZWrite On
            ZTest LessEqual
            Blend Off

            HLSLPROGRAM
            #pragma multi_compile _ NLS_SKINNED
#pragma vertex VSMain
            #pragma fragment PSMain
            #include "NullusShaderLibrary/Core.hlsl"
#include "NullusShaderLibrary/Skinning.hlsl"

            struct Attributes
            {
                float3 positionOS : POSITION;
    NLS_SKIN_ATTRIBUTES
};

            struct Varyings
            {
                float4 positionCS : SV_POSITION;
            };

            cbuffer MaterialProperties : register(b0, space2)
            {
                float4 _BaseColor;
            };

            Varyings VSMain(Attributes input)
            {
                Varyings output;
                output.positionCS = TransformObjectToHClip(NLS_SKIN_POSITION(input));
                return output;
            }

            float4 PSMain(Varyings input) : SV_Target0
            {
                return _BaseColor;
            }
            ENDHLSL
        }
        Pass
        {
            Name "DepthOnly"
            Tags { "LightMode" = "DepthOnly" }
            Cull Back ZWrite On ZTest LessEqual Blend Off
            HLSLPROGRAM
            #pragma multi_compile _ NLS_SKINNED
#pragma vertex VSMain
            #pragma fragment PSMain
            #include "NullusShaderLibrary/Core.hlsl"
#include "NullusShaderLibrary/Skinning.hlsl"
            struct Attributes {  float3 positionOS : POSITION;
    NLS_SKIN_ATTRIBUTES
};
float4 VSMain(Attributes input) : SV_POSITION { return TransformObjectToHClip(NLS_SKIN_POSITION(input)); }
            void PSMain() {}
            ENDHLSL
        }
    }
}
