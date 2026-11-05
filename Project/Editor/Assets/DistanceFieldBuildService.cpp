#include "Project/Editor/Assets/DistanceFieldBuildService.h"

#include <chrono>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/AssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/EditorActions.h"
#include "Core/EditorTaskProgressService.h"
#include "Debug/Logger.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/Assets/SceneModelReader.h"
#include "Rendering/Resources/Parsers/EModelParserFlags.h"

namespace NLS::Editor::Assets
{
namespace
{
namespace df=NLS::Render::DistanceField;
namespace ca=NLS::Core::Assets;
using Clock=std::chrono::steady_clock;
std::mutex jobsMutex;
std::unordered_map<std::string,std::shared_ptr<DistanceFieldBuildJob>> jobs;
std::string JobKey(const std::filesystem::path& root,const std::string& path)
{return root.lexically_normal().generic_string()+"/"+path;}
std::vector<uint8_t> Read(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return {};
    const auto size=file.tellg();if(size<0 || size>std::streamoff(1024ull*1024*1024))return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);if(size)file.read(reinterpret_cast<char*>(bytes.data()),size);return file?bytes:std::vector<uint8_t>{};
}
struct Cancel final:ca::IArtifactWriteCancellation
{
    const std::atomic_bool* flag;
    explicit Cancel(const std::atomic_bool* value):flag(value){}
    bool IsCancellationRequested()const override{return flag && flag->load(std::memory_order_relaxed);}
};
}

std::string DistanceFieldBuildService::MetadataStamp(const std::filesystem::path& path)
{
    std::error_code error;const auto size=std::filesystem::file_size(path,error);if(error)return {};
    const auto stamp=std::filesystem::last_write_time(path,error);if(error)return {};
    return std::to_string(size)+":"+std::to_string(static_cast<std::intmax_t>(stamp.time_since_epoch().count()));
}

DistanceFieldBuildResult DistanceFieldBuildService::Build(const DistanceFieldBuildRequest& request,
    const std::atomic_bool* cancellation,std::function<void(const df::DistanceFieldModelBuildProgress&)> progress)
{
    DistanceFieldBuildResult result;const auto begin=Clock::now();
    Cancel cancel(cancellation);
    const auto elapsed=[&]{return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();};
    const auto fail=[&](std::string message){result.error=std::move(message);result.cancelled=cancel.IsCancellationRequested();result.totalMs=elapsed();return result;};
    if(cancel.IsCancellationRequested())return fail("Distance field build cancelled");
    const auto sourcePath=(request.projectRoot/request.assetPath).lexically_normal();
    const auto meta=ca::AssetMeta::Load(ca::GetAssetMetaPath(sourcePath));
    if(!meta || meta->assetType!=ca::AssetType::ModelScene)return fail("Distance field builds require an imported source model");
    const auto roots=MakeProjectEditorAssetRoots(request.projectRoot);
    AssetDatabaseFacade database(roots);
    const std::array<std::filesystem::path,1> known{sourcePath};
    if(!database.RefreshKnownSourceAssets(known))return fail("Distance field source registration failed");
    auto manifest=LoadArtifactManifestFromProjectArtifactDB(request.projectRoot,meta->id);
    if(!manifest)return fail("Import this model once before building distance fields");
    const auto fingerprint=ca::ComputeModelImportSettingsFingerprint(*meta);
    const auto metaPath=ca::GetAssetMetaPath(sourcePath);
    const auto relativeMeta=(std::filesystem::path(request.assetPath).generic_string()+".meta");
    bool checkedMeta=false;
    for(const auto& dependency:manifest->dependencies)
        if(dependency.kind==ca::AssetDependencyKind::PathToGuidMapping &&
            std::filesystem::path(dependency.value).lexically_normal().generic_string()==relativeMeta)
        {
            checkedMeta=dependency.hashOrVersion==fingerprint || dependency.hashOrVersion==MetadataStamp(metaPath) ||
                (!request.previousMetaStamp.empty() && dependency.hashOrVersion==request.previousMetaStamp);
            break;
        }
    const auto resolve=[&](const ca::ImportedArtifact& artifact)
    {const auto p=std::filesystem::path(artifact.artifactPath);return p.is_absolute()?p:request.projectRoot/p;};
    std::string expectedSource=df::DistanceFieldSourceManifestIdentity(*manifest);
    auto expectedSettings=meta->settings;
    const auto settings=df::ReadDistanceFieldBuildSettings(meta->settings,request.projectRoot);
    expectedSettings["__DF_PROJECT_ROOT"]=request.projectRoot.generic_string();
    expectedSettings["__DF_PROJECT_ENABLED"]=settings.projectEnabled?"true":"false";
    // Legacy manifests only record metadata mtimes. A derived build can still
    // use their published geometry, but must not claim unapplied import options
    // were used to produce that geometry.
    if(!checkedMeta)expectedSettings["__DF_KEEP_IMPORT_STAMP"]="true";
    std::optional<df::DistanceFieldSourceDescription> input;
    if(const auto* entry=manifest->FindSubAsset(df::DistanceFieldSourceKey))input=df::DeserializeDistanceFieldSource(Read(resolve(*entry)));
    if(!input)
    {
        // Migration reads metadata once. Geometry always comes from published LOD0,
        // so material conversion, texture import and rendering LOD builds are skipped.
        const auto bytes=Read(sourcePath);
        bool sourceMatches=false;
        for(const auto& dependency:manifest->dependencies)
            if(dependency.kind==ca::AssetDependencyKind::SourceFileHash &&
                std::filesystem::path(dependency.value).lexically_normal().generic_string()==request.assetPath)
                sourceMatches=dependency.hashOrVersion==ca::ComputeNativeArtifactPayloadHash(bytes);
        if(!sourceMatches)return fail("The source model changed. Import its geometry before migrating distance field metadata.");
        Render::Assets::ImportedScene scene;
        const auto extension=sourcePath.extension().string();++result.legacySourceReads;
        if(extension==".gltf" || extension==".glb")
            scene=Render::Assets::ImportGltfSceneBytes(bytes,meta->id,sourcePath.stem().string(),Render::Assets::ReadSceneImportSettings(meta->settings));
        else
        {
            std::vector<Render::Resources::Parsers::ParsedMeshData> parsed;std::vector<std::string> materials,dependencies;std::string error;
            if(!Render::Assets::ReadModelSource(sourcePath,*meta,Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,parsed,materials,dependencies,scene,error))return fail(error);
        }
        std::map<std::string,Render::Assets::MeshArtifactData> meshes;
        for(const auto& artifact:manifest->subAssets)if(artifact.artifactType==ca::ArtifactType::Mesh)
        {
            if(cancel.IsCancellationRequested())return fail("Distance field build cancelled");
            auto bundle=Render::Assets::LoadMeshArtifactBundle(resolve(artifact));
            if(!bundle || bundle->lodResources.empty())return fail("The model's published LOD0 could not be loaded");
            meshes.emplace(artifact.subAssetKey,std::move(bundle->lodResources.front().mesh));
        }
        input=df::DescribeDistanceFieldSource(scene,meshes);
        const std::vector<ca::ArtifactPayload> description{df::DistanceFieldSourcePayload(*input)};
        const auto publicationStart=Clock::now();
        auto patched=database.PublishDistanceFieldArtifactPatch(request.assetPath,expectedSource,fingerprint,expectedSettings,description,{},&cancel);
        result.publicationMs+=std::chrono::duration<double,std::milli>(Clock::now()-publicationStart).count();
        if(!patched)return fail("Distance field source changed while its metadata was being migrated");
        manifest=std::move(patched);expectedSource=df::DistanceFieldSourceManifestIdentity(*manifest);++result.publications;
    }
    if(!settings.Enabled())
    {
        std::vector<std::string> removed;
        for(const auto& artifact:manifest->subAssets)if(artifact.artifactType==ca::ArtifactType::MeshDistanceField)removed.push_back(artifact.subAssetKey);
        if(!removed.empty() && !database.PublishDistanceFieldArtifactPatch(request.assetPath,expectedSource,fingerprint,expectedSettings,{},removed,&cancel))
            return fail("Distance field disable was superseded by another source update");
        result.success=true;result.totalMs=elapsed();return result;
    }
    df::DistanceFieldModelBuildOptions options;
    options.kernel.cacheRoot=request.projectRoot/"Library/DistanceFieldCache";options.kernel.forceRebuild=request.forceRebuild;
    options.kernel.mipMask=request.mipMask;options.includeFallbacks=request.includeFallbacks;options.previousManifest=&*manifest;
    const auto meshEntries=*manifest;
    options.loadMesh=[&](const std::string& key)->std::optional<Render::Assets::MeshArtifactData>
    {
        const auto* entry=meshEntries.FindSubAsset(key);if(!entry || entry->artifactType!=ca::ArtifactType::Mesh)return {};
        auto bundle=Render::Assets::LoadMeshArtifactBundle(resolve(*entry));
        if(!bundle || bundle->lodResources.empty())return {};return std::move(bundle->lodResources.front().mesh);
    };
    options.readArtifact=[&](const ca::ImportedArtifact& artifact){return Read(resolve(artifact));};
    options.artifactExists=[&](const ca::ImportedArtifact& artifact){std::error_code error;return std::filesystem::is_regular_file(resolve(artifact),error);};
    options.progress=std::move(progress);
    std::map<std::string,ca::ArtifactPayload> pending;
    std::unordered_set<std::string> pendingRemovals;
    uint64_t pendingBytes=0;
    auto lastPublication=Clock::now();
    df::DistanceFieldModelBuildProgress lastState;
    const auto flush=[&]()
    {
        if(pending.empty() && pendingRemovals.empty())return true;
        if(cancel.IsCancellationRequested())return false;
        std::vector<ca::ArtifactPayload> payloads;payloads.reserve(pending.size());
        for(auto& [key,payload]:pending)payloads.push_back(std::move(payload));
        const std::vector<std::string> removed(pendingRemovals.begin(),pendingRemovals.end());
        const auto publicationStart=Clock::now();
        auto patched=database.PublishDistanceFieldArtifactPatch(request.assetPath,expectedSource,fingerprint,expectedSettings,payloads,removed,&cancel);
        result.publicationMs+=std::chrono::duration<double,std::milli>(Clock::now()-publicationStart).count();
        if(!patched)return false;
        manifest=std::move(patched);expectedSource=df::DistanceFieldSourceManifestIdentity(*manifest);++result.publications;
        if(lastState.mip==2 && result.firstCoarseMs==0)result.firstCoarseMs=elapsed();
        pending.clear();pendingRemovals.clear();pendingBytes=0;lastPublication=Clock::now();return true;
    };
    options.publish=[&](const auto& payloads,const auto& removed,const auto& state)
    {
        lastState=state;
        for(const auto& key:removed){pending.erase(key);pendingRemovals.insert(key);}
        for(const auto& payload:payloads)
        {
            if(const auto it=pending.find(payload.subAssetKey);it!=pending.end())pendingBytes-=it->second.payload.size();
            pending.insert_or_assign(payload.subAssetKey,payload);pendingBytes+=payload.payload.size();pendingRemovals.erase(payload.subAssetKey);
        }
        // Publish the first coarse result immediately; amortize subsequent DB
        // transactions without withholding a completed model's preview.
        if(result.firstCoarseMs==0 || pendingBytes>=8ull*1024*1024 || state.completed==state.total ||
            Clock::now()-lastPublication>=std::chrono::milliseconds(1000))return flush();
        return true;
    };
    // Build keeps its own availability table; publication can safely replace manifest.
    const auto initial=*manifest;options.previousManifest=&initial;
    result.model=df::DistanceFieldBuildService::Build(*input,settings,options,cancellation,&cancel);
    if(!result.model.success)return fail(result.model.diagnostics.empty()?"Distance field build failed":result.model.diagnostics.back());
    if(!flush())return fail("Distance field publication was superseded or cancelled");
    result.success=true;result.totalMs=elapsed();return result;
}

std::shared_ptr<DistanceFieldBuildJob> DistanceFieldBuildService::Queue(DistanceFieldBuildRequest request,
    std::function<void(bool,std::string)> completion)
{
    if(!NLS::Core::ServiceLocator::Contains<Core::EditorActions>())return {};
    const auto meta=ca::AssetMeta::Load(ca::GetAssetMetaPath(request.projectRoot/request.assetPath));if(!meta)return {};
    const auto settings=df::ReadDistanceFieldBuildSettings(meta->settings,request.projectRoot);
    const auto signature=request.sourceIdentityHint+"/"+meta->id.ToString()+"/"+std::to_string(df::BuilderVersion)+"/"+
        std::to_string(settings.resolutionScale)+"/"+std::to_string(settings.twoSided)+"/"+
        std::to_string(settings.Enabled())+"/"+std::to_string(request.mipMask)+"/"+std::to_string(request.includeFallbacks);
    const auto key=JobKey(request.projectRoot,request.assetPath);
    auto job=std::make_shared<DistanceFieldBuildJob>();job->signature=signature;
    {
        std::lock_guard lock(jobsMutex);auto& current=jobs[key];
        if(current && current->running.load() && !current->cancelled.load() && current->signature==signature && !request.forceRebuild && !completion)return current;
        if(current && current->running.load())current->cancelled=true;
        current=job;
    }
    auto task=Core::TryBeginEditorTask("Build distance fields",request.assetPath,0.f,[weak=std::weak_ptr(job)]{if(auto job=weak.lock())job->cancelled=true;});
    const bool queued=NLS::Core::ServiceLocator::Get<Core::EditorActions>().TrackBackgroundTask([request=std::move(request),job,task,completion=std::move(completion)]()mutable
    {
        DistanceFieldBuildResult result;
        try
        {
            result=Build(request,&job->cancelled,[&](const auto& progress)
            {
                {std::lock_guard lock(job->mutex);job->progress=progress;}
                task.Update(request.assetPath+" | "+progress.meshKey+" mip="+std::to_string(progress.mip)+" "+
                    std::to_string(progress.completed)+"/"+std::to_string(progress.total),progress.total?float(progress.completed)/progress.total:0.f);
            });
        }
        catch(const std::exception& error){result.error=error.what();}
        catch(...){result.error="Distance field build failed with an unknown error";}
        const bool success=result.success;const auto error=result.error;
        NLS_LOG_INFO("[DistanceFieldBuild] asset="+request.assetPath+" success="+std::to_string(success)+" sourceReads="+std::to_string(result.legacySourceReads)+
            " builtMips="+std::to_string(result.model.stats.builtMips)+" cachedMips="+std::to_string(result.model.stats.cachedMips)+
            " currentMips="+std::to_string(result.model.skippedMips)+" firstCoarseMs="+std::to_string(result.firstCoarseMs)+" totalMs="+std::to_string(result.totalMs)+" "+error);
        task.Finish(success?Core::EditorTaskState::Succeeded:job->cancelled.load()?Core::EditorTaskState::Cancelled:Core::EditorTaskState::Failed,error);
        {std::lock_guard lock(job->mutex);job->result=std::move(result);}job->running.store(false,std::memory_order_release);
        if(NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
            NLS::Core::ServiceLocator::Get<Core::EditorActions>().DelayAction([completion=std::move(completion),success,error]()mutable{if(completion)completion(success,error);});
    });
    if(!queued)
    {job->cancelled=true;job->running=false;task.Finish(Core::EditorTaskState::Failed,"The background task queue is full");return {};}
    return job;
}
bool DistanceFieldBuildService::CanSchedule()
{return NLS::Core::ServiceLocator::Contains<Core::EditorActions>();}
bool DistanceFieldBuildService::IsBuilding(const std::filesystem::path& root,const std::string& path)
{
    std::lock_guard lock(jobsMutex);const auto it=jobs.find(JobKey(root,path));return it!=jobs.end() && it->second->running.load(std::memory_order_acquire);
}
std::shared_ptr<DistanceFieldBuildJob> DistanceFieldBuildService::FindBuildJob(const std::filesystem::path& root,const std::string& path)
{
    std::lock_guard lock(jobsMutex);const auto it=jobs.find(JobKey(root,path));return it==jobs.end()?nullptr:it->second;
}
void DistanceFieldBuildService::CancelAll(const std::filesystem::path& root)
{
    const auto prefix=root.lexically_normal().generic_string()+"/";
    std::lock_guard lock(jobsMutex);for(auto& [key,job]:jobs)if(key.starts_with(prefix) && job->running.load())job->cancelled=true;
}
}
