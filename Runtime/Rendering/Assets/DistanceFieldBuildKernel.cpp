#include "Rendering/Assets/DistanceFieldBuilder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include "Rendering/Assets/DistanceFieldArtifact.h"
#if NLS_HAS_EMBREE
#include <embree4/rtcore.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace NLS::Render::DistanceField
{
namespace
{
using V = Maths::Vector3;
using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
float Dot(V a, V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
V Cross(V a, V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
V Min(V a,V b) { return {(std::min)(a.x,b.x),(std::min)(a.y,b.y),(std::min)(a.z,b.z)}; }
V Max(V a,V b) { return {(std::max)(a.x,b.x),(std::max)(a.y,b.y),(std::max)(a.z,b.z)}; }
float BoxDistance(V p,V lo,V hi)
{
    float sum=0;
    for(int a=0;a<3;++a) { const float d=(std::max)({lo.array[a]-p.array[a],p.array[a]-hi.array[a],0.f}); sum+=d*d; }
    return sum;
}
struct Triangle { V a,ab,ac,normal,lo,hi; };
float PointTriangle(V p,const Triangle& t)
{
    const auto ab=t.ab,ac=t.ac,ap=p-t.a;
    const float d1=Dot(ab,ap),d2=Dot(ac,ap);
    if(d1<=0 && d2<=0) return Dot(ap,ap);
    const V bp=ap-ab;
    const float d3=Dot(ab,bp),d4=Dot(ac,bp);
    if(d3>=0 && d4<=d3) return Dot(bp,bp);
    const float vc=d1*d4-d3*d2;
    if(vc<=0 && d1>=0 && d3<=0) { const V q=ap-ab*(d1/(d1-d3)); return Dot(q,q); }
    const V cp=ap-ac;
    const float d5=Dot(ab,cp),d6=Dot(ac,cp);
    if(d6>=0 && d5<=d6) return Dot(cp,cp);
    const float vb=d5*d2-d1*d6;
    if(vb<=0 && d2>=0 && d6<=0) { const V q=ap-ac*(d2/(d2-d6)); return Dot(q,q); }
    const float va=d3*d6-d5*d4;
    if(va<=0 && d4-d3>=0 && d5-d6>=0)
    { const V q=bp-(ac-ab)*((d4-d3)/((d4-d3)+(d5-d6))); return Dot(q,q); }
    const float inverse=1/(va+vb+vc);
    const V q=ap-ab*(vb*inverse)-ac*(vc*inverse);
    return Dot(q,q);
}
struct Ray
{
    V direction{}, inverse{};
    Ray() = default;
    explicit Ray(V d):direction(d)
    { for(int a=0;a<3;++a) inverse.array[a]=std::abs(d.array[a])<1.e-12f?0:1/d.array[a]; }
};
const std::array<Ray,32>& Rays()
{
    static const auto rays=[]
    {
        std::array<Ray,32> value{Ray(V{})};
        for(uint32_t i=0;i<value.size();++i)
        {
            const float z=1-2*(float(i)+.5f)/value.size(),r=std::sqrt(1-z*z),phi=float(i)*2.39996323f;
            value[i]=Ray({r*std::cos(phi),r*std::sin(phi),z});
        }
        return value;
    }();
    return rays;
}
bool RayBox(V p,const Ray& ray,V lo,V hi,float maximum,float& entryDistance)
{
    entryDistance=0; float exitDistance=maximum;
    for(int a=0;a<3;++a)
    {
        if(ray.inverse.array[a]==0) { if(p.array[a]<lo.array[a] || p.array[a]>hi.array[a]) return false; continue; }
        float a0=(lo.array[a]-p.array[a])*ray.inverse.array[a],a1=(hi.array[a]-p.array[a])*ray.inverse.array[a];
        if(a0>a1) std::swap(a0,a1);
        entryDistance=(std::max)(entryDistance,a0); exitDistance=(std::min)(exitDistance,a1);
        if(entryDistance>exitDistance) return false;
    }
    return true;
}

// Dedicated, persistent workers keep long CPU builds off the render/game queues.
// All requests share this pool; a build never starts another pool per mesh/mip.
class Workers
{
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool stopping=false;
    std::vector<std::thread> threads;
public:
    Workers()
    {
        const auto hardware=(std::max)(1u,std::thread::hardware_concurrency());
        const uint32_t count=(std::min)(16u,hardware>4?hardware-4:1u);
        for(uint32_t i=0;i<count;++i) threads.emplace_back([this]
        {
#if defined(_WIN32)
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
#endif
            for(;;)
            {
                std::function<void()> job;
                { std::unique_lock lock(mutex); wake.wait(lock,[&]{return stopping || !jobs.empty();});
                  if(stopping && jobs.empty()) return; job=std::move(jobs.front()); jobs.pop_front(); }
                job();
            }
        });
    }
    ~Workers()
    {
        { std::lock_guard lock(mutex); stopping=true; } wake.notify_all();
        for(auto& thread:threads) thread.join();
    }
    uint32_t Count() const { return static_cast<uint32_t>(threads.size()); }
    void Run(uint32_t count,uint32_t requested,const std::function<void(uint32_t,uint32_t)>& body)
    {
        if(!count) return;
        const auto active=(std::min)({Count(),requested?requested:Count(),count});
        std::atomic_uint32_t next{0};
        std::vector<std::future<void>> completion;
        for(uint32_t worker=0;worker<active;++worker)
        {
            auto task=std::make_shared<std::packaged_task<void()>>([&,worker]
            {
                for(uint32_t first;(first=next.fetch_add(4,std::memory_order_relaxed))<count;)
                    for(uint32_t i=first;i<(std::min)(first+4,count);++i) body(worker,i);
            });
            completion.push_back(task->get_future());
            { std::lock_guard lock(mutex); jobs.emplace_back([task]{(*task)();}); }
        }
        wake.notify_all();
        for(auto& future:completion) future.get();
    }
};
Workers& Pool() { static Workers pool; return pool; }

#if NLS_HAS_EMBREE
struct EmbreeDevice
{
    RTCDevice value=nullptr;
    EmbreeDevice()
    {
        const auto hardware=(std::max)(1u,std::thread::hardware_concurrency());
        const auto config="threads="+std::to_string((std::min)(16u,hardware>4?hardware-4:1u));
        value=rtcNewDevice(config.c_str());
    }
    ~EmbreeDevice() { if(value) rtcReleaseDevice(value); }
};
std::shared_ptr<EmbreeDevice> Device() { static auto value=std::make_shared<EmbreeDevice>(); return value; }
#endif
bool UseEmbree(DistanceFieldQueryBackend backend)
{
#if NLS_HAS_EMBREE
    return backend!=DistanceFieldQueryBackend::BuiltIn && Device()->value;
#else
    (void)backend; return false;
#endif
}
struct QueryStats { uint64_t distances=0,rays=0,triangles=0,samples=0,shared=0; double distanceMs=0,signMs=0; };
struct Geometry
{
    struct Node { V lo,hi; uint32_t begin=0,count=0,left=0,right=0; };
    std::vector<Triangle> triangles;
    std::vector<Node> nodes;
    std::vector<std::string> diagnostics;
#if NLS_HAS_EMBREE
    std::shared_ptr<EmbreeDevice> device;
    RTCScene scene=nullptr;
    ~Geometry() { if(scene) rtcReleaseScene(scene); }
#endif
    uint64_t Bytes() const { return triangles.size()*sizeof(Triangle)+nodes.size()*sizeof(Node); }
    static float Area(V lo,V hi) { const auto d=hi-lo; return 2*(d.x*d.y+d.y*d.z+d.z*d.x); }
    uint32_t Build(uint32_t begin,uint32_t count,uint32_t depth=0)
    {
        const auto index=static_cast<uint32_t>(nodes.size()); nodes.emplace_back();
        V lo=triangles[begin].lo,hi=triangles[begin].hi;
        for(uint32_t i=1;i<count;++i) { lo=Min(lo,triangles[begin+i].lo); hi=Max(hi,triangles[begin+i].hi); }
        nodes[index]={lo,hi,begin,count};
        if(count<=8) return index;
        constexpr uint32_t bins=16;
        float best=std::numeric_limits<float>::max(); int axis=-1; uint32_t split=0;
        for(int a=0;a<3 && depth<48;++a)
        {
            const float extent=hi.array[a]-lo.array[a]; if(extent<1.e-12f) continue;
            struct Bin { V lo,hi; uint32_t count=0; };
            std::array<Bin,bins> bucket{};
            for(uint32_t i=0;i<count;++i)
            {
                const auto& t=triangles[begin+i];
                const auto b=(std::min)(bins-1,uint32_t((.5f*(t.lo.array[a]+t.hi.array[a])-lo.array[a])/extent*bins));
                auto& v=bucket[b]; if(v.count++) { v.lo=Min(v.lo,t.lo); v.hi=Max(v.hi,t.hi); } else {v.lo=t.lo;v.hi=t.hi;}
            }
            std::array<float,bins> leftArea{},rightArea{}; std::array<uint32_t,bins> leftCount{},rightCount{};
            V l{},h{}; uint32_t n=0;
            for(uint32_t b=0;b<bins;++b) { const auto& v=bucket[b]; if(v.count) {if(n){l=Min(l,v.lo);h=Max(h,v.hi);}else{l=v.lo;h=v.hi;} n+=v.count;} leftCount[b]=n;leftArea[b]=n?Area(l,h):0; }
            n=0;
            for(int b=bins-1;b>=0;--b) { const auto& v=bucket[b]; if(v.count) {if(n){l=Min(l,v.lo);h=Max(h,v.hi);}else{l=v.lo;h=v.hi;} n+=v.count;} rightCount[b]=n;rightArea[b]=n?Area(l,h):0; }
            for(uint32_t b=0;b+1<bins;++b) if(leftCount[b] && rightCount[b+1])
            { const float cost=leftArea[b]*leftCount[b]+rightArea[b+1]*rightCount[b+1]; if(cost<best){best=cost;axis=a;split=b;} }
        }
        uint32_t middle=begin;
        if(axis>=0)
        {
            const float threshold=lo.array[axis]+(hi.array[axis]-lo.array[axis])*float(split+1)/bins;
            middle=static_cast<uint32_t>(std::partition(triangles.begin()+begin,triangles.begin()+begin+count,
                [&](const auto& t){return .5f*(t.lo.array[axis]+t.hi.array[axis])<threshold;})-triangles.begin());
        }
        if(middle==begin || middle==begin+count)
        {
            const auto size=hi-lo; axis=size.x>size.y?(size.x>size.z?0:2):(size.y>size.z?1:2); middle=begin+count/2;
            std::nth_element(triangles.begin()+begin,triangles.begin()+middle,triangles.begin()+begin+count,
                [axis](const auto& a,const auto& b){return a.lo.array[axis]+a.hi.array[axis]<b.lo.array[axis]+b.hi.array[axis];});
        }
        const auto left=Build(begin,middle-begin,depth+1),right=Build(middle,begin+count-middle,depth+1);
        nodes[index].count=0;nodes[index].left=left;nodes[index].right=right; return index;
    }
    bool InitializeEmbree()
    {
#if NLS_HAS_EMBREE
        device=Device(); if(!device->value) return false;
        scene=rtcNewScene(device->value); if(!scene) return false;
        RTCGeometry geometry=rtcNewGeometry(device->value,RTC_GEOMETRY_TYPE_TRIANGLE);
        auto* vertices=static_cast<V*>(rtcSetNewGeometryBuffer(geometry,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT3,sizeof(V),triangles.size()*3));
        auto* indices=static_cast<uint32_t*>(rtcSetNewGeometryBuffer(geometry,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,12,triangles.size()));
        if(!vertices || !indices) {rtcReleaseGeometry(geometry);return false;}
        for(uint32_t i=0;i<triangles.size();++i)
        { const auto& t=triangles[i];vertices[i*3]=t.a;vertices[i*3+1]=t.a+t.ab;vertices[i*3+2]=t.a+t.ac;
          indices[i*3]=i*3;indices[i*3+1]=i*3+1;indices[i*3+2]=i*3+2; }
        rtcSetGeometryBuildQuality(geometry,RTC_BUILD_QUALITY_HIGH);rtcCommitGeometry(geometry);
        rtcAttachGeometry(scene,geometry);rtcReleaseGeometry(geometry);rtcCommitScene(scene);
        return rtcGetDeviceError(device->value)==RTC_ERROR_NONE;
#else
        return false;
#endif
    }
    float Distance(V p,float maximum,QueryStats& stats,bool& found,bool embree) const
    {
        ++stats.distances; found=false; float best=maximum*maximum;
#if NLS_HAS_EMBREE
        if(embree && scene)
        {
            struct Context { const Geometry* geometry; V p; float* best; bool* found; QueryStats* stats; } data{this,p,&best,&found,&stats};
            RTCPointQuery query{p.x,p.y,p.z,0,maximum}; RTCPointQueryContext context;rtcInitPointQueryContext(&context);
            rtcPointQuery(scene,&query,&context,[](RTCPointQueryFunctionArguments* args)
            {
                auto& d=*static_cast<Context*>(args->userPtr);++d.stats->triangles;
                const float distance=PointTriangle(d.p,d.geometry->triangles[args->primID]);
                if(distance<=*d.best) {*d.best=distance;*d.found=true;args->query->radius=std::sqrt(distance);return true;} return false;
            },&data);
            return std::sqrt(best);
        }
#else
        (void)embree;
#endif
        uint32_t stack[128],top=1;stack[0]=0;
        while(top)
        {
            const auto& node=nodes[stack[--top]];
            if(BoxDistance(p,node.lo,node.hi)>best) continue;
            if(node.count) for(uint32_t i=0;i<node.count;++i)
            {++stats.triangles;const float d=PointTriangle(p,triangles[node.begin+i]);if(d<=best){best=d;found=true;}}
            else
            {
                const float dl=BoxDistance(p,nodes[node.left].lo,nodes[node.left].hi),dr=BoxDistance(p,nodes[node.right].lo,nodes[node.right].hi);
                stack[top++]=dl<dr?node.right:node.left;stack[top++]=dl<dr?node.left:node.right;
            }
        }
        return std::sqrt(best);
    }
    bool Backface(V p,const Ray& ray,float maximum,QueryStats& stats,bool embree) const
    {
        ++stats.rays;
#if NLS_HAS_EMBREE
        if(embree && scene)
        {
            RTCRayHit hit{};hit.ray.org_x=p.x;hit.ray.org_y=p.y;hit.ray.org_z=p.z;
            hit.ray.dir_x=ray.direction.x;hit.ray.dir_y=ray.direction.y;hit.ray.dir_z=ray.direction.z;
            hit.ray.tnear=1.e-7f;hit.ray.tfar=maximum;hit.ray.mask=UINT32_MAX;hit.hit.geomID=RTC_INVALID_GEOMETRY_ID;
            RTCIntersectArguments arguments;rtcInitIntersectArguments(&arguments);rtcIntersect1(scene,&hit,&arguments);
            return hit.hit.geomID!=RTC_INVALID_GEOMETRY_ID && Dot(ray.direction,triangles[hit.hit.primID].normal)>0;
        }
#else
        (void)embree;
#endif
        bool back=false;uint32_t stack[128],top=1;stack[0]=0;
        while(top)
        {
            const auto& node=nodes[stack[--top]];float entryDistance;
            if(!RayBox(p,ray,node.lo,node.hi,maximum,entryDistance)) continue;
            if(!node.count)
            {
                float nl,nr;const auto& l=nodes[node.left];const auto& r=nodes[node.right];
                const bool bl=RayBox(p,ray,l.lo,l.hi,maximum,nl),br=RayBox(p,ray,r.lo,r.hi,maximum,nr);
                if(bl && br) {stack[top++]=nl<nr?node.right:node.left;stack[top++]=nl<nr?node.left:node.right;}
                else if(bl) stack[top++]=node.left;else if(br) stack[top++]=node.right;
                continue;
            }
            for(uint32_t i=0;i<node.count;++i)
            {
                ++stats.triangles;const auto& t=triangles[node.begin+i];const V h=Cross(ray.direction,t.ac);
                const float det=Dot(t.ab,h);if(std::abs(det)<1.e-12f) continue;
                const V s=p-t.a;const float u=Dot(s,h)/det;if(u<0 || u>1) continue;
                const V q=Cross(s,t.ab);const float v=Dot(ray.direction,q)/det;if(v<0 || u+v>1) continue;
                const float distance=Dot(t.ac,q)/det;
                if(distance>1.e-7f && distance<maximum) {maximum=distance;back=Dot(ray.direction,t.normal)>0;}
            }
        }
        return back;
    }
};
struct EdgeHash
{
    size_t operator()(const std::array<uint32_t,6>& edge) const
    { size_t h=1469598103934665603ull;for(auto n:edge){h^=n;h*=1099511628211ull;}return h; }
};
struct GeometryCache
{
    std::mutex mutex;
    struct Entry {std::shared_ptr<const Geometry> geometry;uint64_t used=0;};
    std::unordered_map<std::string,Entry> values;
    uint64_t serial=0,bytes=0;
};
GeometryCache& Geometries() { static GeometryCache cache;return cache; }
// One active allocation of sampling scratch; concurrent requests still share
// completed geometry and disk results, and cancellation remains pollable.
std::timed_mutex& Admission() {static std::timed_mutex mutex;return mutex;}
std::shared_ptr<const Geometry> Prepare(std::span<const Assets::MeshArtifactData> sections,const std::string& identity,
    bool embree,DistanceFieldBuildStats& stats,const std::function<bool()>& cancelled)
{
    auto& cache=Geometries();const auto key=identity+(embree?"/embree":"/builtin");
    {std::lock_guard lock(cache.mutex);if(auto it=cache.values.find(key);it!=cache.values.end()){it->second.used=++cache.serial;return it->second.geometry;}}
    const auto start=Clock::now();auto geometry=std::make_shared<Geometry>();uint32_t degenerate=0;
    std::unordered_map<std::array<uint32_t,6>,uint32_t,EdgeHash> edges;
    for(const auto& mesh:sections)
    {
        geometry->triangles.reserve(geometry->triangles.size()+mesh.indices.size()/3);
        for(size_t i=0;i<mesh.indices.size();i+=3)
        {
            if((i&4095)==0 && cancelled()) return {};
            V points[3];
            for(int c=0;c<3;++c) std::copy_n(mesh.vertices[mesh.indices[i+c]].position,3,points[c].array);
            Triangle t;t.a=points[0];t.ab=points[1]-points[0];t.ac=points[2]-points[0];t.normal=Cross(t.ab,t.ac);
            if(Dot(t.normal,t.normal)<1.e-20f){++degenerate;continue;}
            t.lo=Min(Min(points[0],points[1]),points[2]);t.hi=Max(Max(points[0],points[1]),points[2]);
            for(int e=0;e<3;++e)
            {
                std::array<uint32_t,3> a,b;
                for(int axis=0;axis<3;++axis){a[axis]=std::bit_cast<uint32_t>(points[e].array[axis]==0?0.f:points[e].array[axis]);b[axis]=std::bit_cast<uint32_t>(points[(e+1)%3].array[axis]==0?0.f:points[(e+1)%3].array[axis]);}
                if(b<a)std::swap(a,b);++edges[{a[0],a[1],a[2],b[0],b[1],b[2]}];
            }
            geometry->triangles.push_back(t);
        }
    }
    if(geometry->triangles.empty()) return {};
    if(degenerate) geometry->diagnostics.push_back("Distance field ignored "+std::to_string(degenerate)+" degenerate triangles");
    if(std::any_of(edges.begin(),edges.end(),[](const auto& e){return e.second!=2;}))
        geometry->diagnostics.push_back("Distance field source is open or non-manifold; signed interior is approximate");
    edges.clear();edges.rehash(0);geometry->nodes.reserve(geometry->triangles.size()/2+1);geometry->Build(0,static_cast<uint32_t>(geometry->triangles.size()));
    if(embree && !geometry->InitializeEmbree()) return {};
    stats.bvhMs+=Ms(start);++stats.bvhBuilds;
    {
        std::lock_guard lock(cache.mutex);constexpr uint64_t limit=256ull*1024*1024;
        const auto size=geometry->Bytes();
        while(!cache.values.empty() && cache.bytes+size>limit)
        {
            auto oldest=cache.values.end();
            for(auto it=cache.values.begin();it!=cache.values.end();++it)
                if(it->second.geometry.use_count()==1 && (oldest==cache.values.end() || it->second.used<oldest->second.used))oldest=it;
            if(oldest==cache.values.end())break;cache.bytes-=oldest->second.geometry->Bytes();cache.values.erase(oldest);
        }
        if(size<=limit && cache.bytes+size<=limit){cache.bytes+=size;cache.values[key]={geometry,++cache.serial};}
    }
    return geometry;
}
std::vector<uint8_t> Read(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return {};
    const auto size=file.tellg();if(size<=0 || size>std::streamoff(1024ull*1024*1024))return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);return file?bytes:std::vector<uint8_t>{};
}
void CacheWrite(const std::filesystem::path& path,const std::vector<uint8_t>& bytes)
{
    if(bytes.empty())return;std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error)return;
    const auto temporary=path.string()+"."+NLS::Guid::New().ToString()+".tmp";
    {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!file){file.close();std::filesystem::remove(temporary,error);return;}}
    std::filesystem::rename(temporary,path,error);if(error)std::filesystem::remove(temporary,error);
}
}

std::string DistanceFieldQueryBackendName(DistanceFieldQueryBackend backend) {return UseEmbree(backend)?"embree4":"builtin-sah";}
std::string ComputeDistanceFieldGeometryIdentity(std::span<const Assets::MeshArtifactData> sections)
{
    NLS::Core::Assets::Sha256Hasher all;
    std::array<uint8_t,16384> bytes{};size_t used=0;
    for(const auto& mesh:sections)
    {
        if(mesh.indices.size()%3)return {};
        for(auto index:mesh.indices)
        {
            if(index>=mesh.vertices.size())return {};
            for(int a=0;a<3;++a)
            {
                const float v=mesh.vertices[index].position[a];if(!std::isfinite(v))return {};
                const auto word=std::bit_cast<uint32_t>(v==0?0.f:v);
                for(int b=0;b<4;++b)bytes[used++]=uint8_t(word>>(b*8));
                if(used==bytes.size()){all.Update(bytes.data(),used);used=0;}
            }
        }
    }
    if(used)all.Update(bytes.data(),used);
    return NLS::Core::Assets::ToHexString(all.Finalize());
}
std::string ComputeDistanceFieldBuildIdentity(const std::string& geometry,const DistanceFieldBuildSettings& settings,DistanceFieldQueryBackend backend)
{
    const std::string key=std::to_string(BuilderVersion)+"/"+geometry+"/"+std::to_string(std::bit_cast<uint32_t>(settings.resolutionScale))+"/"+
        std::to_string(settings.maxResolution)+"/"+std::to_string(settings.twoSided)+"/"+DistanceFieldQueryBackendName(backend);
    return NLS::Core::Assets::ToHexString(NLS::Core::Assets::ComputeSha256(reinterpret_cast<const uint8_t*>(key.data()),key.size()));
}

DistanceFieldBuildResult BuildMeshDistanceField(std::span<const Assets::MeshArtifactData> sections,
    const DistanceFieldBuildSettings& settings,const std::atomic_bool* cancellation,
    const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation,const DistanceFieldBuildOptions* request)
{
    const DistanceFieldBuildOptions defaults;const auto& options=request?*request:defaults;
    DistanceFieldBuildStats stats;stats.backend=DistanceFieldQueryBackendName(options.backend);
    const auto start=Clock::now();struct Publish{DistanceFieldBuildStats& stats;const DistanceFieldBuildOptions& options;Clock::time_point start;
        ~Publish(){stats.totalMs=Ms(start);if(options.stats)*options.stats=stats;}} publish{stats,options,start};
    DistanceFieldBuildResult result;
    const auto cancelled=[&]{return (cancellation && cancellation->load(std::memory_order_relaxed)) || (importCancellation && importCancellation->IsCancellationRequested());};
    const auto fail=[&](const char* message){result.diagnostics.emplace_back(message);return result;};
    if(cancelled())return fail("Distance field build cancelled");
    if(!settings.Enabled()){result.success=true;return result;}
    if(!std::isfinite(settings.resolutionScale) || settings.resolutionScale<.125f || settings.resolutionScale>4 || settings.maxResolution<7 || settings.maxResolution>512)
        return fail("Distance field resolution settings are out of range");
    auto phase=Clock::now();const auto geometryIdentity=ComputeDistanceFieldGeometryIdentity(sections);stats.geometryMs=Ms(phase);
    if(geometryIdentity.empty())return fail("Distance field geometry has invalid positions or indices");
    const auto identity=ComputeDistanceFieldBuildIdentity(geometryIdentity,settings,options.backend);
    const auto cacheDirectory=options.cacheRoot.empty()?std::filesystem::path{}:options.cacheRoot/identity.substr(0,2)/identity;
    auto asset=std::make_shared<MeshDistanceFieldArtifact>();
    if(options.seed && options.seed->buildIdentity==identity && ValidateDistanceFieldMip(options.seed->mips[2]))*asset=*options.seed;
    if(!cacheDirectory.empty() && asset->buildIdentity.empty())
    {
        phase=Clock::now();auto cached=DeserializeDistanceFieldArtifact(Read(cacheDirectory/"main.nldf"));
        if(cached && cached->buildIdentity==identity)*asset=std::move(*cached);stats.cacheMs+=Ms(phase);
    }
    std::unique_lock admission(Admission(),std::defer_lock);
    while(!admission.try_lock_for(std::chrono::milliseconds(25)))if(cancelled())return fail("Distance field build cancelled");
    // Recheck after admission: a matching job may have filled the shared disk cache.
    if(!cacheDirectory.empty() && asset->buildIdentity.empty())
        if(auto cached=DeserializeDistanceFieldArtifact(Read(cacheDirectory/"main.nldf"));cached && cached->buildIdentity==identity)*asset=std::move(*cached);
    std::shared_ptr<const Geometry> geometry;const bool embree=UseEmbree(options.backend);
    for(uint32_t level:{2u,1u,0u})
    {
        if(!(options.mipMask&(1u<<level)) && level!=2)continue;
        if(cancelled())return fail("Distance field build cancelled");
        const bool forceMip=options.forceRebuild && (options.mipMask&(1u<<level));
        if(!forceMip)
        {
            phase=Clock::now();auto& mip=asset->mips[level];
            if(!cacheDirectory.empty() && !ValidateDistanceFieldMip(mip))if(auto cached=DeserializeDistanceFieldMip(Read(cacheDirectory/("mip"+std::to_string(level)+".nldf"))))mip=std::move(*cached);
            stats.cacheMs+=Ms(phase);
            if(!asset->buildIdentity.empty() && ValidateDistanceFieldMip(mip))
            {++stats.cachedMips;if(options.mipReady)options.mipReady(level,*asset);continue;}
        }
        if(!geometry)
        {
            geometry=Prepare(sections,geometryIdentity,embree,stats,cancelled);
            if(!geometry)return fail(cancelled()?"Distance field build cancelled":"Distance field geometry has no non-degenerate triangles or its acceleration structure failed");
            result.diagnostics=geometry->diagnostics;stats.triangles=geometry->triangles.size();
            asset->twoSided=settings.twoSided;asset->buildIdentity=identity;
            V size=geometry->nodes.front().hi-geometry->nodes.front().lo;
            const float longest=(std::max)({size.x,size.y,size.z});
            if(!std::isfinite(longest) || longest<=0)return fail("Distance field geometry bounds are invalid");
            const uint32_t resolution=std::clamp(uint32_t(std::ceil(settings.maxResolution*settings.resolutionScale)),7u,512u);
            const float padding=longest/resolution*2;
            asset->boundsMin=geometry->nodes.front().lo-V(padding,padding,padding);asset->boundsMax=geometry->nodes.front().hi+V(padding,padding,padding);
            size=asset->boundsMax-asset->boundsMin;
            for(uint32_t mip=0;mip<MipCount;++mip)
            {
                V voxel;for(int a=0;a<3;++a){asset->mips[mip].grid[a]=(std::max)(1u,uint32_t(std::ceil(size.array[a]/longest*resolution/float((1u<<mip)*7))));voxel.array[a]=size.array[a]/(asset->mips[mip].grid[a]*7);}
                asset->mips[mip].maxDistance=voxel.Length()*4;
            }
        }
        auto& mip=asset->mips[level];const auto size=asset->boundsMax-asset->boundsMin;
        const V voxel{size.x/(mip.grid[0]*7),size.y/(mip.grid[1]*7),size.z/(mip.grid[2]*7)};
        const float traceMaximum=size.Length()*2,centerRadius=mip.maxDistance+voxel.Length()*3.5f;
        const uint32_t count=mip.grid[0]*mip.grid[1]*mip.grid[2];mip.indirection.assign(count,OutsideBrick);mip.bricks.clear();
        std::vector<QueryStats> local(Pool().Count());
        const auto inside=[&](V p,QueryStats& s)
        {
            if(settings.twoSided)return false;
            uint32_t back=0,remaining=32;
            for(const auto& ray:Rays())
            {if(geometry->Backface(p,ray,traceMaximum,s,embree))++back;--remaining;
             if(!options.referenceQueries){if(back>8)return true;if(back+remaining<=8)return false;}}
            return back>8;
        };
        Pool().Run(count,options.workerCount,[&](uint32_t worker,uint32_t brick)
        {
            if(cancelled())return;auto& s=local[worker];
            const V center=asset->boundsMin+V((brick%mip.grid[0]*7+3.5f)*voxel.x,(brick/mip.grid[0]%mip.grid[1]*7+3.5f)*voxel.y,(brick/(mip.grid[0]*mip.grid[1])*7+3.5f)*voxel.z);
            bool found=false;const auto t=Clock::now();const float d=geometry->Distance(center,options.referenceQueries?std::sqrt(std::numeric_limits<float>::max()):centerRadius,s,found,embree);s.distanceMs+=Ms(t);
            if((options.referenceQueries && d>centerRadius) || (!options.referenceQueries && !found))
            {const auto ts=Clock::now();mip.indirection[brick]=inside(center,s)?InsideBrick:OutsideBrick;s.signMs+=Ms(ts);}else mip.indirection[brick]=0;
        });
        if(cancelled())return fail("Distance field build cancelled");
        std::vector<uint32_t> candidates;candidates.reserve(count);
        for(uint32_t brick=0;brick<count;++brick)if(mip.indirection[brick]==0){mip.indirection[brick]=static_cast<uint32_t>(candidates.size());candidates.push_back(brick);}
        stats.candidateBricks+=candidates.size();stats.constantBricks+=count-candidates.size();
        mip.bricks.resize(candidates.size()*512);
        const std::array<uint32_t,3> lattice{mip.grid[0]*7+1,mip.grid[1]*7+1,mip.grid[2]*7+1};
        const uint64_t latticeCount=uint64_t(lattice[0])*lattice[1]*lattice[2];
        std::unique_ptr<std::atomic_uint16_t[]> shared;
        if(!options.referenceQueries && latticeCount<=16ull*1024*1024)shared=std::make_unique<std::atomic_uint16_t[]>(latticeCount);
        stats.scratchBytes=(std::max)(stats.scratchBytes,uint64_t(count*4)+mip.bricks.size()+candidates.size()*4+(shared?latticeCount*2:0));
        Pool().Run(static_cast<uint32_t>(candidates.size()),options.workerCount,[&](uint32_t worker,uint32_t candidate)
        {
            auto& s=local[worker];const uint32_t brick=candidates[candidate];
            const uint32_t bx=brick%mip.grid[0]*7,by=brick/mip.grid[0]%mip.grid[1]*7,bz=brick/(mip.grid[0]*mip.grid[1])*7;
            uint8_t minimum=255,maximum=0;
            for(uint32_t z=0;z<8;++z)for(uint32_t y=0;y<8;++y)for(uint32_t x=0;x<8;++x)
            {
                if(cancelled())return;std::atomic_uint16_t* cell=shared?&shared[(uint64_t(bz+z)*lattice[1]+by+y)*lattice[0]+bx+x]:nullptr;
                uint16_t encoded=0;bool compute=true;
                if(cell)
                {
                    uint16_t expected=0;
                    if(!cell->compare_exchange_strong(expected,0x200,std::memory_order_acq_rel))
                    {while(expected==0x200){if(cancelled())return;std::this_thread::yield();expected=cell->load(std::memory_order_acquire);}encoded=expected&255;compute=false;++s.shared;}
                }
                if(compute)
                {
                    const V p=asset->boundsMin+V((bx+x)*voxel.x,(by+y)*voxel.y,(bz+z)*voxel.z);bool found=false;++s.samples;
                    auto t=Clock::now();float distance=geometry->Distance(p,options.referenceQueries?std::sqrt(std::numeric_limits<float>::max()):mip.maxDistance,s,found,embree);s.distanceMs+=Ms(t);
                    t=Clock::now();if(inside(p,s))distance=-distance;s.signMs+=Ms(t);
                    encoded=uint16_t(std::lround(std::clamp(distance/mip.maxDistance*.5f+.5f,0.f,1.f)*255));
                    if(cell)cell->store(0x100|encoded,std::memory_order_release);
                }
                mip.bricks[uint64_t(candidate)*512+(z*8+y)*8+x]=uint8_t(encoded);minimum=(std::min)(minimum,uint8_t(encoded));maximum=(std::max)(maximum,uint8_t(encoded));
            }
            if(minimum==255)mip.indirection[brick]=OutsideBrick;else if(maximum==0)mip.indirection[brick]=InsideBrick;
        });
        if(cancelled())return fail("Distance field build cancelled");
        uint32_t compact=0;
        for(uint32_t brick:candidates)if(mip.indirection[brick]!=OutsideBrick && mip.indirection[brick]!=InsideBrick)
        {const auto old=mip.indirection[brick];if(old!=compact)std::copy_n(mip.bricks.data()+uint64_t(old)*512,512,mip.bricks.data()+uint64_t(compact)*512);mip.indirection[brick]=compact++;}
        mip.bricks.resize(uint64_t(compact)*512);
        for(const auto& s:local){stats.distanceQueries+=s.distances;stats.signRays+=s.rays;stats.triangleTests+=s.triangles;stats.sampledVoxels+=s.samples;stats.sharedSamples+=s.shared;stats.distanceMs+=s.distanceMs;stats.signMs+=s.signMs;}
        ++stats.builtMips;
        if(!cacheDirectory.empty())
        {phase=Clock::now();CacheWrite(cacheDirectory/("mip"+std::to_string(level)+".nldf"),SerializeDistanceFieldMip(mip));if(level==2)CacheWrite(cacheDirectory/"main.nldf",SerializeDistanceFieldArtifact(*asset));stats.cacheMs+=Ms(phase);}
        if(options.mipReady)options.mipReady(level,*asset);
    }
    result.asset=std::move(asset);result.success=true;return result;
}
}
