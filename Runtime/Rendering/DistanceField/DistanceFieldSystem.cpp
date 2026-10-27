#include "Rendering/DistanceField/DistanceFieldSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Core/ResourceManagement/ShaderManager.h"

namespace NLS::Render::DistanceField
{
    namespace
    {
        using namespace RHI;
        using namespace Context;
        constexpr uint32_t MeshXY=64,PageXY=64;
        constexpr uint64_t MeshBudget=128ull*1024*1024,PageBudget=128ull*1024*1024,UploadBudget=8ull*1024*1024;
        std::mutex systemsMutex;
        std::unordered_map<RHIDevice*,std::weak_ptr<DistanceFieldSystem>> systems;
        std::atomic_uint64_t runtimeRevision{1};
        std::atomic_uint32_t pendingLoads{0};
        std::atomic_uint32_t pendingUploads{0};
        std::atomic_uint64_t nextViewId{1};

        struct GpuObject
        {
            Maths::Vector4 inverse[3],minScale,maxBand;
            std::array<uint32_t,4> gridOffset;
            Maths::Vector4 worldMinStatic,worldMax;
        };
        static_assert(sizeof(GpuObject)==128);
        struct Constants
        {
            uint32_t objects=0,dirty=0,capacity=0,meshXY=MeshXY;
            uint32_t pagesX=PageXY,pagesY=PageXY,probes=0,cache=0;
            uint32_t width=0,height=0,visualization=0,channel=0;
            Maths::Vector4 clips[4];
            float inverseViewProjection[16]{};
            Maths::Vector4 camera;
            float slice=0,traceDistance=2048;
            uint32_t mostlyStaticOnly=0,padding=0;
        };
        struct FrameOwner
        {
            DistanceFieldFrameInput input;
            std::deque<std::vector<uint8_t>> uploadBytes;
            std::vector<std::shared_ptr<RHIBuffer>> buffers;
            std::vector<std::shared_ptr<RHITexture>> textures;
            std::vector<std::shared_ptr<RHIBindingSet>> bindings;
        };
        struct TimingReadback
        {
            std::array<uint64_t,2> ticks{};
            std::shared_ptr<RHITimestampQuery> query;
            std::shared_ptr<PostSubmitBufferReadbackState> state=std::make_shared<PostSubmitBufferReadbackState>();
        };
        std::mutex retiredTimingMutex;
        std::vector<std::shared_ptr<TimingReadback>> retiredTimings;
        struct CounterReadback
        {
            std::array<uint32_t,5> data{};
            std::shared_ptr<RHICompletionToken> completion;
        };
        std::vector<std::shared_ptr<CounterReadback>> retiredCounters;
        bool TimingFinished(const std::shared_ptr<TimingReadback>& timing)
        {
            std::lock_guard lock(timing->state->mutex);
            if(!timing->state->beginAttempted) return timing->state.use_count()==1;
            return !timing->state->beginSucceeded || !timing->state->completion ||
                timing->state->completion->Poll().code!=RHICompletionStatusCode::Pending;
        }
        std::shared_ptr<RHIBuffer> Buffer(const std::shared_ptr<RHIDevice>& device,const void* bytes,
            size_t size,BufferUsageFlags usage,MemoryUsage memory,const std::string& name)
        {
            RHIBufferDesc desc; desc.size=(std::max)(size,size_t(16)); desc.usage=usage; desc.memoryUsage=memory; desc.debugName=name;
            RHIBufferUploadDesc upload;
            if(bytes && size) { upload.data=bytes; upload.dataSize=size; }
            return device->CreateBuffer(desc,upload);
        }
        TextureVisibilityTransition TextureTransition(const std::shared_ptr<RHITexture>& texture,ResourceState after)
        {
            TextureVisibilityTransition result; result.texture=texture;
            result.after=after; result.useTrackedBeforeState=true;
            result.sourceStages=result.destinationStages=PipelineStageMask::AllCommands;
            result.sourceAccess=AccessMask::MemoryRead|AccessMask::MemoryWrite;
            result.destinationAccess=AccessMask::MemoryRead|AccessMask::MemoryWrite;
            return result;
        }
        void DeclareTexture(RenderPassCommandInput& pass,const std::shared_ptr<RHITexture>& texture,
            ResourceAccessMode mode,ResourceState state)
        {
            if(!texture) return;
            TextureResourceAccess access; access.texture=texture; access.mode=mode; access.state=state;
            access.stages=PipelineStageMask::AllCommands;
            access.access=mode==ResourceAccessMode::Write?AccessMask::MemoryWrite:AccessMask::MemoryRead;
            pass.textureResourceAccesses.push_back(std::move(access));
        }
    }

    struct DistanceFieldSystem::Impl
    {
        std::shared_ptr<RHIDevice> device;
        mutable std::mutex mutex;
        DistanceFieldStats stats;
        std::shared_ptr<const DistanceFieldStats> published{std::make_shared<const DistanceFieldStats>()};
        std::shared_ptr<UploadContext> uploads;
        std::shared_ptr<RHITexture> meshAtlas,pageAtlas;
        std::shared_ptr<RHITextureView> meshView,pageView;
        std::shared_ptr<RHISampler> sampler;
        std::shared_ptr<RHIBuffer> freePages,poolCounters;
        std::shared_ptr<RHIBindingLayout> bindingLayout;
        std::shared_ptr<RHIPipelineLayout> pipelineLayout;
        std::map<std::string,std::shared_ptr<RHIComputePipeline>> pipelines;
        uint32_t pageCapacity=0;
        uint64_t serial=0,residencyRevision=1,lastReadbackFailures=0,lastReadbackRetries=0;
        uint64_t budgetFrame=0,remainingUploadBudget=UploadBudget;
        uint32_t unfinishedUploads=0;
        std::vector<uint32_t> freeMeshSlots;
        TextureFormat pageFormat=TextureFormat::R32F;
        struct Allocation { std::vector<uint32_t> slots,indirection; size_t uploaded=0; };
        struct Asset
        {
            MeshDistanceFieldSource source;
            std::shared_ptr<MeshDistanceFieldArtifact> data;
            std::future<std::shared_ptr<MeshDistanceFieldArtifact>> future;
            std::array<std::optional<Allocation>,3> allocations;
            std::array<bool,3> allocationFailed{};
            uint64_t lastUse=0,lastDetailUse=0;
            uint32_t requestedMip=2,loadingMip=2;
            bool failed=false;
        };
        std::unordered_map<std::string,Asset> assets;
        struct View
        {
            std::weak_ptr<const void> lifetime;
            DistanceFieldSceneData scene;
            DistanceFieldViewState state;
            std::shared_ptr<RHIBuffer> table;
            std::shared_ptr<RHITexture> debugOutput;
            std::shared_ptr<RHITextureView> debugView;
            uint64_t residency=0;
        };
        std::unordered_map<uint64_t,View> views;
        std::shared_ptr<CounterReadback> readback;
        std::vector<std::shared_ptr<TimingReadback>> timings;

        void PollTimings()
        {
            for(auto it=timings.begin();it!=timings.end();)
            {
                if(!TimingFinished(*it)) { ++it; continue; }
                auto timing=*it;
                std::lock_guard lock(timing->state->mutex);
                if(timing->state->beginSucceeded && (!timing->state->completion || timing->state->completion->GetStatus().Succeeded()) &&
                    timing->ticks[1]>=timing->ticks[0] && timing->query->Frequency())
                    stats.gpuUpdateMs=double(timing->ticks[1]-timing->ticks[0])*1000.0/double(timing->query->Frequency());
                it=timings.erase(it);
            }
            std::lock_guard lock(retiredTimingMutex);
            retiredTimings.erase(std::remove_if(retiredTimings.begin(),retiredTimings.end(),TimingFinished),retiredTimings.end());
            retiredCounters.erase(std::remove_if(retiredCounters.begin(),retiredCounters.end(),[](const auto& counter)
            { return !counter->completion || counter->completion->Poll().code!=RHICompletionStatusCode::Pending; }),retiredCounters.end());
        }

        bool Initialize()
        {
        if(meshAtlas && pageAtlas && meshView && pageView && sampler && freePages && poolCounters &&
            bindingLayout && pipelineLayout && uploads && device->IsBackendReady()) return true;
            if(!device || !device->GetCapabilities().SupportsFeature(RHIDeviceFeature::DistanceFields) || !device->IsBackendReady())
            { stats.diagnostic="Distance fields require the Windows DX12 backend"; return false; }
            const auto& format=device->GetCapabilities().GetTextureFormatCapability(TextureFormat::R16F);
            pageFormat=format.storage && format.storageRead && format.sampled?TextureFormat::R16F:TextureFormat::R32F;
            const auto& fallback=device->GetCapabilities().GetTextureFormatCapability(pageFormat);
            if(!fallback.storage || !fallback.sampled)
            { stats.diagnostic="No sampled floating point volume UAV format is available"; return false; }
            const uint32_t bytes=pageFormat==TextureFormat::R16F?2u:4u;
            pageCapacity=static_cast<uint32_t>(PageBudget/(512u*bytes));
            pageCapacity=pageCapacity/(PageXY*PageXY)*(PageXY*PageXY);
            RHITextureDesc desc; desc.dimension=TextureDimension::Texture3D; desc.format=TextureFormat::R8;
            desc.extent={MeshXY*8,MeshXY*8,uint32_t(MeshBudget/(MeshXY*MeshXY*512))*8};
            desc.usage=TextureUsageFlags::Sampled|TextureUsageFlags::CopyDst;
            desc.debugName="MeshDistanceField.BrickAtlas"; meshAtlas=device->CreateTexture(desc);
            desc.format=pageFormat; desc.extent={PageXY*8,PageXY*8,pageCapacity/(PageXY*PageXY)*8};
            desc.usage=TextureUsageFlags::Sampled|TextureUsageFlags::Storage;
            desc.debugName="GlobalDistanceField.PageAtlas"; pageAtlas=device->CreateTexture(desc);
            if(!meshAtlas || !pageAtlas) { stats.diagnostic="Distance field volume allocation failed"; return false; }
            RHITextureViewDesc view; view.viewType=TextureViewType::Texture3D;
            meshView=device->CreateTextureView(meshAtlas,view); pageView=device->CreateTextureView(pageAtlas,view);
            SamplerDesc samplerDesc; samplerDesc.wrapU=samplerDesc.wrapV=samplerDesc.wrapW=TextureWrap::ClampToEdge;
            sampler=device->CreateSampler(samplerDesc,"DistanceField.LinearClamp");
            const uint32_t meshCapacity=static_cast<uint32_t>(MeshBudget/512);
            freeMeshSlots.clear(); freeMeshSlots.reserve(meshCapacity);
            for(uint32_t i=meshCapacity;i>0;--i) freeMeshSlots.push_back(i-1);
            std::vector<uint32_t> pages(pageCapacity); std::iota(pages.begin(),pages.end(),0);
            const uint32_t counters[]={pageCapacity,0,0,0,0};
            freePages=Buffer(device,pages.data(),pages.size()*4,BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead,MemoryUsage::GPUOnly,"DistanceField.FreePages");
            poolCounters=Buffer(device,counters,sizeof(counters),BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|BufferUsageFlags::CopySrc,MemoryUsage::GPUOnly,"DistanceField.PoolCounters");
            RHIBindingLayoutDesc layout;
            const auto add=[&](const char* name,BindingType type,uint32_t slot,uint32_t stride=0)
            { layout.entries.push_back({name,type,0,slot,1,ShaderStageMask::Compute,0,stride}); };
            add("DFConstants",BindingType::UniformBuffer,0);
            add("Objects",BindingType::StructuredBuffer,0,sizeof(GpuObject));
            add("MeshIndirection",BindingType::StructuredBuffer,1,4);
            add("DirtyPages",BindingType::StructuredBuffer,2,sizeof(DistanceFieldDirtyPage));
            add("MeshAtlas",BindingType::Texture,3);
            add("ProbePositions",BindingType::StructuredBuffer,4,16);
            add("GlobalAtlas",BindingType::Texture,5);
            add("LinearClamp",BindingType::Sampler,0);
            for(uint32_t i=0;i<6;++i) add("DFStorage",BindingType::StorageBuffer,i,4);
            add("GlobalAtlasWrite",BindingType::RWTexture,6);
            add("DebugOutput",BindingType::RWTexture,7);
            add("ProbeResults",BindingType::StorageBuffer,8,16);
            bindingLayout=device->CreateBindingLayout(layout);
            RHIPipelineLayoutDesc pipeline; pipeline.bindingLayouts={bindingLayout}; pipeline.debugName="DistanceField.Layout";
            pipelineLayout=device->CreatePipelineLayout(pipeline);
            uploads=CreateBackendUploadContext(device->CreateUploadBackend());
            if(!meshView || !pageView || !sampler || !freePages || !poolCounters || !bindingLayout || !pipelineLayout || !uploads)
            { stats.diagnostic="Distance field GPU resource initialization failed"; return false; }
            return true;
        }

        std::shared_ptr<RHIComputePipeline> Pipeline(const std::string& entry)
        {
            if(const auto it=pipelines.find(entry);it!=pipelines.end()) return it->second;
            auto path=std::filesystem::path(NLS::Core::ResourceManagement::ShaderManager::EngineAssetsRoot())/"Shaders/HLSL/DistanceField.hlsl";
#if defined(NLS_DEVELOPMENT_ENGINE_ASSETS_ROOT)
            if(!std::filesystem::exists(path)) path=std::filesystem::path(NLS_DEVELOPMENT_ENGINE_ASSETS_ROOT)/"Shaders/HLSL/DistanceField.hlsl";
#endif
            ShaderCompiler::ShaderCompilationInput input; input.assetPath=path.string();
            input.stage=ShaderCompiler::ShaderStage::Compute;
            input.options.targetPlatform=ShaderCompiler::ShaderTargetPlatform::DXIL;
            input.options.targetProfile="cs_6_0"; input.options.entryPoint=entry;
            ShaderCompiler::ShaderCompiler compiler; const auto compiled=compiler.Compile(input);
            if(compiled.status!=ShaderCompiler::ShaderCompilationStatus::Succeeded)
            { stats.diagnostic=compiled.diagnostics; return {}; }
            RHIShaderModuleDesc shader; shader.stage=ShaderStage::Compute; shader.targetBackend=NativeBackendType::DX12;
            shader.entryPoint=entry; shader.bytecode=compiled.bytecode;
            RHIComputePipelineDesc desc; desc.computeShader=device->CreateShaderModule(shader);
            desc.pipelineLayout=pipelineLayout; desc.debugName="DistanceField."+entry;
            auto result=device->CreateComputePipeline(desc);
            if(!result) stats.diagnostic="Distance field pipeline creation failed: "+entry;
            else pipelines[entry]=result;
            return result;
        }

        void PollAssets()
        {
            for(auto& [key,asset]:assets)
            {
                if(!asset.future.valid() || asset.future.wait_for(std::chrono::seconds(0))!=std::future_status::ready) continue;
                auto data=asset.future.get(); --pendingLoads;
                if(!data) { asset.failed=true; ++stats.failedLoads; continue; }
                asset.data=std::move(data); ++runtimeRevision;
            }
        }
        Asset* Resolve(const DistanceFieldObjectInput& object,uint32_t requestedMip)
        {
            std::optional<MeshDistanceFieldSource> source;
            if(!object.asset) source=FindMeshDistanceFieldSource(object.meshArtifactPath);
            if(!object.asset && !source) return nullptr;
            const std::string key=object.asset?"memory:"+object.asset->buildIdentity:
                (source->contentHash.empty()?source->mainPath:"artifact:"+source->contentHash);
            auto& asset=assets[key];
            asset.lastUse=serial; asset.requestedMip=(std::min)(asset.requestedMip,requestedMip);
            if(requestedMip<2) asset.lastDetailUse=serial;
            if(object.asset && !asset.data) asset.data=std::make_shared<MeshDistanceFieldArtifact>(*object.asset);
            if(source) asset.source=*source;
            if(!asset.future.valid() && !asset.failed && pendingLoads.load()<4)
            {
                const bool main=!asset.data;
                const bool detail=asset.data && (asset.data->mips[asset.requestedMip].indirection.empty()) && asset.requestedMip<2;
                if(main || detail)
                {
                    auto previous=asset.data;
                    const auto captured=asset.source; const uint32_t mip=asset.requestedMip;
                    ++pendingLoads;
                    asset.future=std::async(std::launch::async,[previous,captured,mip]() -> std::shared_ptr<MeshDistanceFieldArtifact>
                    {
                        try
                        {
                            auto result=previous?std::make_shared<MeshDistanceFieldArtifact>(*previous):LoadDistanceFieldArtifact(captured.mainPath);
                            if(!result) return {};
                            if(previous) for(uint32_t i=mip;i<2;++i)
                            {
                                if(!result->mips[i].indirection.empty()) continue;
                                auto detail=LoadDistanceFieldMip(captured.mipPaths[i]);
                                if(!detail || detail->grid!=result->mips[i].grid || detail->maxDistance!=result->mips[i].maxDistance) return {};
                                result->mips[i]=std::move(*detail);
                            }
                            return result;
                        }
                        catch(...) { return {}; }
                    });
                }
            }
            return &asset;
        }

        void EvictUnused()
        {
            for(auto it=assets.begin();it!=assets.end();)
            {
                auto& asset=it->second;
                if(serial-asset.lastUse>120 && !asset.future.valid())
                {
                    for(auto& allocation:asset.allocations) if(allocation)
                        freeMeshSlots.insert(freeMeshSlots.end(),allocation->slots.begin(),allocation->slots.end());
                    it=assets.erase(it); ++residencyRevision; ++runtimeRevision;
                }
                else
                {
                    if(serial-asset.lastDetailUse>30)
                        for(uint32_t mip=0;mip<2;++mip) if(asset.allocations[mip])
                        { freeMeshSlots.insert(freeMeshSlots.end(),asset.allocations[mip]->slots.begin(),asset.allocations[mip]->slots.end()); asset.allocations[mip].reset(); ++residencyRevision; ++runtimeRevision; }
                    ++it;
                }
            }
        }

        void UploadAsset(Asset& asset,std::shared_ptr<FrameOwner> owner,std::vector<UploadTextureRequest>& requests,uint64_t& remaining,bool mandatoryOnly=false)
        {
            if(!asset.data) return;
            for(int mipIndex=2;mipIndex>=int(mandatoryOnly?2:asset.requestedMip);--mipIndex)
            {
                const auto& mip=asset.data->mips[mipIndex];
                if(mip.indirection.empty()) continue;
                auto& allocation=asset.allocations[mipIndex];
                if(!allocation)
                {
                    if(asset.allocationFailed[mipIndex]) ++stats.allocationRetries;
                    if(mipIndex==2 && freeMeshSlots.size()<mip.BrickCount())
                        for(auto& [key,other]:assets) for(uint32_t detail=0;detail<2;++detail)
                            if(other.allocations[detail] && freeMeshSlots.size()<mip.BrickCount())
                            {
                                freeMeshSlots.insert(freeMeshSlots.end(),other.allocations[detail]->slots.begin(),other.allocations[detail]->slots.end());
                                other.allocations[detail].reset(); ++residencyRevision; ++runtimeRevision;
                                if(freeMeshSlots.size()>=mip.BrickCount()) break;
                            }
                    if(freeMeshSlots.size()<mip.BrickCount())
                    {
                        ++stats.allocationFailures; asset.allocationFailed[mipIndex]=true;
                        if(mipIndex==2) stats.diagnostic="Mesh distance field atlas is full; mandatory mip allocation failed";
                        continue;
                    }
                    asset.allocationFailed[mipIndex]=false;
                    allocation.emplace(); allocation->slots.reserve(mip.BrickCount());
                    for(uint32_t i=0;i<mip.BrickCount();++i) { allocation->slots.push_back(freeMeshSlots.back()); freeMeshSlots.pop_back(); }
                    allocation->indirection=mip.indirection;
                    for(auto& entry:allocation->indirection) if(entry!=OutsideBrick && entry!=InsideBrick) entry=allocation->slots[entry];
                }
                while(allocation->uploaded<allocation->slots.size() && remaining>=16384)
                {
                    const size_t first=allocation->uploaded; const uint32_t slot=allocation->slots[first];
                    uint32_t run=1;
                    while(first+run<allocation->slots.size() && slot%MeshXY+run<MeshXY && allocation->slots[first+run]==slot+run) ++run;
                    while(run>1 && uint64_t(((run*8+255)/256)*256)*64>remaining) --run;
                    const uint64_t staging=uint64_t(((run*8+255)/256)*256)*64;
                    if(staging>remaining) break;
                    auto& bytes=owner->uploadBytes.emplace_back(size_t(run)*512);
                    for(uint32_t brick=0;brick<run;++brick) for(uint32_t z=0;z<8;++z) for(uint32_t y=0;y<8;++y)
                        std::copy_n(mip.bricks.data()+(first+brick)*512+(z*8+y)*8,8,bytes.data()+(z*8+y)*run*8+brick*8);
                    UploadTextureRequest request; request.destination=meshAtlas; request.data=bytes.data(); request.dataSize=bytes.size();
                    request.extent={run*8,8,8}; request.textureOffset={int32_t(slot%MeshXY*8),int32_t(slot/MeshXY%MeshXY*8),int32_t(slot/(MeshXY*MeshXY)*8)};
                    request.rowPitch=run*8; request.slicePitch=run*64; request.debugName="DistanceField.BrickUpload";
                    requests.push_back(request); allocation->uploaded+=run; remaining-=staging; stats.uploadedBytes+=staging;
                    if(allocation->uploaded==allocation->slots.size()) { ++residencyRevision; ++runtimeRevision; }
                }
            }
        }

        std::shared_ptr<RHIBindingSet> Bind(const Constants& constants,const std::shared_ptr<FrameOwner>& owner,
            const std::array<std::shared_ptr<RHIBuffer>,6>& rw,const std::shared_ptr<RHIBuffer>& objects,
            const std::shared_ptr<RHIBuffer>& indirection,const std::shared_ptr<RHIBuffer>& dirty,
            const std::shared_ptr<RHIBuffer>& positions,const std::shared_ptr<RHIBuffer>& results,
            const std::shared_ptr<RHITextureView>& debug)
        {
            auto uniform=Buffer(device,&constants,sizeof(constants),BufferUsageFlags::Uniform,MemoryUsage::CPUToGPU,"DistanceField.Constants");
            if(!uniform) return {};
            owner->buffers.push_back(uniform);
            RHIBindingSetDesc desc; desc.layout=bindingLayout;
            const auto buffer=[&](BindingType type,uint32_t slot,std::shared_ptr<RHIBuffer> value,uint32_t stride)
            { RHIBindingSetEntry e; e.type=type; e.binding=slot; e.buffer=std::move(value); e.elementStride=stride; desc.entries.push_back(std::move(e)); };
            const auto texture=[&](BindingType type,uint32_t slot,std::shared_ptr<RHITextureView> value)
            { RHIBindingSetEntry e; e.type=type; e.binding=slot; e.textureView=std::move(value); desc.entries.push_back(std::move(e)); };
            buffer(BindingType::UniformBuffer,0,uniform,0);
            buffer(BindingType::StructuredBuffer,0,objects,sizeof(GpuObject));
            buffer(BindingType::StructuredBuffer,1,indirection,4);
            buffer(BindingType::StructuredBuffer,2,dirty,sizeof(DistanceFieldDirtyPage));
            texture(BindingType::Texture,3,meshView);
            buffer(BindingType::StructuredBuffer,4,positions,16);
            texture(BindingType::Texture,5,pageView);
            RHIBindingSetEntry sampling; sampling.type=BindingType::Sampler; sampling.sampler=sampler; desc.entries.push_back(sampling);
            for(uint32_t i=0;i<6;++i) buffer(BindingType::StorageBuffer,i,rw[i],4);
            texture(BindingType::RWTexture,6,pageView); texture(BindingType::RWTexture,7,debug);
            buffer(BindingType::StorageBuffer,8,results,16);
            auto binding=device->CreateBindingSet(desc); if(binding) owner->bindings.push_back(binding);
            return binding;
        }

        void PollReadback()
        {
            if(readback && (!readback->completion || readback->completion->Poll().code!=RHICompletionStatusCode::Pending))
            {
                const bool success=!readback->completion || readback->completion->GetStatus().Succeeded();
                const auto readbackData=readback->data; readback.reset();
                if(success)
                {
                    stats.residentPages=readbackData[2];
                    stats.allocationFailures+=uint32_t(readbackData[1]-uint32_t(lastReadbackFailures));
                    stats.allocationRetries+=uint32_t(readbackData[4]-uint32_t(lastReadbackRetries));
                    lastReadbackRetries=readbackData[4];
                    if(readbackData[1]!=lastReadbackFailures) for(auto& [key,view]:views) view.state.Invalidate();
                    lastReadbackFailures=readbackData[1];
                }
            }
            if(!readback && poolCounters && serial%4==0)
            {
                auto data=std::make_shared<CounterReadback>();
                RHIBufferReadbackDesc desc; desc.source=poolCounters; desc.sourceState=poolCounters->GetState();
                desc.data=data->data.data(); desc.size=sizeof(data->data); desc.debugName="DistanceField.PoolStats";
                const auto result=device->BeginReadBuffer(desc);
                if(result.Succeeded()) { data->completion=result.completion; readback=std::move(data); }
            }
        }
    };

    DistanceFieldSystem::DistanceFieldSystem(std::shared_ptr<RHIDevice> device) : m_impl(std::make_unique<Impl>()) { m_impl->device=std::move(device); }
    DistanceFieldSystem::~DistanceFieldSystem()
    {
        pendingUploads.fetch_sub(m_impl->unfinishedUploads);
        for(auto& timing:m_impl->timings) if(!TimingFinished(timing))
        { std::lock_guard lock(retiredTimingMutex); retiredTimings.push_back(std::move(timing)); }
        // The readback destination is member storage. Keep it alive until its request completes.
        if(m_impl->readback && m_impl->readback->completion &&
            m_impl->readback->completion->Wait(5000000000ull).code==RHICompletionStatusCode::Pending)
        { std::lock_guard lock(retiredTimingMutex); retiredCounters.push_back(std::move(m_impl->readback)); }
        for(auto& [key,asset]:m_impl->assets) if(asset.future.valid()) { asset.future.wait(); --pendingLoads; }
    }
    std::shared_ptr<DistanceFieldSystem> DistanceFieldSystem::Get(const std::shared_ptr<RHIDevice>& device)
    {
        if(!device) return {};
        std::lock_guard lock(systemsMutex); auto& slot=systems[device.get()];
        if(auto system=slot.lock()) return system;
        auto system=std::make_shared<DistanceFieldSystem>(device); slot=system; return system;
    }
    uint64_t DistanceFieldSystem::RuntimeRevision() { return runtimeRevision.load(); }
    bool DistanceFieldSystem::HasPendingWork() { return pendingLoads.load()!=0 || pendingUploads.load()!=0; }
    uint64_t DistanceFieldSystem::AllocateViewId() { return nextViewId.fetch_add(1); }
    std::shared_ptr<const uint64_t> DistanceFieldSystem::CreateViewLifetime()
    { return std::shared_ptr<const uint64_t>(new uint64_t(AllocateViewId()),[](const uint64_t* id){ delete id; ++runtimeRevision; }); }
    DistanceFieldStats DistanceFieldSystem::GetStats() const { return *std::atomic_load(&m_impl->published); }

    DistanceFieldFrameResources DistanceFieldSystem::BuildFrame(DistanceFieldFrameInput input)
    {
        auto& impl=*m_impl; std::lock_guard lock(impl.mutex);
        struct Publish { Impl& impl; ~Publish() { std::atomic_store(&impl.published, std::make_shared<const DistanceFieldStats>(impl.stats)); } } publish{impl};
        const auto begin=std::chrono::steady_clock::now();
        DistanceFieldFrameResources result;
        ++impl.serial; impl.stats.uploadedBytes=0; impl.stats.dirtyPages=0; impl.stats.diagnostic.clear();
        impl.PollAssets(); impl.EvictUnused();
        impl.PollTimings();
        if(!impl.Initialize()) { result.stats=impl.stats; return result; }
        impl.PollReadback();
        auto owner=std::make_shared<FrameOwner>(); owner->input=std::move(input);
        auto& frame=owner->input;
        if(frame.debug.forceRebuild) for(auto& [key,asset]:impl.assets) asset.failed=false;
        const uint64_t frameStamp=frame.frameSerial?frame.frameSerial:impl.serial;
        if(impl.budgetFrame!=frameStamp)
        {
            impl.budgetFrame=frameStamp; impl.remainingUploadBudget=UploadBudget;
            for(auto& [key,asset]:impl.assets) asset.requestedMip=2;
        }
        // Compile before reserving atlas slots or advancing view revisions.
        for(const auto* kernel:{"UploadMarker","ResetWork","CullPages","ReleasePages","AllocatePages","ComposePages","Probe","Visualize","ReleaseView"})
            if(!impl.Pipeline(kernel)) { result.stats=impl.stats; return result; }
        auto& view=impl.views[frame.viewId]; view.lifetime=frame.viewLifetime;
        if(!view.table)
        {
            std::vector<uint32_t> empty(DistanceFieldViewState::PageTableEntries,OutsideBrick);
            view.table=Buffer(impl.device,empty.data(),empty.size()*4,BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead,MemoryUsage::GPUOnly,"DistanceField.PageTable");
        }
        if(!view.table) { impl.stats.diagnostic="Distance field page table allocation failed"; result.stats=impl.stats; return result; }
        if(!view.debugOutput || view.debugOutput->GetDesc().extent.width!=(std::max)(frame.width,1u) || view.debugOutput->GetDesc().extent.height!=(std::max)(frame.height,1u))
        {
            RHITextureDesc desc; desc.extent={(std::max)(frame.width,1u),(std::max)(frame.height,1u),1};
            desc.format=TextureFormat::RGBA8; desc.usage=TextureUsageFlags::Storage|TextureUsageFlags::Sampled|TextureUsageFlags::CopySrc;
            desc.debugName="DistanceField.Visualization"; view.debugOutput=impl.device->CreateTexture(desc);
            if(view.debugOutput) view.debugView=impl.device->CreateTextureView(view.debugOutput,{});
        }
        if(!view.debugView) { impl.stats.diagnostic="Distance field visualization target allocation failed"; result.stats=impl.stats; return result; }
        std::vector<DistanceFieldObjectInput> resolved;
        std::vector<Impl::Asset*> objectAssets;
        std::vector<UploadTextureRequest> textureUploads;
        auto& remaining=impl.remainingUploadBudget;
        std::unordered_map<std::string,Impl::Asset*> resolvedAssets;
        for(auto object:frame.objects)
        {
            if(!object.enabled) continue;
            const float dx=object.localToWorld.data[3]-frame.cameraPosition.x,dy=object.localToWorld.data[7]-frame.cameraPosition.y,dz=object.localToWorld.data[11]-frame.cameraPosition.z;
            const uint32_t requested=frame.debug.visualization==DistanceFieldVisualization::Mesh?0u:(dx*dx+dy*dy+dz*dz<64*64?0u:2u);
            auto* asset=impl.Resolve(object,requested);
            if(!asset || !asset->data) continue;
            object.asset=asset->data; resolvedAssets[asset->data->buildIdentity]=asset;
            resolved.push_back(std::move(object)); objectAssets.push_back(asset);
        }
        // Upload each shared artifact only once, even when many scene instances use it.
        std::vector<Impl::Asset*> unique=objectAssets;
        std::sort(unique.begin(),unique.end()); unique.erase(std::unique(unique.begin(),unique.end()),unique.end());
        for(auto* asset:unique) impl.UploadAsset(*asset,owner,textureUploads,remaining,true);
        for(auto* asset:unique) impl.UploadAsset(*asset,owner,textureUploads,remaining);
        uint32_t unfinished=0;
        for(const auto& [key,asset]:impl.assets) for(const auto& allocation:asset.allocations)
            if(allocation && allocation->uploaded<allocation->slots.size()) ++unfinished;
        pendingUploads.fetch_sub(impl.unfinishedUploads); pendingUploads.fetch_add(unfinished);
        impl.unfinishedUploads=unfinished;
        view.scene.Synchronize(resolved);
        const bool releasingView=std::any_of(impl.views.begin(),impl.views.end(),[&](const auto& item)
        { return item.first!=frame.viewId && item.second.lifetime.expired(); });
        auto dirtyPages=view.state.Update(view.scene,frame.cameraPosition,frame.debug.forceRebuild || releasingView || view.residency!=impl.residencyRevision);
        view.residency=impl.residencyRevision;
        std::vector<GpuObject> objects;
        std::vector<uint32_t> indirection;
        std::unordered_map<const Impl::Allocation*,uint32_t> offsets;
        for(const auto& object:view.scene.Objects())
        {
            auto* asset=resolvedAssets[object.input.asset->buildIdentity];
            if(!asset || !asset->data) continue;
            uint32_t mip=0;
            while(mip<3 && (!asset->allocations[mip] || asset->allocations[mip]->uploaded!=asset->allocations[mip]->slots.size())) ++mip;
            if(mip==3) continue;
            const auto& allocation=*asset->allocations[mip]; const auto& data=asset->data->mips[mip];
            auto [it,inserted]=offsets.emplace(&allocation,static_cast<uint32_t>(indirection.size()));
            if(inserted) indirection.insert(indirection.end(),allocation.indirection.begin(),allocation.indirection.end());
            GpuObject gpu{};
            for(int row=0;row<3;++row) gpu.inverse[row]={object.worldToLocal.data[row*4],object.worldToLocal.data[row*4+1],object.worldToLocal.data[row*4+2],object.worldToLocal.data[row*4+3]};
            const auto& min=asset->data->boundsMin; const auto& max=asset->data->boundsMax;
            gpu.minScale={min.x,min.y,min.z,object.distanceScale}; gpu.maxBand={max.x,max.y,max.z,data.maxDistance};
            gpu.gridOffset={data.grid[0],data.grid[1],data.grid[2],it->second};
            gpu.worldMinStatic={object.boundsMin.x,object.boundsMin.y,object.boundsMin.z,object.input.mostlyStatic?1.f:0.f};
            gpu.worldMax={object.boundsMax.x,object.boundsMax.y,object.boundsMax.z,float(mip)}; objects.push_back(gpu);
        }
        Constants constants; constants.objects=static_cast<uint32_t>(objects.size()); constants.dirty=static_cast<uint32_t>(dirtyPages.size());
        constants.capacity=impl.pageCapacity; constants.probes=static_cast<uint32_t>(frame.probePositions.size());
        constants.width=frame.width; constants.height=frame.height; constants.visualization=static_cast<uint32_t>(frame.debug.visualization);
        constants.channel=static_cast<uint32_t>(frame.debug.channel); constants.slice=frame.debug.slicePosition;
        constants.traceDistance=frame.traceDistance; constants.mostlyStaticOnly=frame.debug.mostlyStaticOnly;
        for(int i=0;i<4;++i) { const auto& c=view.state.clipmaps[i]; constants.clips[i]={c.min.x,c.min.y,c.min.z,c.voxelSize}; }
        std::copy_n(frame.inverseViewProjection.data,16,constants.inverseViewProjection);
        constants.camera={frame.cameraPosition.x,frame.cameraPosition.y,frame.cameraPosition.z,1};
        const auto immutable=[&](const void* bytes,size_t count,const char* name)
        { auto b=Buffer(impl.device,bytes,count,BufferUsageFlags::ShaderRead,MemoryUsage::CPUToGPU,name); owner->buffers.push_back(b); return b; };
        const auto writable=[&](size_t count,const char* name,BufferUsageFlags extra=BufferUsageFlags::None)
        { auto b=Buffer(impl.device,nullptr,count,BufferUsageFlags::Storage|BufferUsageFlags::ShaderRead|extra,MemoryUsage::GPUOnly,name); owner->buffers.push_back(b); return b; };
        const auto objectBuffer=immutable(objects.empty()?nullptr:objects.data(),(std::max)(objects.size()*sizeof(GpuObject),sizeof(GpuObject)),"DistanceField.Objects");
        const auto indirectBuffer=immutable(indirection.data(),indirection.size()*4,"DistanceField.MeshIndirection");
        const auto dirtyBuffer=immutable(dirtyPages.empty()?nullptr:dirtyPages.data(),(std::max)(dirtyPages.size()*sizeof(DistanceFieldDirtyPage),sizeof(DistanceFieldDirtyPage)),"DistanceField.DirtyPages");
        const auto positions=immutable(frame.probePositions.data(),frame.probePositions.size()*16,"DistanceField.ProbePositions");
        const auto probes=writable((std::max)(frame.probePositions.size(),size_t(1))*16,"DistanceField.ProbeResults",BufferUsageFlags::CopySrc);
        const auto candidates=writable((std::max)(dirtyPages.size(),size_t(1))*66*4,"DistanceField.Candidates");
        const auto work=writable((std::max)(dirtyPages.size(),size_t(1))*4,"DistanceField.ComposeWork");
        const auto args=writable(16,"DistanceField.DispatchArguments",BufferUsageFlags::Indirect);
        std::array<std::shared_ptr<RHIBuffer>,6> rw{view.table,impl.freePages,impl.poolCounters,candidates,work,args};
        if(std::any_of(owner->buffers.begin(),owner->buffers.end(),[](const auto& b){return !b;}))
        { impl.stats.diagnostic="Distance field frame buffer allocation failed"; result.stats=impl.stats; return result; }
        const auto pass=[&](const char* entry,uint32_t x,uint32_t y=1,uint32_t z=1,uint32_t cache=0) -> RenderPassCommandInput*
        {
            auto pipeline=impl.Pipeline(entry); if(!pipeline) return nullptr;
            Constants local=constants; local.cache=cache;
            auto bindings=impl.Bind(local,owner,rw,objectBuffer,indirectBuffer,dirtyBuffer,positions,probes,view.debugView);
            if(!bindings) { impl.stats.diagnostic="Distance field binding allocation failed"; return nullptr; }
            RenderPassCommandInput p; p.kind=RenderPassCommandKind::Compute; p.queueType=QueueType::Graphics;
            p.queueDependencyPolicy=QueueDependencyPolicy::Previous; p.debugName=std::string("DistanceField.")+entry;
            RecordedComputeDispatchInput dispatch; dispatch.pipeline=pipeline; dispatch.debugName=p.debugName;
            dispatch.groupCountX=x; dispatch.groupCountY=y; dispatch.groupCountZ=z;
            dispatch.bindingSets.push_back({0,bindings}); dispatch.resourceOwner=owner;
            dispatch.shaderWriteBuffersBefore={rw[0],impl.freePages,impl.poolCounters,candidates,work,probes};
            if(std::string_view(entry)!="ComposePages") dispatch.shaderWriteBuffersBefore.push_back(args);
            dispatch.uavBarrierBuffersBefore=dispatch.shaderWriteBuffersBefore;
            dispatch.uavBarrierBuffersAfter=dispatch.shaderWriteBuffersBefore;
            for(auto& b:dispatch.shaderWriteBuffersBefore)
                p.bufferResourceAccesses.push_back({b,ResourceAccessMode::Write,ResourceState::ShaderWrite,PipelineStageMask::ComputeShader,AccessMask::ShaderWrite});
            p.computeDispatchInputs.push_back(std::move(dispatch)); result.passes.push_back(std::move(p)); return &result.passes.back();
        };
        for(auto it=impl.views.begin();it!=impl.views.end();)
        {
            if(it->first!=frame.viewId && it->second.lifetime.expired())
            {
                if(it->second.table)
                {
                    rw[0]=it->second.table;
                    if(!pass("ReleaseView",1000)) { rw[0]=view.table; result.passes.clear(); result.stats=impl.stats; return result; }
                    rw[0]=view.table;
                }
                it=impl.views.erase(it);
            }
            else ++it;
        }
        if(!textureUploads.empty())
        {
            auto* upload=pass("UploadMarker",1);
            if(!upload) { result.passes.clear(); result.stats=impl.stats; return result; }
            auto& dispatch=upload->computeDispatchInputs.front();
            dispatch.uploadContext=CreateBackendUploadContext(impl.device->CreateUploadBackend());
            dispatch.textureUploadsBefore=std::move(textureUploads);
            DeclareTexture(*upload,impl.meshAtlas,ResourceAccessMode::Write,ResourceState::CopyDst);
        }
        if(!dirtyPages.empty())
        {
            if(!pass("ResetWork",1) || !pass("CullPages",(constants.dirty+63)/64) ||
                !pass("ReleasePages",(constants.dirty+63)/64))
            { result.passes.clear(); result.stats=impl.stats; return result; }
            auto* allocate=pass("AllocatePages",(constants.dirty+63)/64);
            if(!allocate) { result.passes.clear(); result.stats=impl.stats; return result; }
            allocate->computeDispatchInputs.front().exportedBufferVisibilityTransitions.push_back({args,ResourceState::ShaderWrite,ResourceState::IndirectArgument,
                PipelineStageMask::AllCommands,PipelineStageMask::AllCommands,AccessMask::MemoryWrite,AccessMask::MemoryRead});
            for(uint32_t cache=0;cache<2;++cache)
            {
                auto* compose=pass("ComposePages",1,1,1,cache);
                if(!compose) { result.passes.clear(); result.stats=impl.stats; return result; }
                auto& dispatch=compose->computeDispatchInputs.front(); dispatch.indirectArguments=args;
                dispatch.textureVisibilityTransitionsBefore.push_back(TextureTransition(impl.pageAtlas,ResourceState::ShaderWrite));
                dispatch.exportedTextureVisibilityTransitions.push_back(TextureTransition(impl.pageAtlas,ResourceState::ShaderRead));
                DeclareTexture(*compose,impl.pageAtlas,ResourceAccessMode::Write,ResourceState::ShaderWrite);
                DeclareTexture(*compose,impl.meshAtlas,ResourceAccessMode::Read,ResourceState::ShaderRead);
                compose->bufferResourceAccesses.push_back({args,ResourceAccessMode::Read,ResourceState::IndirectArgument,PipelineStageMask::AllCommands,AccessMask::MemoryRead});
            }
        }
        if(constants.probes)
        {
            auto* probe=pass("Probe",(constants.probes+63)/64);
            if(probe) { DeclareTexture(*probe,impl.meshAtlas,ResourceAccessMode::Read,ResourceState::ShaderRead); DeclareTexture(*probe,impl.pageAtlas,ResourceAccessMode::Read,ResourceState::ShaderRead); result.probeResults=probes; }
        }
        if(frame.debug.visualization!=DistanceFieldVisualization::Off && frame.width && frame.height)
        {
            auto* debug=pass("Visualize",(frame.width+7)/8,(frame.height+7)/8);
            if(debug)
            {
                auto& dispatch=debug->computeDispatchInputs.front();
                dispatch.textureVisibilityTransitionsBefore.push_back(TextureTransition(view.debugOutput,ResourceState::ShaderWrite));
                dispatch.textureVisibilityTransitionsBefore.push_back(TextureTransition(impl.pageAtlas,ResourceState::ShaderRead));
                DeclareTexture(*debug,view.debugOutput,ResourceAccessMode::Write,ResourceState::ShaderWrite);
                DeclareTexture(*debug,impl.pageAtlas,ResourceAccessMode::Read,ResourceState::ShaderRead);
                DeclareTexture(*debug,impl.meshAtlas,ResourceAccessMode::Read,ResourceState::ShaderRead);
                if(frame.outputTexture && frame.outputTexture->GetDesc().format==TextureFormat::RGBA8)
                {
                    dispatch.textureCopyTransitionsBefore={TextureTransition(view.debugOutput,ResourceState::CopySrc),TextureTransition(frame.outputTexture,ResourceState::CopyDst)};
                    RHITextureCopyDesc copy; copy.source=view.debugOutput; copy.destination=frame.outputTexture; copy.extent={frame.width,frame.height,1};
                    dispatch.textureCopiesAfter.push_back(copy);
                    dispatch.exportedTextureVisibilityTransitions={TextureTransition(view.debugOutput,ResourceState::ShaderRead),TextureTransition(frame.outputTexture,ResourceState::ShaderRead)};
                    DeclareTexture(*debug,frame.outputTexture,ResourceAccessMode::Write,ResourceState::CopyDst);
                }
            }
        }
        if(!result.passes.empty())
        {
            if(auto query=impl.device->CreateTimestampQuery())
            {
                auto timing=std::make_shared<TimingReadback>(); timing->query=query;
                result.passes.front().computeDispatchInputs.front().timestampBegin=query;
                auto& end=result.passes.back(); end.computeDispatchInputs.back().timestampEnd=query;
                end.bufferResourceAccesses.push_back({query->Results(),ResourceAccessMode::Write,ResourceState::CopyDst,
                    PipelineStageMask::Copy,AccessMask::CopyWrite});
                PostSubmitBufferReadbackRequest request;
                request.desc.source=query->Results(); request.desc.sourceState=ResourceState::CopyDst;
                request.desc.data=timing->ticks.data(); request.desc.size=sizeof(timing->ticks);
                request.desc.debugName="DistanceField.GpuTiming";
                request.state=timing->state; request.destinationKeepAlive=timing;
                result.timingReadback=request; impl.timings.push_back(std::move(timing));
            }
        }
        impl.stats.objects=objects.size(); impl.stats.dirtyPages=dirtyPages.size();
        impl.stats.residentBricks=MeshBudget/512-impl.freeMeshSlots.size(); impl.stats.meshBytes=MeshBudget; impl.stats.pageBytes=PageBudget;
        impl.stats.metadataBytes=objects.size()*sizeof(GpuObject)+indirection.size()*4+dirtyPages.size()*(sizeof(DistanceFieldDirtyPage)+66*4+4)+DistanceFieldViewState::PageTableEntries*4;
        impl.stats.pendingLoads=pendingLoads.load(); impl.stats.revision=runtimeRevision.load();
        impl.stats.updateCpuMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        auto sampling=std::make_shared<DistanceFieldGpuResources>();
        sampling->objects=objectBuffer; sampling->meshIndirection=indirectBuffer; sampling->pageTable=view.table;
        sampling->samplingConstants=Buffer(impl.device,&constants,sizeof(constants),BufferUsageFlags::Uniform,MemoryUsage::CPUToGPU,"DistanceField.SamplingConstants");
        sampling->meshAtlas=impl.meshView; sampling->globalAtlas=impl.pageView; sampling->sampler=impl.sampler;
        sampling->objectCount=constants.objects; sampling->revision=impl.residencyRevision;
        sampling->lifetime=owner; result.gpuResources=std::move(sampling);
        result.stats=impl.stats; return result;
    }
}
