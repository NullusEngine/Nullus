Shader "Nullus/VFX/Dynamic Sprite"
{
    Properties
    {
        _Tint("Tint", Color) = (1,1,1,1)
        _Intensity("Intensity", Float) = 1
        _Detail("Detail", Texture2D) = "white"
    }
    SubShader
    {
        Tags { "RenderPipeline" = "Nullus" }
        Pass
        {
            Name "ParticleSprite"
            Tags { "LightMode" = "ParticleSprite" }
            HLSLPROGRAM
            #pragma fragment ParticleMaterialPS
            #include "NullusShaderLibrary/ParticleSprite.hlsl"
            cbuffer ParticleProperties : register(b0,space2)
            {
                float4 _Tint;
                float _Intensity;
            };
            Texture2D<float4> _Detail : register(t0,space2);
            SamplerState sampler_Detail : register(s0,space2);
            float4 EvaluateParticleMaterial(ParticleMaterialInput input)
            {
                float4 detail = _Detail.Sample(sampler_Detail,input.uv);
                float4 color = input.baseColor*_Tint*input.dynamicParameter0*detail;
                color.rgb *= _Intensity;
                color.a *= 1-input.normalizedAge;
                return color;
            }
            ENDHLSL
        }
    }
}
