#ifndef NULLUS_SKINNING_INCLUDED
#define NULLUS_SKINNING_INCLUDED
#if defined(NLS_SKINNED)
struct NLSSkinMatrix { row_major float4x4 position; row_major float4x4 normal; };
StructuredBuffer<NLSSkinMatrix> u_SkinningPalette : register(t15, space2);
#define NLS_SKIN_ATTRIBUTES uint4 skinJoints : BLENDINDICES; float4 skinWeights : BLENDWEIGHT;
float3 NLSSkinPosition(float3 p,uint4 joints,float4 weights)
{
    float3 value=0;
    [unroll] for(uint i=0;i<4;++i) if(weights[i]>0)value+=weights[i]*mul(u_SkinningPalette[joints[i]].position,float4(p,1)).xyz;
    return value;
}
float3 NLSSkinNormal(float3 n,uint4 joints,float4 weights)
{
    float3 value=0;
    [unroll] for(uint i=0;i<4;++i) if(weights[i]>0)value+=weights[i]*mul((float3x3)u_SkinningPalette[joints[i]].normal,n);
    return normalize(value+float3(0,0,1e-20));
}
float3 NLSSkinDirection(float3 n,uint4 joints,float4 weights)
{
    float3 value=0;
    [unroll] for(uint i=0;i<4;++i) if(weights[i]>0)value+=weights[i]*mul((float3x3)u_SkinningPalette[joints[i]].position,n);
    return normalize(value+float3(0,0,1e-20));
}
#define NLS_SKIN_POSITION(v) NLSSkinPosition(v.positionOS,v.skinJoints,v.skinWeights)
#define NLS_SKIN_NORMAL(v) NLSSkinNormal(v.normalOS,v.skinJoints,v.skinWeights)
#define NLS_SKIN_TANGENT(v) NLSSkinDirection(v.tangentOS,v.skinJoints,v.skinWeights)
#define NLS_SKIN_BITANGENT(v) NLSSkinDirection(v.bitangentOS,v.skinJoints,v.skinWeights)
#else
#define NLS_SKIN_ATTRIBUTES
#define NLS_SKIN_POSITION(v) v.positionOS
#define NLS_SKIN_NORMAL(v) v.normalOS
#define NLS_SKIN_TANGENT(v) v.tangentOS
#define NLS_SKIN_BITANGENT(v) v.bitangentOS
#endif
#endif
