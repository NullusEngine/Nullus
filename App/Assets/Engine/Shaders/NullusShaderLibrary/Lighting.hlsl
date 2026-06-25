#ifndef NULLUS_SHADER_LIBRARY_LIGHTING_INCLUDED
#define NULLUS_SHADER_LIBRARY_LIGHTING_INCLUDED

struct NullusSimpleLight
{
    float3 directionWS;
    float intensity;
    float3 color;
    float padding0;
};

#endif
