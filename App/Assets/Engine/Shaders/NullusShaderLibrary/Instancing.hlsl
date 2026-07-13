#ifndef NULLUS_SHADER_LIBRARY_INSTANCING_INCLUDED
#define NULLUS_SHADER_LIBRARY_INSTANCING_INCLUDED

cbuffer ObjectIndexConstants : register(b1, space3)
{
    uint u_ObjectIndex;
    uint u_ObjectFlags;
    uint u_ObjectPadding0;
    uint u_ObjectPadding1;
};

static const uint NLS_OBJECT_FLAG_RECEIVE_SHADOWS = 1u;
static const uint NLS_OBJECT_FLAG_CAST_SHADOWS = 2u;

#endif
