#include "Rendering/Assets/DistanceFieldBuildService.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <unordered_map>
#include "Assets/NativeArtifactContainer.h"
#include "Json/json.hpp"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/Assets/SceneImportPipeline.h"

namespace NLS::Render::DistanceField
{
namespace
{
using Json = nlohmann::json;
using namespace NLS::Core::Assets;
std::string Digest(std::string_view text)
{return ToHexString(ComputeSha256(reinterpret_cast<const uint8_t*>(text.data()),text.size()));}
Json SourceJson(const DistanceFieldSourceDescription& source)
{
    Json j; j["version"]=1; j["meshes"]=Json::array();j["groups"]=Json::array();
    for(const auto& m:source.meshes)j["meshes"].push_back({{"key",m.key},{"logical",m.logicalKey},{"geometry",m.geometryIdentity},{"participates",m.participates}});
    for(const auto& g:source.groups)j["groups"].push_back({{"key",g.key},{"geometry",g.geometryIdentity},{"members",g.members}});
    return j;
}
void Sum(DistanceFieldBuildStats& a,const DistanceFieldBuildStats& b)
{
    a.triangles+=b.triangles;a.builtMips+=b.builtMips;a.cachedMips+=b.cachedMips;a.bvhBuilds+=b.bvhBuilds;
    a.distanceQueries+=b.distanceQueries;a.signRays+=b.signRays;a.triangleTests+=b.triangleTests;
    a.sampledVoxels+=b.sampledVoxels;a.sharedSamples+=b.sharedSamples;a.candidateBricks+=b.candidateBricks;a.constantBricks+=b.constantBricks;
    a.scratchBytes=(std::max)(a.scratchBytes,b.scratchBytes);a.geometryMs+=b.geometryMs;a.bvhMs+=b.bvhMs;a.distanceMs+=b.distanceMs;
    a.signMs+=b.signMs;a.cacheMs+=b.cacheMs;a.totalMs+=b.totalMs;a.backend=b.backend;
}
std::vector<uint8_t> ReadCache(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return {};
    const auto size=file.tellg();if(size<=0 || size>std::streamoff(1024ull*1024*1024))return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);return file?bytes:std::vector<uint8_t>{};
}
std::shared_ptr<MeshDistanceFieldArtifact> Cached(const std::string& identity,const DistanceFieldBuildOptions& options,uint32_t level)
{
    if(options.cacheRoot.empty() || options.forceRebuild)return {};
    const auto root=options.cacheRoot/identity.substr(0,2)/identity;
    auto asset=DeserializeDistanceFieldArtifact(ReadCache(root/"main.nldf"));
    if(!asset || asset->buildIdentity!=identity)return {};
    if(level!=2)
    {auto mip=DeserializeDistanceFieldMip(ReadCache(root/("mip"+std::to_string(level)+".nldf")));if(!mip)return {};asset->mips[level]=std::move(*mip);}
    return std::make_shared<MeshDistanceFieldArtifact>(std::move(*asset));
}
struct Task
{
    std::vector<std::string> keys,members;
    std::string geometry,identity;
    bool fallback=false;
    uint32_t available=0;
    std::shared_ptr<MeshDistanceFieldArtifact> seed;
};
std::shared_ptr<MeshDistanceFieldArtifact> CoarseOnly(const MeshDistanceFieldArtifact& asset)
{
    auto result=std::make_shared<MeshDistanceFieldArtifact>();
    result->boundsMin=asset.boundsMin;result->boundsMax=asset.boundsMax;result->twoSided=asset.twoSided;result->buildIdentity=asset.buildIdentity;
    result->mips[2]=asset.mips[2];result->mipPaths=asset.mipPaths;
    for(uint32_t mip=0;mip<2;++mip){result->mips[mip].grid=asset.mips[mip].grid;result->mips[mip].maxDistance=asset.mips[mip].maxDistance;}
    return result;
}
std::vector<ArtifactPayload> Payloads(const Task& task,const MeshDistanceFieldArtifact& asset,uint32_t available,uint32_t level)
{
    std::vector<ArtifactPayload> result;
    for(const auto& key:task.keys)
    {
        auto main=CoarseOnly(asset);
        // Availability lives in the manifest. Keep the mandatory artifact stable
        // while detail payloads arrive so the GPU reuses its resident coarse mip.
        for(uint32_t i=0;i<2;++i)main->mipPaths[i]=DistanceFieldSubAssetKey(key,i);
        if(level!=2)
            result.push_back({DistanceFieldSubAssetKey(key,level),ArtifactType::MeshDistanceField,"mesh-distance-field-mip",
                key+" Distance Field Mip "+std::to_string(level),std::filesystem::path("distancefields")/(asset.buildIdentity+"-mip"+std::to_string(level)+".nldf"),SerializeDistanceFieldMip(asset.mips[level])});
        result.push_back({DistanceFieldSubAssetKey(key),ArtifactType::MeshDistanceField,"mesh-distance-field",key+" Distance Field",
            std::filesystem::path("distancefields")/(asset.buildIdentity+"-main.nldf"),SerializeDistanceFieldArtifact(*main)});
    }
    return result;
}
}

DistanceFieldSourceDescription DescribeDistanceFieldSource(const Assets::ImportedScene& scene,
    const std::map<std::string,Assets::MeshArtifactData>& meshes)
{
    DistanceFieldSourceDescription result;
    std::map<std::string,std::vector<Assets::MeshArtifactData>> groups;
    std::map<std::string,std::vector<std::string>> members;
    for(const auto& [key,mesh]:meshes)
    {
        const auto name=key.starts_with("mesh:")?key.substr(5):key;
        const auto primitive=Assets::ParsePrimitiveMeshSourceKey(name);const auto logical=primitive?primitive->first:name;
        const bool deformed=std::any_of(scene.meshes.begin(),scene.meshes.end(),[&](const auto& m){return m.sourceKey==logical && m.morphTargetCount;}) ||
            std::any_of(scene.nodes.begin(),scene.nodes.end(),[&](const auto& n){return n.meshKey==logical && !n.skinKey.empty();});
        const bool participates=!deformed && (mesh.materialIndex>=scene.materials.size() || scene.materials[mesh.materialIndex].alphaMode!="BLEND");
        result.meshes.push_back({key,"mesh:"+logical,ComputeDistanceFieldGeometryIdentity({&mesh,1}),participates});
        if(participates && primitive){groups["mesh:"+logical].push_back(mesh);members["mesh:"+logical].push_back(key);}
    }
    for(const auto& [key,sections]:groups)result.groups.push_back({key,ComputeDistanceFieldGeometryIdentity(sections),members[key]});
    result.identity=Digest(SourceJson(result).dump());return result;
}
std::vector<uint8_t> SerializeDistanceFieldSource(const DistanceFieldSourceDescription& source)
{
    auto json=SourceJson(source);json["identity"]=Digest(json.dump());const auto text=json.dump();
    NativeArtifactMetadata metadata;metadata.artifactType=ArtifactType::MeshDistanceFieldSource;metadata.schemaName="mesh-distance-field-source";metadata.schemaVersion=1;
    return WriteNativeArtifactContainer(std::move(metadata),std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),text.size()));
}
std::optional<DistanceFieldSourceDescription> DeserializeDistanceFieldSource(const std::vector<uint8_t>& bytes)
{
    const auto view=ReadNativeArtifactContainerView(bytes,ArtifactType::MeshDistanceFieldSource,1);if(!view)return {};
    try
    {
        auto j=Json::parse(view->payloadData,view->payloadData+view->payloadSize);
        if(j.at("version").get<uint32_t>()!=1)return {};
        DistanceFieldSourceDescription source;source.identity=j.at("identity").get<std::string>();j.erase("identity");
        if(source.identity!=Digest(j.dump()) || j.at("meshes").size()>1000000 || j.at("groups").size()>1000000)return {};
        std::unordered_map<std::string,bool> keys;
        for(const auto& m:j.at("meshes"))
        {
            DistanceFieldSourceMesh mesh{m.at("key").get<std::string>(),m.at("logical").get<std::string>(),m.at("geometry").get<std::string>(),m.at("participates").get<bool>()};
            if(mesh.key.empty() || !keys.emplace(mesh.key,mesh.participates).second)return {};
            source.meshes.push_back(std::move(mesh));
        }
        for(const auto& g:j.at("groups"))
        {
            DistanceFieldSourceGroup group{g.at("key").get<std::string>(),g.at("geometry").get<std::string>(),g.at("members").get<std::vector<std::string>>()};
            if(group.key.empty() || group.members.empty())return {};
            for(const auto& member:group.members)if(!keys.contains(member) || !keys[member])return {};
            source.groups.push_back(std::move(group));
        }
        return source;
    }
    catch(const Json::exception&){return {};}
}
ArtifactPayload DistanceFieldSourcePayload(const DistanceFieldSourceDescription& source)
{
    return {DistanceFieldSourceKey,ArtifactType::MeshDistanceFieldSource,"mesh-distance-field-source","Distance Field Build Input",
        std::filesystem::path("distancefields")/(source.identity+"-source.ndfi"),SerializeDistanceFieldSource(source)};
}
std::string DistanceFieldSourceManifestIdentity(const ArtifactManifest& manifest)
{
    std::map<std::string,std::string> entries;
    for(const auto& a:manifest.subAssets)
        if(a.artifactType==ArtifactType::Mesh || a.artifactType==ArtifactType::MeshDistanceFieldSource ||
            a.artifactType==ArtifactType::Skin || a.artifactType==ArtifactType::MorphTarget || a.artifactType==ArtifactType::SkeletalMesh)
            entries[a.subAssetKey]=a.contentHash;
    std::string value=manifest.sourceAssetId.ToString()+"/"+manifest.targetPlatform;
    for(const auto& [key,hash]:entries)value+="\n"+key+"="+hash;
    return Digest(value);
}

DistanceFieldModelBuildResult DistanceFieldBuildService::Build(const DistanceFieldSourceDescription& source,
    const DistanceFieldBuildSettings& settings,const DistanceFieldModelBuildOptions& options,
    const std::atomic_bool* cancellation,const IArtifactWriteCancellation* importCancellation)
{
    DistanceFieldModelBuildResult result;
    const auto cancelled=[&]{return (cancellation && cancellation->load(std::memory_order_relaxed)) || (importCancellation && importCancellation->IsCancellationRequested());};
    if(!settings.Enabled()){result.success=true;return result;}
    if(!options.loadMesh){result.diagnostics.push_back("Distance field mesh provider is missing");return result;}
    std::vector<Task> tasks;
    std::unordered_map<std::string,bool> grouped;
    for(const auto& group:source.groups)
    {
        Task task;task.keys={group.key};task.members=group.members;task.geometry=group.geometryIdentity;
        if(group.members.size()==1){task.keys.push_back(group.members.front());grouped[group.members.front()]=true;}
        tasks.push_back(std::move(task));
    }
    for(const auto& mesh:source.meshes)if(mesh.participates && !grouped.contains(mesh.key))
    {
        const bool isFallback=std::any_of(source.groups.begin(),source.groups.end(),[&](const auto& g){return g.key==mesh.logicalKey;});
        if(!isFallback || options.includeFallbacks)tasks.push_back({{mesh.key},{mesh.key},mesh.geometryIdentity,{},isFallback,0});
    }
    // Geometry may be shared by different logical meshes as well as instances.
    // Even a forced build computes each unique triangle stream once.
    std::unordered_map<std::string,size_t> geometryTasks;
    std::vector<Task> unique;
    for(auto& task:tasks)
    {
        const auto [it,inserted]=geometryTasks.emplace(task.geometry,unique.size());
        if(inserted)unique.push_back(std::move(task));
        else
        {
            auto& shared=unique[it->second];
            shared.keys.insert(shared.keys.end(),task.keys.begin(),task.keys.end());
            if(shared.fallback && !task.fallback){shared.members=std::move(task.members);shared.fallback=false;}
        }
    }
    tasks=std::move(unique);
    std::stable_sort(tasks.begin(),tasks.end(),[](const Task& a,const Task& b){return a.fallback<b.fallback;});
    for(auto& task:tasks)
    {
        task.identity=ComputeDistanceFieldBuildIdentity(task.geometry,settings,options.kernel.backend);
        if(options.kernel.forceRebuild || !options.previousManifest || !options.readArtifact)continue;
        uint32_t available=7;
        for(const auto& key:task.keys)
        {
            const auto* entry=options.previousManifest->FindSubAsset(DistanceFieldSubAssetKey(key));
            const auto existing=entry?DeserializeDistanceFieldArtifact(options.readArtifact(*entry)):std::nullopt;
            if(!existing || existing->buildIdentity!=task.identity){available=0;break;}
            task.seed=CoarseOnly(*existing);
            for(uint32_t mip=0;mip<2;++mip)
            {
                const auto* detail=options.previousManifest->FindSubAsset(DistanceFieldSubAssetKey(key,mip));
                if(!detail || (options.artifactExists && !options.artifactExists(*detail)))available&=~(1u<<mip);
            }
        }
        task.available=available;
    }
    const uint32_t mipMask=options.kernel.mipMask|4u;
    result.stats.backend=DistanceFieldQueryBackendName(options.kernel.backend);
    uint32_t completed=0,total=0;for(uint32_t mip=0;mip<3;++mip)if(mipMask&(1u<<mip))total+=static_cast<uint32_t>(tasks.size());
    std::map<std::string,ArtifactPayload> latest;
    std::vector<std::pair<size_t,uint32_t>> schedule;
    for(size_t i=0;i<tasks.size();++i)if(!tasks[i].fallback)schedule.emplace_back(i,2);
    // Keep a mesh's detail work adjacent. A level-wide traversal of every
    // fallback evicted useful BVHs before the next mip could reuse them.
    for(size_t i=0;i<tasks.size();++i)if(!tasks[i].fallback)
        for(uint32_t level:{1u,0u})if(mipMask&(1u<<level))schedule.emplace_back(i,level);
    for(size_t i=0;i<tasks.size();++i)if(tasks[i].fallback)
        for(uint32_t level:{2u,1u,0u})if(mipMask&(1u<<level))schedule.emplace_back(i,level);
    for(const auto& [taskIndex,level]:schedule)
    {
            auto& task=tasks[taskIndex];
            if(cancelled()){result.diagnostics.push_back("Distance field build cancelled");return result;}
            DistanceFieldModelBuildProgress progress{task.keys.front(),"Building mesh distance fields",level,completed,total,result.stats};
            if(task.available&(1u<<level))
            {++result.skippedMips;++completed;progress.completed=completed;progress.stage="Distance field already current";if(options.progress)options.progress(progress);continue;}
            if(options.progress)options.progress(progress);
            auto asset=Cached(task.identity,options.kernel,level);
            if(asset)++result.stats.cachedMips;
            else
            {
                std::vector<Assets::MeshArtifactData> meshes;meshes.reserve(task.members.size());
                for(const auto& key:task.members)
                {
                    if(cancelled()){result.diagnostics.push_back("Distance field build cancelled");return result;}
                    auto mesh=options.loadMesh(key);++result.meshLoads;
                    if(!mesh){result.diagnostics.push_back("Distance field source mesh could not be loaded: "+key);return result;}
                    meshes.push_back(std::move(*mesh));
                }
                if(ComputeDistanceFieldGeometryIdentity(meshes)!=task.geometry)
                {result.diagnostics.push_back("Distance field source geometry changed: "+task.keys.front());return result;}
                auto kernel=options.kernel;kernel.mipMask=1u<<level;kernel.mipReady={};kernel.seed=task.seed.get();DistanceFieldBuildStats stats;kernel.stats=&stats;
                auto built=BuildMeshDistanceField(meshes,settings,cancellation,importCancellation,&kernel);Sum(result.stats,stats);
                result.diagnostics.insert(result.diagnostics.end(),built.diagnostics.begin(),built.diagnostics.end());
                if(!built.success || !built.asset)return result;asset=std::move(built.asset);
            }
            if(cancelled()){result.diagnostics.push_back("Distance field build cancelled");return result;}
            const uint32_t available=task.available|(1u<<level)|4u;
            auto payloads=Payloads(task,*asset,available,level);
            if(std::any_of(payloads.begin(),payloads.end(),[](const auto& p){return p.payload.empty();}))
            {result.diagnostics.push_back("Distance field artifact serialization failed");return result;}
            std::vector<std::string> removed;
            if(task.available==0)for(const auto& key:task.keys)for(uint32_t mip=0;mip<2;++mip)
                if(!(available&(1u<<mip)))removed.push_back(DistanceFieldSubAssetKey(key,mip));
            progress.completed=++completed;progress.stats=result.stats;
            if(options.publish && !options.publish(payloads,removed,progress))
            {result.diagnostics.push_back("Distance field publication rejected an obsolete or cancelled job");return result;}
            if(!options.publish)for(auto& payload:payloads)latest.insert_or_assign(payload.subAssetKey,std::move(payload));
            task.available=available;task.seed=CoarseOnly(*asset);if(options.progress)options.progress(progress);
    }
    for(auto& [key,payload]:latest)result.payloads.push_back(std::move(payload));
    result.success=true;return result;
}
}
