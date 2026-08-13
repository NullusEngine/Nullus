#ifndef NULLUS_SHADER_LIBRARY_INSTANCING_INCLUDED
#define NULLUS_SHADER_LIBRARY_INSTANCING_INCLUDED

#if !defined(NLS_OBJECT_DRAW_CONSTANTS_INCLUDED)
#define NLS_OBJECT_DRAW_CONSTANTS_INCLUDED
#if defined(NLS_SPIRV)
struct NLSObjectDrawConstants
{
    uint objectIndex;
    uint objectFlags;
    uint objectPadding0;
    uint objectPadding1;
};
[[vk::push_constant]] ConstantBuffer<NLSObjectDrawConstants> g_NLSObjectDrawConstants;
#define u_ObjectIndex g_NLSObjectDrawConstants.objectIndex
#define u_ObjectFlags g_NLSObjectDrawConstants.objectFlags
#define u_ObjectPadding0 g_NLSObjectDrawConstants.objectPadding0
#define u_ObjectPadding1 g_NLSObjectDrawConstants.objectPadding1
#else
cbuffer ObjectIndexConstants : register(b1, space3)
{
    uint u_ObjectIndex;
    uint u_ObjectFlags;
    uint u_ObjectPadding0;
    uint u_ObjectPadding1;
};
#endif

static const uint NLS_OBJECT_FLAG_RECEIVE_SHADOWS = 1u;
static const uint NLS_OBJECT_FLAG_CAST_SHADOWS = 2u;
#endif

#endif
