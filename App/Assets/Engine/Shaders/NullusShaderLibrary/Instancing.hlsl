#ifndef NULLUS_SHADER_LIBRARY_INSTANCING_INCLUDED
#define NULLUS_SHADER_LIBRARY_INSTANCING_INCLUDED

cbuffer ObjectIndexConstants : register(b1, space3)
{
    uint u_ObjectIndex;
};

#endif
