#include "Rendering/DistanceField/DistanceFieldSceneData.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include "Math/Vector4.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"

namespace NLS::Render::DistanceField
{
    namespace
    {
        using V=Maths::Vector3;
        V Point(const Maths::Matrix4& matrix,V p) { return V(matrix*Maths::Vector4(p.x,p.y,p.z,1)); }
        float Scale(const Maths::Matrix4& matrix)
        {
            // Smallest singular value, including mirrored and sheared transforms.
            double gram[3][3]{};
            for(int i=0;i<3;++i) for(int j=0;j<3;++j) for(int k=0;k<3;++k)
                gram[i][j]+=double(matrix.data[i*4+k])*matrix.data[j*4+k];
            for(int sweep=0;sweep<12;++sweep) for(int p=0;p<2;++p) for(int q=p+1;q<3;++q)
            {
                if(std::abs(gram[p][q])<1.e-16) continue;
                const double angle=.5*std::atan2(2*gram[p][q],gram[q][q]-gram[p][p]);
                const double c=std::cos(angle),s=std::sin(angle),a=gram[p][p],b=gram[q][q],d=gram[p][q];
                gram[p][p]=c*c*a-2*c*s*d+s*s*b;
                gram[q][q]=s*s*a+2*c*s*d+c*c*b; gram[p][q]=gram[q][p]=0;
                for(int r=0;r<3;++r) if(r!=p && r!=q)
                { const auto x=gram[r][p],y=gram[r][q]; gram[r][p]=gram[p][r]=c*x-s*y; gram[r][q]=gram[q][r]=s*x+c*y; }
            }
            const double value=(std::min)({gram[0][0],gram[1][1],gram[2][2]});
            return value>1.e-12 && std::isfinite(value)?static_cast<float>(std::sqrt(value)):0;
        }
        bool Same(const DistanceFieldObjectInput& a,const DistanceFieldObjectInput& b)
        { return a.sceneId==b.sceneId && a.index==b.index && a.generation==b.generation &&
            a.asset==b.asset && a.enabled==b.enabled && a.mostlyStatic==b.mostlyStatic &&
            std::memcmp(a.localToWorld.data,b.localToWorld.data,sizeof(a.localToWorld.data))==0; }
        bool InvertAffine(const Maths::Matrix4& source,Maths::Matrix4& inverse)
        {
            for(auto value:source.data) if(!std::isfinite(value)) return false;
            const auto& m=source.data;
            if(std::abs(m[12])+std::abs(m[13])+std::abs(m[14])>1.e-6f || std::abs(m[15]-1)>1.e-6f) return false;
            const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
            const double determinant=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
            if(determinant==0 || !std::isfinite(determinant)) return false;
            const double entries[9]={e*i-f*h,c*h-b*i,b*f-c*e,f*g-d*i,a*i-c*g,c*d-a*f,d*h-e*g,b*g-a*h,a*e-b*d};
            inverse=Maths::Matrix4::Identity;
            for(int row=0;row<3;++row)
            {
                for(int column=0;column<3;++column) inverse.data[row*4+column]=static_cast<float>(entries[row*3+column]/determinant);
                inverse.data[row*4+3]=-(inverse.data[row*4]*m[3]+inverse.data[row*4+1]*m[7]+inverse.data[row*4+2]*m[11]);
            }
            for(auto value:inverse.data) if(!std::isfinite(value)) return false;
            return true;
        }
    }
    std::vector<DistanceFieldObjectInput> GroupDistanceFieldObjects(std::vector<DistanceFieldObjectInput> objects)
    {
        std::map<std::string,std::vector<size_t>> groups;
        for(size_t i=0;i<objects.size();++i)
        {
            const auto source=FindMeshDistanceFieldSource(objects[i].meshArtifactPath);
            if(!objects[i].enabled || !source || source->groupMainPath.empty()) continue;
            std::string key=std::to_string(objects[i].sceneId)+"|"+source->groupMainPath+"|";
            key.append(reinterpret_cast<const char*>(objects[i].localToWorld.data),sizeof(objects[i].localToWorld.data));
            groups[key].push_back(i);
        }
        std::vector<bool> consumed(objects.size());
        std::vector<DistanceFieldObjectInput> result;
        for(const auto& [key,indices]:groups)
        {
            const auto source=FindMeshDistanceFieldSource(objects[indices.front()].meshArtifactPath);
            if(!source || source->groupMembers.empty()) continue;
            const bool complete=std::all_of(source->groupMembers.begin(),source->groupMembers.end(),[&](const auto& path)
            { return std::any_of(indices.begin(),indices.end(),[&](size_t i)
                {
                    const auto candidate=std::filesystem::path(objects[i].meshArtifactPath).lexically_normal().generic_string();
                    return candidate==path || (!std::filesystem::path(path).is_absolute() &&
                        NLS::Core::Assets::TryMakePortableContentArtifactPath(candidate)==path);
                }); });
            if(!complete) continue;
            auto group=objects[indices.front()]; group.meshArtifactPath=source->groupMainPath;
            group.mostlyStatic=true;
            for(auto i:indices) { consumed[i]=true; group.mostlyStatic=group.mostlyStatic && objects[i].mostlyStatic; }
            result.push_back(std::move(group));
        }
        for(size_t i=0;i<objects.size();++i) if(!consumed[i]) result.push_back(std::move(objects[i]));
        return result;
    }

    void DistanceFieldSceneData::Synchronize(std::span<const DistanceFieldObjectInput> inputs)
    {
        std::vector<DistanceFieldObject> next;
        next.reserve(inputs.size());
        using Key = std::tuple<uint64_t,uint32_t,uint32_t>;
        std::map<Key,const DistanceFieldObject*> old;
        for(const auto& object:m_objects) old[{object.input.sceneId,object.input.index,object.input.generation}]=&object;
        std::vector<DistanceFieldDirtyBounds> dirty;
        for(const auto& input:inputs)
        {
            if(!input.enabled || !input.asset || (m_sceneId && input.sceneId!=m_sceneId)) continue;
            const float scale=Scale(input.localToWorld);
            if(!scale) continue;
            const Key key{input.sceneId,input.index,input.generation};
            const auto found=old.find(key);
            if(found!=old.end() && Same(found->second->input,input))
            { next.push_back(*found->second); old.erase(found); continue; }
            if(found!=old.end()) { dirty.push_back({found->second->boundsMin,found->second->boundsMax}); old.erase(found); }
            DistanceFieldObject object; object.input=input; object.distanceScale=scale;
            if(!InvertAffine(input.localToWorld,object.worldToLocal)) continue;
            object.boundsMin=V(std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max());
            object.boundsMax=object.boundsMin*-1;
            for(int corner=0;corner<8;++corner)
            {
                const V local{corner&1?input.asset->boundsMax.x:input.asset->boundsMin.x,
                    corner&2?input.asset->boundsMax.y:input.asset->boundsMin.y,
                    corner&4?input.asset->boundsMax.z:input.asset->boundsMin.z};
                const auto p=Point(input.localToWorld,local);
                for(int a=0;a<3;++a) { object.boundsMin.array[a]=(std::min)(object.boundsMin.array[a],p.array[a]); object.boundsMax.array[a]=(std::max)(object.boundsMax.array[a],p.array[a]); }
            }
            bool finite=true; for(auto value:object.worldToLocal.data) finite=finite && std::isfinite(value);
            if(!finite) continue;
            dirty.push_back({object.boundsMin,object.boundsMax}); next.push_back(std::move(object));
        }
        for(const auto& [key,object]:old) dirty.push_back({object->boundsMin,object->boundsMax});
        if(!dirty.empty()) { ++m_revision; m_changes.push_back({m_revision,std::move(dirty)}); if(m_changes.size()>128) m_changes.pop_front(); }
        m_objects=std::move(next);
    }
    bool DistanceFieldSceneData::ChangesSince(uint64_t revision,std::vector<DistanceFieldDirtyBounds>& bounds) const
    {
        bounds.clear();
        if(!revision || (!m_changes.empty() && revision+1<m_changes.front().revision)) return false;
        for(const auto& change:m_changes) if(change.revision>revision) bounds.insert(bounds.end(),change.bounds.begin(),change.bounds.end());
        return true;
    }
    DistanceFieldSample DistanceFieldSceneData::SampleReference(const V& p,uint32_t mip) const
    {
        DistanceFieldSample result; result.distance=std::numeric_limits<float>::max();
        for(const auto& object:m_objects)
        {
            auto sample=SampleMeshDistanceField(*object.input.asset,Point(object.worldToLocal,p),mip);
            sample.distance*=object.distanceScale; sample.voxelSize*=object.distanceScale;
            if(sample.valid && sample.distance<result.distance)
            {
                result=sample;
                const auto& m=object.worldToLocal.data;
                result.gradient={m[0]*sample.gradient.x+m[4]*sample.gradient.y+m[8]*sample.gradient.z,
                    m[1]*sample.gradient.x+m[5]*sample.gradient.y+m[9]*sample.gradient.z,
                    m[2]*sample.gradient.x+m[6]*sample.gradient.y+m[10]*sample.gradient.z};
                result.gradient.Normalise();
            }
        }
        return result;
    }

    std::vector<DistanceFieldDirtyPage> DistanceFieldViewState::Update(const DistanceFieldSceneData& scene,V camera,bool force)
    {
        std::vector<DistanceFieldDirtyBounds> bounds;
        const bool full=force || !m_revision || !scene.ChangesSince(m_revision,bounds);
        std::vector<DistanceFieldDirtyPage> pages;
        const float radius[4]={16,64,256,1024};
        for(uint32_t level=0;level<4;++level)
        {
            auto& clip=clipmaps[level];
            const float voxel=radius[level]*2/(GridResolution*BrickIntervals), pageSize=voxel*BrickIntervals;
            std::array<int32_t,3> origin;
            for(int a=0;a<3;++a) origin[a]=static_cast<int32_t>(std::floor(camera.array[a]/pageSize))-int32_t(GridResolution/2);
            const auto oldOrigin=clip.origin;
            for(int a=0;a<3;++a) clip.min.array[a]=origin[a]*pageSize;
            clip.voxelSize=voxel; clip.origin=origin;
            for(uint32_t z=0;z<GridResolution;++z) for(uint32_t y=0;y<GridResolution;++y) for(uint32_t x=0;x<GridResolution;++x)
            {
                const int32_t coordinate[3]={origin[0]+int32_t(x),origin[1]+int32_t(y),origin[2]+int32_t(z)};
                V minimum{coordinate[0]*pageSize,coordinate[1]*pageSize,coordinate[2]*pageSize};
                bool changed=full;
                for(int a=0;a<3;++a) changed=changed || coordinate[a]<oldOrigin[a] || coordinate[a]>=oldOrigin[a]+int32_t(GridResolution);
                if(!changed) for(const auto& box:bounds)
                {
                    bool overlap=true;
                    // The truncated field band extends beyond the geometric AABB.
                    for(int a=0;a<3;++a) overlap=overlap && minimum.array[a]<=box.max.array[a]+pageSize && minimum.array[a]+pageSize>=box.min.array[a]-pageSize;
                    if(overlap) { changed=true; break; }
                }
                if(!changed) continue;
                uint32_t wrapped[3];
                for(int a=0;a<3;++a) wrapped[a]=uint32_t((coordinate[a]%int32_t(GridResolution)+int32_t(GridResolution))%int32_t(GridResolution));
                const uint32_t index=(wrapped[2]*GridResolution+wrapped[1])*GridResolution+wrapped[0];
                for(uint32_t cache=0;cache<2;++cache)
                    pages.push_back({(cache*4+level)*PagesPerClipmap+index,level,cache,0,minimum,voxel});
            }
        }
        m_revision=scene.Revision(); return pages;
    }
}
