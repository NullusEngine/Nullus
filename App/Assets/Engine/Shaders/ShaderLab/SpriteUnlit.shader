Shader "Nullus/SpriteUnlit"
{
    Properties { _BaseColor("Base Color", Color) = (1, 1, 1, 1) }
    SubShader
    {
        Tags { "RenderPipeline" = "Nullus" "RenderType" = "Transparent" "Queue" = "Transparent" }
        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" "ShadingModel" = "Unlit" }
            Cull Off ZWrite Off ZTest LessEqual Blend SrcAlpha OneMinusSrcAlpha
            HLSLPROGRAM
            #pragma vertex SpriteVS
            #pragma fragment SpritePS
            #include "NullusShaderLibrary/SpriteUnlit.hlsl"
            ENDHLSL
        }
    }
}
