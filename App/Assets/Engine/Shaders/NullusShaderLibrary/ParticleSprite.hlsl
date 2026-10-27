#ifndef NULLUS_PARTICLE_SPRITE_INCLUDED
#define NULLUS_PARTICLE_SPRITE_INCLUDED

// ParticleSprite material ABI 7. The engine supplies SpriteVS. A ShaderLab
// ParticleSprite pass supplies EvaluateParticleMaterial and uses ParticleMaterialPS.
cbuffer VfxSpriteView : register(b0, space1)
{
    row_major float4x4 VfxViewProjection;
    row_major float4x4 VfxLocalToWorld;
    float4 VfxCameraRight; float4 VfxCameraUp;
    float4 VfxTint; float4 VfxUVTransform; float4 VfxDepthProjection; float4 VfxShading;
    uint VfxCapacity; uint VfxSorted; uint VfxAlignment; uint VfxBlend;
    uint VfxSubImagesX; uint VfxSubImagesY; uint VfxBlendSubImages; uint VfxCircularMask;
    float4 VfxDynamicDefaults[4];
};
Texture2D<float4> VfxSpriteTexture : register(t2, space1);
Texture2D<float> VfxSceneDepth : register(t3, space1);
SamplerState VfxSpriteSampler : register(s0, space1);

struct SpriteVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation float frame : TEXCOORD1;
    float4 dynamicParameter0 : TEXCOORD2;
    float4 dynamicParameter1 : TEXCOORD3;
    float4 dynamicParameter2 : TEXCOORD4;
    float4 dynamicParameter3 : TEXCOORD5;
    float2 ageLifetime : TEXCOORD6;
    float3 worldPosition : TEXCOORD7;
    float3 velocity : TEXCOORD8;
};
struct ParticleMaterialInput
{
    float2 uv;
    float4 particleColor;
    float4 baseColor;
    float4 dynamicParameter0, dynamicParameter1, dynamicParameter2, dynamicParameter3;
    float3 worldPosition, velocity;
    float age, lifetime, normalizedAge;
};

float2 ParticleSpriteUV(float2 uv, uint frame)
{
    uint width,height; VfxSpriteTexture.GetDimensions(width,height);
    float2 grid = float2(VfxSubImagesX,VfxSubImagesY);
    uv = uv*VfxUVTransform.xy+VfxUVTransform.zw;
    if (VfxSubImagesX == 1 && VfxSubImagesY == 1) return uv;
    float2 inset = min(0.5*grid/float2(width,height),0.5);
    return (clamp(uv,inset,1-inset)+float2(frame%VfxSubImagesX,frame/VfxSubImagesX))/grid;
}
float ParticleViewDistance(float depth)
{
    float denominator = depth*VfxDepthProjection.z-VfxDepthProjection.x;
    return abs(denominator) > 1e-12 ? abs((VfxDepthProjection.y-depth*VfxDepthProjection.w)/denominator) : 1e30;
}
#ifdef NLS_PARTICLE_DEFAULT_MATERIAL
float4 EvaluateParticleMaterial(ParticleMaterialInput input)
{
    return input.baseColor*input.dynamicParameter0;
}
#else
float4 EvaluateParticleMaterial(ParticleMaterialInput input);
#endif

float4 ParticleMaterialPS(SpriteVertex input) : SV_Target0
{
    uint frame = (uint)floor(input.frame);
    float4 texel = VfxSpriteTexture.Sample(VfxSpriteSampler,ParticleSpriteUV(input.uv,frame));
    if (VfxBlendSubImages != 0)
    {
        uint next = min(frame+1,VfxSubImagesX*VfxSubImagesY-1);
        texel = lerp(texel,VfxSpriteTexture.Sample(VfxSpriteSampler,ParticleSpriteUV(input.uv,next)),frac(input.frame));
    }
    ParticleMaterialInput parameters;
    parameters.uv = input.uv;
    parameters.particleColor = input.color;
    parameters.baseColor = input.color*VfxTint*texel;
    parameters.dynamicParameter0 = input.dynamicParameter0;
    parameters.dynamicParameter1 = input.dynamicParameter1;
    parameters.dynamicParameter2 = input.dynamicParameter2;
    parameters.dynamicParameter3 = input.dynamicParameter3;
    parameters.worldPosition = input.worldPosition; parameters.velocity = input.velocity;
    parameters.age = input.ageLifetime.x; parameters.lifetime = input.ageLifetime.y;
    parameters.normalizedAge = parameters.lifetime > 1e-6 ? saturate(parameters.age/parameters.lifetime) : 1;
    float4 color = EvaluateParticleMaterial(parameters);
    color.rgb *= VfxShading.x;
    if (VfxCircularMask != 0)
    {
        float2 centered = input.uv*2-1; float radius = dot(centered,centered);
        clip(1-radius); color.a *= saturate((1-radius)*4);
    }
    if (VfxShading.z > 0)
    {
        uint width,height; VfxSceneDepth.GetDimensions(width,height);
        int2 pixel = clamp(int2(input.position.xy),int2(0,0),int2(width-1,height-1));
        float sceneDepth = VfxSceneDepth.Load(int3(pixel,0));
        color.a *= saturate((ParticleViewDistance(sceneDepth)-ParticleViewDistance(input.position.z))/VfxShading.z);
    }
    color.a = saturate(color.a);
    if (VfxBlend == 2) clip(color.a-VfxShading.y);
    if (VfxBlend == 2 || VfxBlend == 3) color.a = 1;
    if (VfxBlend == 4) color.rgb *= color.a;
    return color;
}
#endif
