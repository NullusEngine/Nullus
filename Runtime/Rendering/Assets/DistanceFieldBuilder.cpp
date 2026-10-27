#include "Rendering/Assets/DistanceFieldBuilder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <fstream>
#include <sstream>
#include <numeric>
#include <thread>
#include "Assets/ArtifactManifest.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/Assets/SceneImportPipeline.h"

namespace NLS::Render::DistanceField
{
    namespace
    {
        using V = Maths::Vector3;
        V Min(V a, V b) { return {(std::min)(a.x,b.x),(std::min)(a.y,b.y),(std::min)(a.z,b.z)}; }
        V Max(V a, V b) { return {(std::max)(a.x,b.x),(std::max)(a.y,b.y),(std::max)(a.z,b.z)}; }
        float Dot(V a, V b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
        V Cross(V a, V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
        struct Triangle { V a,b,c,normal,lo,hi; };
        struct Node { V lo,hi; uint32_t begin=0,count=0,left=0,right=0; };

        float PointTriangleSquared(V p, const Triangle& t)
        {
            const V ab=t.b-t.a, ac=t.c-t.a, ap=p-t.a;
            const float d1=Dot(ab,ap), d2=Dot(ac,ap);
            if (d1<=0 && d2<=0) return Dot(ap,ap);
            const V bp=p-t.b;
            const float d3=Dot(ab,bp), d4=Dot(ac,bp);
            if (d3>=0 && d4<=d3) return Dot(bp,bp);
            const float vc=d1*d4-d3*d2;
            if (vc<=0 && d1>=0 && d3<=0) { const V q=ap-ab*(d1/(d1-d3)); return Dot(q,q); }
            const V cp=p-t.c;
            const float d5=Dot(ab,cp), d6=Dot(ac,cp);
            if (d6>=0 && d5<=d6) return Dot(cp,cp);
            const float vb=d5*d2-d1*d6;
            if (vb<=0 && d2>=0 && d6<=0) { const V q=ap-ac*(d2/(d2-d6)); return Dot(q,q); }
            const float va=d3*d6-d5*d4;
            if (va<=0 && d4-d3>=0 && d5-d6>=0)
            { const V q=bp-(t.c-t.b)*((d4-d3)/((d4-d3)+(d5-d6))); return Dot(q,q); }
            const float inverse=1/(va+vb+vc);
            const V q=ap-ab*(vb*inverse)-ac*(vc*inverse);
            return Dot(q,q);
        }

        float BoxDistanceSquared(V p, V lo, V hi)
        {
            float sum=0;
            for (int a=0;a<3;++a) { const float d=(std::max)({lo.array[a]-p.array[a],p.array[a]-hi.array[a],0.0f}); sum+=d*d; }
            return sum;
        }

        bool RayBox(V p,V d,V lo,V hi,float maximum)
        {
            float near=0,far=maximum;
            for (int a=0;a<3;++a)
            {
                if (std::abs(d.array[a])<1.e-12f) { if (p.array[a]<lo.array[a] || p.array[a]>hi.array[a]) return false; continue; }
                float t0=(lo.array[a]-p.array[a])/d.array[a],t1=(hi.array[a]-p.array[a])/d.array[a];
                if(t0>t1) std::swap(t0,t1);
                near=(std::max)(near,t0); far=(std::min)(far,t1);
                if(near>far) return false;
            }
            return true;
        }

        class TriangleBvh
        {
        public:
            std::vector<Triangle> triangles;
            std::vector<Node> nodes;
            uint32_t Build(uint32_t begin,uint32_t count)
            {
                const uint32_t index=static_cast<uint32_t>(nodes.size());
                nodes.emplace_back();
                V lo=triangles[begin].lo,hi=triangles[begin].hi;
                for(uint32_t i=1;i<count;++i) { lo=Min(lo,triangles[begin+i].lo); hi=Max(hi,triangles[begin+i].hi); }
                nodes[index]={lo,hi,begin,count};
                if(count<=8) return index;
                const V size=hi-lo;
                const int axis=size.x>size.y ? (size.x>size.z ? 0:2) : (size.y>size.z ? 1:2);
                const uint32_t middle=begin+count/2;
                std::nth_element(triangles.begin()+begin,triangles.begin()+middle,triangles.begin()+begin+count,
                    [axis](const Triangle& a,const Triangle& b) { return a.lo.array[axis]+a.hi.array[axis]<b.lo.array[axis]+b.hi.array[axis]; });
                const auto left=Build(begin,count/2), right=Build(middle,count-count/2);
                nodes[index].count=0; nodes[index].left=left; nodes[index].right=right;
                return index;
            }

            float Distance(V p) const
            {
                float best=std::numeric_limits<float>::max();
                uint32_t stack[128],top=1; stack[0]=0;
                while(top)
                {
                    const auto& node=nodes[stack[--top]];
                    if(BoxDistanceSquared(p,node.lo,node.hi)>best) continue;
                    if(node.count) for(uint32_t i=0;i<node.count;++i) best=(std::min)(best,PointTriangleSquared(p,triangles[node.begin+i]));
                    else
                    {
                        const auto dl=BoxDistanceSquared(p,nodes[node.left].lo,nodes[node.left].hi);
                        const auto dr=BoxDistanceSquared(p,nodes[node.right].lo,nodes[node.right].hi);
                        stack[top++]=dl<dr?node.right:node.left; stack[top++]=dl<dr?node.left:node.right;
                    }
                }
                return std::sqrt(best);
            }

            bool Backface(V p,V direction,float maximum) const
            {
                bool back=false;
                uint32_t stack[128],top=1; stack[0]=0;
                while(top)
                {
                    const auto& node=nodes[stack[--top]];
                    if(!RayBox(p,direction,node.lo,node.hi,maximum)) continue;
                    if(!node.count) { stack[top++]=node.left; stack[top++]=node.right; continue; }
                    for(uint32_t i=0;i<node.count;++i)
                    {
                        const auto& t=triangles[node.begin+i];
                        const V edge1=t.b-t.a,edge2=t.c-t.a,h=Cross(direction,edge2);
                        const float det=Dot(edge1,h);
                        if(std::abs(det)<1.e-12f) continue;
                        const V s=p-t.a;
                        const float u=Dot(s,h)/det;
                        if(u<0 || u>1) continue;
                        const V q=Cross(s,edge1);
                        const float v=Dot(direction,q)/det;
                        if(v<0 || u+v>1) continue;
                        const float distance=Dot(edge2,q)/det;
                        if(distance>1.e-7f && distance<maximum) { maximum=distance; back=Dot(direction,t.normal)>0; }
                    }
                }
                return back;
            }
        };

        std::array<V,32> Directions()
        {
            std::array<V,32> result;
            for(uint32_t i=0;i<result.size();++i)
            {
                const float z=1-2*(float(i)+.5f)/result.size(), r=std::sqrt(1-z*z),phi=float(i)*2.39996323f;
                result[i]={r*std::cos(phi),r*std::sin(phi),z};
            }
            return result;
        }
    }

    DistanceFieldBuildSettings ReadDistanceFieldBuildSettings(
        const std::map<std::string,std::string>& settings,const std::filesystem::path& projectRoot)
    {
        DistanceFieldBuildSettings result;
        if(const auto it=settings.find("DISTANCE_FIELD_GENERATION");it!=settings.end())
        {
            if(it->second=="enabled" || it->second=="1" || it->second=="true") result.mode=GenerationMode::Enabled;
            else if(it->second=="disabled" || it->second=="2" || it->second=="false") result.mode=GenerationMode::Disabled;
        }
        if(const auto it=settings.find("DISTANCE_FIELD_RESOLUTION_SCALE");it!=settings.end())
        { try { result.resolutionScale=std::stof(it->second); } catch(...) { result.resolutionScale=0; } }
        if(const auto it=settings.find("DISTANCE_FIELD_TWO_SIDED");it!=settings.end()) result.twoSided=it->second=="true" || it->second=="1";
        std::error_code ec;
        if(!projectRoot.empty() && std::filesystem::is_directory(projectRoot,ec))
        {
            std::vector<std::filesystem::path> files;
            for(std::filesystem::directory_iterator it(projectRoot,ec),end;!ec && it!=end;it.increment(ec))
                if(it->path().extension()==".nullus") files.push_back(it->path());
            std::sort(files.begin(),files.end());
            if(!files.empty())
            {
                std::ifstream stream(files.front()); std::string line;
                while(std::getline(stream,line))
                {
                    line.erase(std::remove_if(line.begin(),line.end(),[](unsigned char ch){return std::isspace(ch)!=0;}),line.end());
                    if(line=="generate_mesh_distance_fields=true" || line=="generate_mesh_distance_fields=1") result.projectEnabled=true;
                }
            }
        }
        return result;
    }

    bool AppendDistanceFieldPayloads(const std::string& key,std::span<const Assets::MeshArtifactData> sections,
        const DistanceFieldBuildSettings& settings,std::vector<NLS::Core::Assets::ArtifactPayload>& payloads,
        std::vector<std::string>& diagnostics,const std::atomic_bool* cancellation,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation)
    {
        auto result=BuildMeshDistanceField(sections,settings,cancellation,importCancellation);
        diagnostics.insert(diagnostics.end(),result.diagnostics.begin(),result.diagnostics.end());
        if(!result.success) return false;
        if(!result.asset) return true;
        auto& asset=*result.asset;
        for(uint32_t i=0;i<2;++i)
        {
            asset.mipPaths[i]=DistanceFieldSubAssetKey(key,i);
            auto bytes=SerializeDistanceFieldMip(asset.mips[i]);
            if(bytes.empty()) return false;
            payloads.push_back({asset.mipPaths[i],NLS::Core::Assets::ArtifactType::MeshDistanceField,
                "mesh-distance-field-mip",key+" Distance Field Mip "+std::to_string(i),
                std::filesystem::path("distancefields")/(asset.buildIdentity+"-mip"+std::to_string(i)+".nldf"),std::move(bytes)});
        }
        auto bytes=SerializeDistanceFieldArtifact(asset);
        if(bytes.empty()) return false;
        payloads.push_back({DistanceFieldSubAssetKey(key),NLS::Core::Assets::ArtifactType::MeshDistanceField,
            "mesh-distance-field",key+" Distance Field",std::filesystem::path("distancefields")/(asset.buildIdentity+"-main.nldf"),std::move(bytes)});
        return true;
    }

    bool AppendDistanceFieldModelPayloads(const Assets::ImportedScene& scene,
        const std::map<std::string,Assets::MeshArtifactData>& meshes,const DistanceFieldBuildSettings& settings,
        std::vector<NLS::Core::Assets::ArtifactPayload>& payloads,std::vector<std::string>& diagnostics,
        const std::atomic_bool* cancellation,const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation)
    {
        if(!settings.Enabled()) return true;
        std::map<std::string,std::vector<Assets::MeshArtifactData>> groups;
        for(const auto& [key,mesh]:meshes)
        {
            const std::string source=key.starts_with("mesh:")?key.substr(5):key;
            const auto primitive=Assets::ParsePrimitiveMeshSourceKey(source);
            const std::string logical=primitive?primitive->first:source;
            const bool deformed=std::any_of(scene.meshes.begin(),scene.meshes.end(),[&](const auto& m)
                { return m.sourceKey==logical && m.morphTargetCount!=0; }) ||
                std::any_of(scene.nodes.begin(),scene.nodes.end(),[&](const auto& n)
                { return n.meshKey==logical && !n.skinKey.empty(); });
            if(deformed) { diagnostics.push_back("Distance field skipped deformed mesh "+key); continue; }
            if(mesh.materialIndex<scene.materials.size() && scene.materials[mesh.materialIndex].alphaMode=="BLEND") continue;
            if(!AppendDistanceFieldPayloads(key,{&mesh,1},settings,payloads,diagnostics,cancellation,importCancellation)) return false;
            if(primitive) groups["mesh:"+logical].push_back(mesh);
        }
        for(const auto& [key,sections]:groups)
            if(!AppendDistanceFieldPayloads(key,sections,settings,payloads,diagnostics,cancellation,importCancellation)) return false;
        return true;
    }

    DistanceFieldBuildResult BuildMeshDistanceField(std::span<const Assets::MeshArtifactData> sections,
        const DistanceFieldBuildSettings& settings,const std::atomic_bool* cancellation,
        const NLS::Core::Assets::IArtifactWriteCancellation* importCancellation)
    {
        DistanceFieldBuildResult result;
        const auto cancelled=[&]() { return (cancellation && cancellation->load(std::memory_order_relaxed)) ||
            (importCancellation && importCancellation->IsCancellationRequested()); };
        if(cancelled()) { result.diagnostics.push_back("Distance field build cancelled"); return result; }
        if(!settings.Enabled()) { result.success=true; return result; }
        if(!std::isfinite(settings.resolutionScale) || settings.resolutionScale<.125f ||
            settings.resolutionScale>4 || settings.maxResolution<7 || settings.maxResolution>512)
        { result.diagnostics.push_back("Distance field resolution settings are out of range"); return result; }
        TriangleBvh bvh;
        uint32_t degenerate=0;
        std::vector<uint8_t> identity;
        const auto append=[&](uint32_t word) { for(int i=0;i<4;++i) identity.push_back(uint8_t(word>>(8*i))); };
        append(BuilderVersion); append(std::bit_cast<uint32_t>(settings.resolutionScale));
        append(settings.maxResolution); append(settings.twoSided);
        std::map<std::array<uint32_t,6>,uint32_t> edges;
        for(const auto& mesh:sections)
        {
            if(mesh.indices.size()%3) { result.diagnostics.push_back("Distance field mesh index count is not triangular"); return result; }
            for(size_t i=0;i<mesh.indices.size();i+=3)
            {
                Triangle triangle;
                V* positions[]={&triangle.a,&triangle.b,&triangle.c};
                for(int c=0;c<3;++c)
                {
                    if(mesh.indices[i+c]>=mesh.vertices.size()) { result.diagnostics.push_back("Distance field mesh index is out of range"); return result; }
                    for(int a=0;a<3;++a)
                    {
                        const float v=mesh.vertices[mesh.indices[i+c]].position[a];
                        if(!std::isfinite(v)) { result.diagnostics.push_back("Distance field geometry contains non-finite positions"); return result; }
                        positions[c]->array[a]=v; append(std::bit_cast<uint32_t>(v==0?0.0f:v));
                    }
                }
                triangle.normal=Cross(triangle.b-triangle.a,triangle.c-triangle.a);
                if(Dot(triangle.normal,triangle.normal)<1.e-20f) { ++degenerate; continue; }
                triangle.lo=Min(Min(triangle.a,triangle.b),triangle.c);
                triangle.hi=Max(Max(triangle.a,triangle.b),triangle.c);
                for(int e=0;e<3;++e)
                {
                    std::array<uint32_t,3> a,b;
                    for(int axis=0;axis<3;++axis) { a[axis]=std::bit_cast<uint32_t>(positions[e]->array[axis]==0?0.0f:positions[e]->array[axis]); b[axis]=std::bit_cast<uint32_t>(positions[(e+1)%3]->array[axis]==0?0.0f:positions[(e+1)%3]->array[axis]); }
                    if(b<a) std::swap(a,b);
                    ++edges[{a[0],a[1],a[2],b[0],b[1],b[2]}];
                }
                bvh.triangles.push_back(triangle);
            }
        }
        if(bvh.triangles.empty()) { result.diagnostics.push_back("Distance field geometry has no non-degenerate triangles"); return result; }
        if(degenerate) result.diagnostics.push_back("Distance field ignored "+std::to_string(degenerate)+" degenerate triangles");
        if(std::any_of(edges.begin(),edges.end(),[](const auto& e){return e.second!=2;}))
            result.diagnostics.push_back("Distance field source is open or non-manifold; signed interior is approximate");
        bvh.nodes.reserve(bvh.triangles.size()*2);
        bvh.Build(0,static_cast<uint32_t>(bvh.triangles.size()));
        auto asset=std::make_shared<MeshDistanceFieldArtifact>();
        asset->twoSided=settings.twoSided;
        asset->buildIdentity=NLS::Core::Assets::ToHexString(NLS::Core::Assets::ComputeSha256(identity.data(),identity.size()));
        V size=bvh.nodes[0].hi-bvh.nodes[0].lo;
        const float longest=(std::max)({size.x,size.y,size.z});
        const uint32_t resolution=std::clamp(uint32_t(std::ceil(settings.maxResolution*settings.resolutionScale)),7u,512u);
        const float padding=longest/float(resolution)*2;
        asset->boundsMin=bvh.nodes[0].lo-V(padding,padding,padding);
        asset->boundsMax=bvh.nodes[0].hi+V(padding,padding,padding);
        size=asset->boundsMax-asset->boundsMin;
        const auto directions=Directions();
        const float traceMaximum=size.Length()*2;
        const auto inside=[&](V point)
        {
            if(settings.twoSided) return false;
            uint32_t back=0;
            for(auto direction:directions) if(bvh.Backface(point,direction,traceMaximum)) ++back;
            return back>directions.size()/4;
        };
        for(uint32_t level=0;level<MipCount;++level)
        {
            auto& mip=asset->mips[level];
            V voxel;
            for(int a=0;a<3;++a)
            {
                mip.grid[a]=(std::max)(1u,uint32_t(std::ceil(size.array[a]/longest*resolution/float((1u<<level)*BrickIntervals))));
                voxel.array[a]=size.array[a]/(mip.grid[a]*BrickIntervals);
            }
            mip.maxDistance=voxel.Length()*4;
            const uint32_t count=mip.grid[0]*mip.grid[1]*mip.grid[2];
            mip.indirection.resize(count);
            std::vector<std::array<uint8_t,512>> temporary(count);
            std::atomic_uint32_t next{0};
            const auto work=[&]()
            {
                for(uint32_t brick;(brick=next.fetch_add(1))<count;)
                {
                    if(cancelled()) return;
                    const uint32_t bx=brick%mip.grid[0],by=brick/mip.grid[0]%mip.grid[1],bz=brick/(mip.grid[0]*mip.grid[1]);
                    const V origin=asset->boundsMin+V(bx*7*voxel.x,by*7*voxel.y,bz*7*voxel.z);
                    const V center=origin+voxel*3.5f;
                    const float centerDistance=bvh.Distance(center);
                    if(centerDistance>mip.maxDistance+voxel.Length()*3.5f)
                    { mip.indirection[brick]=inside(center)?InsideBrick:OutsideBrick; continue; }
                    uint8_t minimum=255,maximum=0;
                    for(uint32_t z=0;z<8;++z) for(uint32_t y=0;y<8;++y) for(uint32_t x=0;x<8;++x)
                    {
                        const V p=origin+V(x*voxel.x,y*voxel.y,z*voxel.z);
                        float distance=bvh.Distance(p);
                        if(inside(p)) distance=-distance;
                        const uint8_t encoded=uint8_t(std::lround(std::clamp(distance/mip.maxDistance*.5f+.5f,0.0f,1.0f)*255));
                        temporary[brick][(z*8+y)*8+x]=encoded;
                        minimum=(std::min)(minimum,encoded); maximum=(std::max)(maximum,encoded);
                    }
                    mip.indirection[brick]=minimum==255?OutsideBrick:(maximum==0?InsideBrick:0);
                }
            };
            // Dedicated bounded workers avoid waiting for nested jobs in the game TaskGraph.
            const uint32_t workers=(std::min)({8u,(std::max)(1u,std::thread::hardware_concurrency()),count});
            std::vector<std::thread> threads;
            for(uint32_t i=1;i<workers;++i) threads.emplace_back(work);
            work(); for(auto& thread:threads) thread.join();
            if(cancelled()) { result.diagnostics.push_back("Distance field build cancelled"); return result; }
            for(uint32_t i=0;i<count;++i) if(mip.indirection[i]!=OutsideBrick && mip.indirection[i]!=InsideBrick)
            {
                mip.indirection[i]=mip.BrickCount();
                mip.bricks.insert(mip.bricks.end(),temporary[i].begin(),temporary[i].end());
            }
        }
        result.asset=std::move(asset); result.success=true;
        return result;
    }
}
