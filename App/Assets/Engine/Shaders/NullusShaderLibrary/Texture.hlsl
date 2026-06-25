#ifndef NULLUS_SHADER_LIBRARY_TEXTURE_INCLUDED
#define NULLUS_SHADER_LIBRARY_TEXTURE_INCLUDED

float2 TransformTexCoord(float2 uv, float4 scaleOffset)
{
    return uv * scaleOffset.xy + scaleOffset.zw;
}

#endif
