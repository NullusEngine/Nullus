// Software sparse mesh bricks and paged, toroidally addressed global clipmaps.
struct DFObject
{
    float4 WorldToLocal0,WorldToLocal1,WorldToLocal2;
    float4 LocalMinScale,LocalMaxBand;
    uint4 GridOffset;
    float4 WorldMinStatic,WorldMax;
};
struct DFPage { uint TableIndex,Clipmap,Cache,Padding; float3 Min; float Voxel; };
cbuffer DFConstants : register(b0)
{
    uint ObjectCount,DirtyCount,PageCapacity,MeshBricksXY;
    uint AtlasPagesX,AtlasPagesY,ProbeCount,CacheFilter;
    uint Width,Height,Visualization,DebugChannel;
    float4 ClipMinVoxel[4];
    row_major float4x4 InverseViewProjection;
    float4 CameraPosition;
    float SlicePosition,TraceDistance; uint MostlyStaticOnly,Padding;
};
StructuredBuffer<DFObject> Objects : register(t0);
StructuredBuffer<uint> MeshIndirection : register(t1);
StructuredBuffer<DFPage> DirtyPages : register(t2);
Texture3D<float> MeshAtlas : register(t3);
StructuredBuffer<float4> ProbePositions : register(t4);
Texture3D<float> GlobalAtlas : register(t5);
SamplerState LinearClamp : register(s0);
RWStructuredBuffer<uint> PageTable : register(u0);
RWStructuredBuffer<uint> FreePages : register(u1);
// Pool free count, failed allocations, allocated pages, composed pages, retries.
RWStructuredBuffer<uint> PoolCounters : register(u2);
RWStructuredBuffer<uint> Candidates : register(u3);
RWStructuredBuffer<uint> ComposeWork : register(u4);
RWStructuredBuffer<uint> DispatchArguments : register(u5);
RWTexture3D<float> GlobalAtlasWrite : register(u6);
RWTexture2D<float4> DebugOutput : register(u7);
RWStructuredBuffer<float4> ProbeResults : register(u8);
static const uint InvalidPage=0xffffffffu,InsideBrick=0xfffffffeu;
static const uint RetryPage=0xfffffffdu;
static const uint Grid=20,PagesPerClipmap=8000,CandidateStride=66;

uint3 AtlasCoordinate(uint page)
{ return uint3(page%AtlasPagesX,page/AtlasPagesX%AtlasPagesY,page/(AtlasPagesX*AtlasPagesY))*8; }

float SampleMeshDistanceField(DFObject object,float3 world)
{
    float4 p=float4(world,1);
    float3 local=float3(dot(object.WorldToLocal0,p),dot(object.WorldToLocal1,p),dot(object.WorldToLocal2,p));
    float3 size=object.LocalMaxBand.xyz-object.LocalMinScale.xyz;
    float3 t=saturate((local-object.LocalMinScale.xyz)/size)*float3(object.GridOffset.xyz)*7;
    uint3 cell=min((uint3)(t/7),object.GridOffset.xyz-1);
    float3 samplePosition=t-float3(cell)*7;
    uint entry=MeshIndirection[object.GridOffset.w+(cell.z*object.GridOffset.y+cell.y)*object.GridOffset.x+cell.x];
    float distance=object.LocalMaxBand.w;
    if(entry==InsideBrick) distance=-distance;
    else if(entry!=InvalidPage)
    {
        uint3 base=uint3(entry%MeshBricksXY,entry/MeshBricksXY%MeshBricksXY,entry/(MeshBricksXY*MeshBricksXY))*8;
        uint w,h,d; MeshAtlas.GetDimensions(w,h,d);
        distance=(MeshAtlas.SampleLevel(LinearClamp,(float3(base)+samplePosition+.5)/float3(w,h,d),0)*2-1)*object.LocalMaxBand.w;
    }
    float3 outside=max(max(object.LocalMinScale.xyz-local,local-object.LocalMaxBand.xyz),0);
    if(any(outside>0)) distance=sqrt(dot(outside,outside)+max(distance,0)*max(distance,0));
    return distance*object.LocalMinScale.w;
}

bool Overlap(DFObject object,DFPage page)
{
    float band=page.Voxel*6.92820323;
    return all(object.WorldMinStatic.xyz<=page.Min+page.Voxel*7+band) &&
        all(object.WorldMax.xyz>=page.Min-band);
}
#ifndef NLS_DF_SAMPLING_ONLY
[numthreads(64,1,1)]
void CullPages(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=DirtyCount) return;
    DFPage page=DirtyPages[id.x]; uint count=0,total=0;
    for(uint objectIndex=0;objectIndex<ObjectCount;++objectIndex)
    {
        DFObject object=Objects[objectIndex];
        bool isStatic=object.WorldMinStatic.w!=0;
        if((page.Cache==0 && !isStatic) || !Overlap(object,page)) continue;
        ++total;
        if(page.Cache==1 && isStatic) continue;
        if(count<64) Candidates[id.x*CandidateStride+2+count]=objectIndex;
        ++count;
    }
    Candidates[id.x*CandidateStride]=count;
    Candidates[id.x*CandidateStride+1]=total;
}
[numthreads(64,1,1)]
void ReleasePages(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=DirtyCount || Candidates[id.x*CandidateStride+1]!=0) return;
    DFPage page=DirtyPages[id.x]; uint old;
    InterlockedExchange(PageTable[page.TableIndex],InvalidPage,old);
    if(old<PageCapacity)
    {
        uint freeIndex; InterlockedAdd(PoolCounters[0],1,freeIndex);
        FreePages[freeIndex]=old;
        InterlockedAdd(PoolCounters[2],0xffffffffu);
    }
}
[numthreads(64,1,1)]
void AllocatePages(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=DirtyCount || Candidates[id.x*CandidateStride+1]==0) return;
    DFPage page=DirtyPages[id.x];
    if(PageTable[page.TableIndex]>=PageCapacity)
    {
        if(PageTable[page.TableIndex]==RetryPage) InterlockedAdd(PoolCounters[4],1);
        uint available=PoolCounters[0],observed;
        for(;;)
        {
            if(available==0) { PageTable[page.TableIndex]=RetryPage; InterlockedAdd(PoolCounters[1],1); return; }
            InterlockedCompareExchange(PoolCounters[0],available,available-1,observed);
            if(observed==available) break;
            available=observed;
        }
        PageTable[page.TableIndex]=FreePages[available-1];
        InterlockedAdd(PoolCounters[2],1);
    }
    uint workIndex; InterlockedAdd(DispatchArguments[0],1,workIndex);
    ComposeWork[workIndex]=id.x;
}
[numthreads(8,8,8)]
void ComposePages(uint3 group:SV_GroupID,uint3 cell:SV_GroupThreadID)
{
    uint work=ComposeWork[group.x]; DFPage page=DirtyPages[work];
    if(page.Cache!=CacheFilter) return;
    uint physical=PageTable[page.TableIndex];
    if(physical>=PageCapacity) return;
    float3 p=page.Min+float3(cell)*page.Voxel;
    float distance=page.Voxel*6.92820323;
    bool parentValid=false;
    if(page.Cache==1)
    {
        uint parent=PageTable[page.TableIndex-4*PagesPerClipmap];
        if(parent<PageCapacity) { distance=GlobalAtlasWrite[AtlasCoordinate(parent)+cell]; parentValid=true; }
    }
    uint count=Candidates[work*CandidateStride];
    bool scanAll=count>64 || (page.Cache==1 && !parentValid);
    uint n=scanAll?ObjectCount:count;
    for(uint i=0;i<n;++i)
    {
        uint objectIndex=scanAll?i:Candidates[work*CandidateStride+2+i];
        DFObject object=Objects[objectIndex];
        bool isStatic=object.WorldMinStatic.w!=0;
        if((page.Cache==0 && !isStatic) || (page.Cache==1 && parentValid && isStatic)) continue;
        if(Overlap(object,page)) distance=min(distance,SampleMeshDistanceField(object,p));
    }
    GlobalAtlasWrite[AtlasCoordinate(physical)+cell]=clamp(distance,-page.Voxel*6.92820323,page.Voxel*6.92820323);
    if(all(cell==0)) InterlockedAdd(PoolCounters[3],1);
}

#endif

float SampleGlobalDistanceField(float3 p,out bool valid,out float voxel,out uint level)
{
    valid=false; voxel=0; level=0;
    for(uint clip=0;clip<4;++clip)
    {
        float4 c=ClipMinVoxel[clip];
        float3 t=(p-c.xyz)/(c.w*7);
        if(any(t<0) || any(t>=20)) continue;
        if(voxel==0) { voxel=c.w; level=clip; }
        int3 absolutePage=(int3)floor(p/(c.w*7));
        uint3 wrapped=(uint3)((absolutePage%20+20)%20);
        uint table=(MostlyStaticOnly?0:4)*PagesPerClipmap+clip*PagesPerClipmap+(wrapped.z*20+wrapped.y)*20+wrapped.x;
        uint page=PageTable[table];
        if(page>=PageCapacity) continue;
        float3 fraction=frac(p/(c.w*7))*7;
        uint w,h,d; GlobalAtlas.GetDimensions(w,h,d);
        valid=true; voxel=c.w; level=clip;
        return GlobalAtlas.SampleLevel(LinearClamp,(float3(AtlasCoordinate(page))+fraction+.5)/float3(w,h,d),0);
    }
    return 1.e20;
}

float SampleScene(float3 p,out bool valid,out float voxel,out uint level)
{
    if(Visualization==2) return SampleGlobalDistanceField(p,valid,voxel,level);
    float distance=1.e20; valid=false; voxel=.01; level=0;
    for(uint i=0;i<ObjectCount;++i)
    {
        float d=SampleMeshDistanceField(Objects[i],p);
        if(d<distance)
        {
            distance=d; valid=true; level=(uint)Objects[i].WorldMax.w;
            voxel=max(max((Objects[i].LocalMaxBand.x-Objects[i].LocalMinScale.x)/Objects[i].GridOffset.x,
                (Objects[i].LocalMaxBand.y-Objects[i].LocalMinScale.y)/Objects[i].GridOffset.y),
                (Objects[i].LocalMaxBand.z-Objects[i].LocalMinScale.z)/Objects[i].GridOffset.z)*Objects[i].LocalMinScale.w/7;
        }
    }
    return distance;
}

#ifndef NLS_DF_SAMPLING_ONLY
[numthreads(8,8,1)]
void Visualize(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=Width || id.y>=Height) return;
    float2 uv=(float2(id.xy)+.5)/float2(Width,Height);
    bool valid; float voxel; uint level;
    float3 p; float distance; uint steps=0; bool hit=false;
    if(DebugChannel==6 && Visualization==2)
    {
        float4 c=ClipMinVoxel[0];
        p=float3(c.x+uv.x*c.w*140,SlicePosition,c.z+uv.y*c.w*140);
        distance=SampleGlobalDistanceField(p,valid,voxel,level);
        float3 color=valid?(level==0?float3(.1,.9,.25):level==1?float3(.9,.8,.15):float3(.9,.4,.1)):float3(.5,.03,.05);
        float3 edge=frac(p/(max(voxel,c.w)*7));
        if(any(edge<.025)||any(edge>.975)) color*=.35;
        DebugOutput[id.xy]=float4(color,1); return;
    }
    if(DebugChannel==2)
    {
        float4 c=ClipMinVoxel[0];
        p=float3(c.x+uv.x*c.w*140,SlicePosition,c.z+uv.y*c.w*140);
        distance=SampleScene(p,valid,voxel,level);
        float v=saturate(abs(distance)/max(voxel*4,.001));
        DebugOutput[id.xy]=float4(valid?(distance<0?float3(.1,.3,1)*v:float3(1,.25,.1)*v):float3(.1,.1,.1),1); return;
    }
    float4 nearPoint=mul(InverseViewProjection,float4(uv.x*2-1,1-uv.y*2,0,1)); nearPoint/=nearPoint.w;
    float4 farPoint=mul(InverseViewProjection,float4(uv.x*2-1,1-uv.y*2,1,1)); farPoint/=farPoint.w;
    float3 direction=normalize(farPoint.xyz-nearPoint.xyz);
    float travel=0;
    for(steps=0;steps<512 && travel<TraceDistance;++steps)
    {
        p=nearPoint.xyz+direction*travel;
        distance=SampleScene(p,valid,voxel,level);
        if(valid && distance<max(voxel*.25,.0001)) { hit=true; break; }
        // An empty global page advances conservatively across the nearest clipmap.
        travel+=valid?max(distance*.8,max(voxel*.1,.001)):max(voxel*3,max(ClipMinVoxel[0].w*3,.05));
    }
    float3 color=float3(.035,.04,.05);
    if(DebugChannel==1) color=steps/512.0;
    else if(hit)
    {
        bool v;float vs;uint l; float e=max(voxel,.0001);
        float3 n=float3(SampleScene(p+float3(e,0,0),v,vs,l)-SampleScene(p-float3(e,0,0),v,vs,l),
            SampleScene(p+float3(0,e,0),v,vs,l)-SampleScene(p-float3(0,e,0),v,vs,l),
            SampleScene(p+float3(0,0,e),v,vs,l)-SampleScene(p-float3(0,0,e),v,vs,l));
        n=length(n)>.00001?normalize(n):float3(0,1,0);
        color=DebugChannel==3?n*.5+.5:(.2+.8*saturate(dot(n,normalize(float3(.4,1,.2))))).xxx;
        if(DebugChannel==4) color=level==0?float3(.3,1,.3):level==1?float3(1,1,.2):level==2?float3(.2,.6,1):float3(1,.2,1);
        if(DebugChannel==5)
        {
            float3 edge=frac(p/(voxel*7));
            color=any(edge<.025)||any(edge>.975)?float3(1,.25,.1):float3(.2,.7,.9);
        }
        if(DebugChannel==6) color=level==0?float3(.1,.9,.25):level==1?float3(.9,.8,.15):float3(.9,.25,.1);
    }
    DebugOutput[id.xy]=float4(color,1);
}
[numthreads(64,1,1)]
void Probe(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=ProbeCount) return;
    bool valid;float voxel;uint level;
    float d=SampleScene(ProbePositions[id.x].xyz,valid,voxel,level);
    ProbeResults[id.x]=float4(d,voxel,valid?1:0,level);
}
[numthreads(1,1,1)] void UploadMarker(uint3 id:SV_DispatchThreadID) {}
[numthreads(1,1,1)] void ResetWork(uint3 id:SV_DispatchThreadID)
{ DispatchArguments[0]=0; DispatchArguments[1]=1; DispatchArguments[2]=1; }
[numthreads(64,1,1)] void ReleaseView(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=64000) return;
    uint old; InterlockedExchange(PageTable[id.x],InvalidPage,old);
    if(old<PageCapacity)
    {
        uint freeIndex; InterlockedAdd(PoolCounters[0],1,freeIndex);
        FreePages[freeIndex]=old;
        InterlockedAdd(PoolCounters[2],0xffffffffu);
    }
}
#endif
