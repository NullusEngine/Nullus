#ifndef NULLUS_SHADER_LIBRARY_FOG_INCLUDED
#define NULLUS_SHADER_LIBRARY_FOG_INCLUDED

float3 ApplyNullusFog(float3 color, float fogFactor, float3 fogColor)
{
    return lerp(color, fogColor, saturate(fogFactor));
}

#endif
