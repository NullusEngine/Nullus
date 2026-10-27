#pragma once

// Bind the immutable DistanceFieldGpuResources and its sampling constants after
// the frame's update passes. No rendering or simulation kernels are included.
#define NLS_DF_SAMPLING_ONLY
#include "DistanceField.hlsl"
#undef NLS_DF_SAMPLING_ONLY

struct DistanceFieldSample
{
    float Distance;
    float3 Gradient;
    float VoxelSize;
    bool Valid;
    uint Clipmap;
};

DistanceFieldSample SampleMeshDistanceField(float3 world,DFObject object)
{
    DistanceFieldSample result=(DistanceFieldSample)0;
    result.Distance=SampleMeshDistanceField(object,world);
    float3 voxel=(object.LocalMaxBand.xyz-object.LocalMinScale.xyz)/float3(object.GridOffset.xyz)/7;
    result.VoxelSize=max(max(voxel.x,voxel.y),voxel.z)*object.LocalMinScale.w;
    result.Valid=all(object.GridOffset.xyz>0) && object.LocalMinScale.w>0;
    float e=max(result.VoxelSize,.00001);
    result.Gradient=float3(SampleMeshDistanceField(object,world+float3(e,0,0))-SampleMeshDistanceField(object,world-float3(e,0,0)),
        SampleMeshDistanceField(object,world+float3(0,e,0))-SampleMeshDistanceField(object,world-float3(0,e,0)),
        SampleMeshDistanceField(object,world+float3(0,0,e))-SampleMeshDistanceField(object,world-float3(0,0,e)))/(2*e);
    return result;
}

DistanceFieldSample SampleGlobalDistanceField(float3 world)
{
    DistanceFieldSample result=(DistanceFieldSample)0;
    result.Distance=SampleGlobalDistanceField(world,result.Valid,result.VoxelSize,result.Clipmap);
    if(!result.Valid) return result;
    float e=max(result.VoxelSize,.00001);
    bool v;float size;uint level;
    result.Gradient=float3(SampleGlobalDistanceField(world+float3(e,0,0),v,size,level)-SampleGlobalDistanceField(world-float3(e,0,0),v,size,level),
        SampleGlobalDistanceField(world+float3(0,e,0),v,size,level)-SampleGlobalDistanceField(world-float3(0,e,0),v,size,level),
        SampleGlobalDistanceField(world+float3(0,0,e),v,size,level)-SampleGlobalDistanceField(world-float3(0,0,e),v,size,level))/(2*e);
    return result;
}
