#include "VfxTestExecution.h"
#include "VfxStructuredTestAsset.h"
#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <filesystem>
#include <fstream>
#include <GLFW/glfw3.h>

#include <Guid.h>
#include <VFX/GpuDataSet.h>
#include <VFX/GpuEmitter.h>
#include <VFX/CpuEmitter.h>
#include <VFX/SystemRenderProxy.h>
#include <VFX/ParticleRenderGraph.h>
#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxShaderCompiler.h>
#include <VFX/Authoring/VfxAuthoring.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/RhiCommandListRecorder.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>
#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>
#include "VfxStageTestAsset.h"
#if defined(_WIN32)
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#endif

TEST(VfxGpuTests, Dx12ProgramsCompactionAndParametersMatchCpuState)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 device validation runs on Windows";
#else
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    namespace shader = NLS::Render::ShaderCompiler;
    namespace render = NLS::Render::Context;
    auto& executor = render::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another test owns the global RHI executor";
    executor.Initialize(render::RhiThreadMode::None,{});
    struct Shutdown
    {
        render::RhiCommandListExecutor& executor;
        ~Shutdown() { executor.Shutdown(); }
    } shutdown{executor};
    NLS::Render::Settings::DriverSettings settings;
    settings.debugMode = false;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    auto device = NLS::Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "DX12 compute device is unavailable";
    ASSERT_TRUE(device->GetCapabilities().supportsCompute);
    executor.SetTargets(render::RhiCommandListExecutor::CreateDeviceTargets(device));
    auto asset = CreateDefaultSystem().emitters[0];
    ParticleDataSet cpu;
    cpu.Initialize(asset.attributes,137);
    for (uint32_t index = 0; index < 129; ++index)
    {
        ASSERT_TRUE(cpu.Spawn(static_cast<uint64_t>(index)+0x100000001ull));
        if (index%2) cpu.Write(static_cast<uint32_t>(cpu.FindAttribute("Particles.Age")),index,{3,0,0,0});
    }
    GpuDataSet gpu;
    std::string error;
    ASSERT_TRUE(gpu.Initialize(device,cpu,error)) << error;
    const auto root = std::filesystem::temp_directory_path()/("NullusVfxGpu_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
    shader::ShaderCompiler compiler;
    auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"VfxValidation");
    ASSERT_NE(pool,nullptr);
    auto commands = pool->CreateCommandBuffer("VfxValidation");
    ASSERT_NE(commands,nullptr);
    commands->Begin();
    std::vector<std::shared_ptr<GpuDispatchResources>> keepAlive;
    std::vector<Value> registers;
    for (const auto* graph : {&asset.spawn,&asset.update})
    {
        const auto compiled = CompileGraph(*graph,asset.attributes);
        ASSERT_TRUE(compiled.Succeeded());
        const auto path = root/(graph->id+".hlsl");
        { std::ofstream file(path); file << compiled.compiled.hlsl; }
        shader::ShaderCompilationInput input;
        input.assetPath = path.string(); input.stage = shader::ShaderStage::Compute;
        input.options.targetPlatform = shader::ShaderTargetPlatform::DXIL;
        input.options.targetProfile = "cs_6_0"; input.options.entryPoint = "Main";
        input.options.artifactDirectory = (root/"Artifacts").string();
        const auto output = compiler.Compile(input);
        ASSERT_EQ(output.status,shader::ShaderCompilationStatus::Succeeded) << output.diagnostics;
        rhi::RHIShaderModuleDesc module;
        module.stage = rhi::ShaderStage::Compute; module.targetBackend = rhi::NativeBackendType::DX12;
        module.entryPoint = "Main"; module.bytecode = output.bytecode;
        auto pipeline = gpu.CreatePipeline(module,error);
        ASSERT_NE(pipeline,nullptr) << error;
        const float delta = graph->stage == ScriptStage::ParticleSpawn ? 0 : 0.25f;
        auto dispatch = gpu.Record(*commands,pipeline,cpu.Count(),delta,asset.seed,11,{},error);
        ASSERT_NE(dispatch,nullptr) << error;
        keepAlive.push_back(std::move(dispatch));
        ASSERT_TRUE(RunParticleProgram(compiled.compiled.program, cpu, 0, cpu.Count(), {delta, asset.seed, 11, nullptr}, registers));
    }
    commands->End();
    auto fence = device->CreateFence("VfxValidationComplete");
    ASSERT_NE(fence,nullptr);
    fence->Reset();
    rhi::RHISubmitDesc submit;
    submit.commandBuffers = {commands}; submit.signalFence = fence;
    const auto submitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
    ASSERT_TRUE(submitted.Succeeded()) << submitted.message;
    ASSERT_TRUE(fence->Wait(30000000000ull));
    std::vector<float> values(gpu.ScalarCount()*gpu.Capacity());
    rhi::RHIBufferReadbackDesc read;
    read.source = gpu.AttributeBuffer(); read.sourceState = rhi::ResourceState::ShaderWrite;
    read.size = values.size()*sizeof(float); read.data = values.data(); read.debugName = "VfxValidationReadback";
    const auto readback = device->BeginReadBuffer(read);
    ASSERT_TRUE(readback.Succeeded()) << readback.message;
    if (readback.completion) ASSERT_TRUE(readback.completion->Wait(30000000000ull).Succeeded());
    size_t offset = 0;
    for (uint32_t attribute = 0; attribute < cpu.Attributes().size(); ++attribute)
    {
        for (uint32_t component = 0; component < ComponentCount(cpu.Attributes()[attribute].type); ++component)
        {
            for (uint32_t particle = 0; particle < cpu.Count(); ++particle)
                EXPECT_NEAR(values[offset+particle],cpu.Read(attribute,particle)[component],1e-5f)
                    << cpu.Attributes()[attribute].name << ":" << particle << ":" << component;
            for (uint32_t particle = cpu.Count(); particle < gpu.Capacity(); ++particle)
                EXPECT_EQ(values[offset+particle],0) << "Padded dispatch wrote outside the active particle range";
            offset += gpu.Capacity();
        }
    }
    std::string compactSource;
    ASSERT_TRUE(BuildGpuCompactionShader(cpu.Attributes(),compactSource,error)) << error;
    const auto compactPath = root/"compact.hlsl";
    { std::ofstream file(compactPath); file << compactSource; }
    std::vector<std::shared_ptr<rhi::RHIComputePipeline>> compactionPipelines;
    for (const auto* entry : {"Compact","Finish"})
    {
        shader::ShaderCompilationInput input;
        input.assetPath = compactPath.string(); input.stage = shader::ShaderStage::Compute;
        input.options.targetPlatform = shader::ShaderTargetPlatform::DXIL;
        input.options.targetProfile = "cs_6_0"; input.options.entryPoint = entry;
        input.options.artifactDirectory = (root/"Artifacts").string();
        const auto output = compiler.Compile(input);
        ASSERT_EQ(output.status,shader::ShaderCompilationStatus::Succeeded) << output.diagnostics;
        rhi::RHIShaderModuleDesc module;
        module.stage = rhi::ShaderStage::Compute; module.targetBackend = rhi::NativeBackendType::DX12;
        module.entryPoint = entry; module.bytecode = output.bytecode;
        compactionPipelines.push_back(gpu.CreateCompactionPipeline(module,error));
        ASSERT_NE(compactionPipelines.back(),nullptr) << error;
    }
    auto compactCommands = pool->CreateCommandBuffer("VfxCompactionValidation");
    ASSERT_NE(compactCommands,nullptr);
    compactCommands->Begin();
    auto compactResources = gpu.RecordCompaction(*compactCommands,compactionPipelines[0],compactionPipelines[1],error);
    ASSERT_NE(compactResources,nullptr) << error;
    compactCommands->End();
    fence->Reset();
    submit.commandBuffers = {compactCommands};
    const auto compactSubmit = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
    ASSERT_TRUE(compactSubmit.Succeeded()) << compactSubmit.message;
    ASSERT_TRUE(gpu.CommitCompaction(compactResources));
    ASSERT_TRUE(fence->Wait(30000000000ull));
    std::array<uint32_t,8> counters{};
    const auto readBuffer = [&](std::shared_ptr<rhi::RHIBuffer> buffer, void* data, uint64_t size)
    {
        rhi::RHIBufferReadbackDesc request;
        request.source = std::move(buffer); request.sourceState = request.source->GetState();
        request.size = size; request.data = data; request.debugName = "VfxCompactionValidationReadback";
        const auto result = device->BeginReadBuffer(request);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
    };
    ASSERT_TRUE(readBuffer(gpu.CounterBuffer(),counters.data(),sizeof(counters)));
    ASSERT_EQ(counters[0],65u);
    EXPECT_EQ(counters[1],0u);
    EXPECT_EQ(counters[4],6u);
    EXPECT_EQ(counters[5],65u);
    std::vector<std::array<uint32_t,2>> compactedIds(gpu.Capacity());
    ASSERT_TRUE(readBuffer(gpu.IdBuffer(),compactedIds.data(),compactedIds.size()*sizeof(compactedIds[0])));
    ASSERT_TRUE(readBuffer(gpu.AttributeBuffer(),values.data(),values.size()*sizeof(float)));
    std::vector<uint64_t> liveIds;
    for (uint32_t particle = 0; particle < counters[0]; ++particle)
    {
        const uint64_t id = compactedIds[particle][0] | (static_cast<uint64_t>(compactedIds[particle][1])<<32);
        ASSERT_GE(id,0x100000001ull);
        const auto sourceIndex = static_cast<uint32_t>(id-0x100000001ull);
        ASSERT_LT(sourceIndex,cpu.Count());
        EXPECT_EQ(sourceIndex%2,0u);
        liveIds.push_back(id);
        size_t scalar = 0;
        for (uint32_t attribute = 0; attribute < cpu.Attributes().size(); ++attribute)
            for (uint32_t component = 0; component < ComponentCount(cpu.Attributes()[attribute].type); ++component)
                EXPECT_NEAR(values[(scalar++)*gpu.Capacity()+particle],cpu.Read(attribute,sourceIndex)[component],1e-5f);
    }
    std::sort(liveIds.begin(),liveIds.end());
    EXPECT_EQ(std::unique(liveIds.begin(),liveIds.end()),liveIds.end());
    auto repeated = pool->CreateCommandBuffer("VfxRepeatCompaction");
    ASSERT_NE(repeated,nullptr);
    repeated->Begin();
    auto secondResources = gpu.RecordCompaction(*repeated,compactionPipelines[0],compactionPipelines[1],error,3);
    ASSERT_NE(secondResources,nullptr) << error;
    repeated->End();
    fence->Reset();
    submit.commandBuffers = {repeated};
    ASSERT_TRUE(device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit).Succeeded());
    ASSERT_TRUE(gpu.CommitCompaction(secondResources));
    EXPECT_FALSE(gpu.CommitCompaction(compactResources));
    ASSERT_TRUE(fence->Wait(30000000000ull));
    ASSERT_TRUE(readBuffer(gpu.CounterBuffer(),counters.data(),sizeof(counters)));
    EXPECT_EQ(counters[0],65u);
    EXPECT_EQ(counters[1],0u);
    EXPECT_EQ(counters[5],65u);
    EXPECT_EQ(counters[4],3u);
    Graph parameterGraph;
    parameterGraph.id = NLS::Guid::New().ToString();
    const auto positionNode = AddNode(parameterGraph,Operation::Attribute,ValueType::Float3);
    FindNode(parameterGraph,positionNode)->name = "Particles.Position";
    const auto parameterNode = AddNode(parameterGraph,Operation::Parameter);
    FindNode(parameterGraph,parameterNode)->name = "User.Scale";
    FindNode(parameterGraph,parameterNode)->value = {2,0,0,0};
    const auto multiply = AddNode(parameterGraph,Operation::Multiply);
    FindNode(parameterGraph,multiply)->inputs = {positionNode,parameterNode};
    const auto write = AddNode(parameterGraph,Operation::WriteAttribute);
    FindNode(parameterGraph,write)->name = "Particles.Position";
    FindNode(parameterGraph,write)->inputs = {multiply};
    const auto parameterProgram = CompileGraph(parameterGraph,cpu.Attributes());
    ASSERT_TRUE(parameterProgram.Succeeded());
    const auto parameterPath = root/"parameter.hlsl";
    { std::ofstream file(parameterPath); file << parameterProgram.compiled.hlsl; }
    shader::ShaderCompilationInput parameterInput;
    parameterInput.assetPath = parameterPath.string(); parameterInput.stage = shader::ShaderStage::Compute;
    parameterInput.options.targetPlatform = shader::ShaderTargetPlatform::DXIL;
    parameterInput.options.targetProfile = "cs_6_0"; parameterInput.options.entryPoint = "Main";
    parameterInput.options.artifactDirectory = (root/"Artifacts").string();
    const auto parameterBinary = compiler.Compile(parameterInput);
    ASSERT_EQ(parameterBinary.status,shader::ShaderCompilationStatus::Succeeded) << parameterBinary.diagnostics;
    rhi::RHIShaderModuleDesc parameterModule;
    parameterModule.stage = rhi::ShaderStage::Compute; parameterModule.targetBackend = rhi::NativeBackendType::DX12;
    parameterModule.entryPoint = "Main"; parameterModule.bytecode = parameterBinary.bytecode;
    auto parameterPipeline = gpu.CreatePipeline(parameterModule,error);
    ASSERT_NE(parameterPipeline,nullptr) << error;
    auto parameterCommands = pool->CreateCommandBuffer("VfxParameterValidation");
    ASSERT_NE(parameterCommands,nullptr);
    parameterCommands->Begin();
    std::array<Value,1> parameterValues{Value{3,0,0,0}};
    auto parameterResources = gpu.Record(*parameterCommands,parameterPipeline,cpu.Count(),0,asset.seed,12,parameterValues,error);
    ASSERT_NE(parameterResources,nullptr) << error;
    parameterValues[0][0] = 99;
    parameterCommands->End();
    fence->Reset();
    submit.commandBuffers = {parameterCommands};
    ASSERT_TRUE(device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit).Succeeded());
    ASSERT_TRUE(fence->Wait(30000000000ull));
    ASSERT_TRUE(readBuffer(gpu.IdBuffer(),compactedIds.data(),compactedIds.size()*sizeof(compactedIds[0])));
    ASSERT_TRUE(readBuffer(gpu.AttributeBuffer(),values.data(),values.size()*sizeof(float)));
    const std::unordered_map<std::string,Value> parameters{{"User.Scale",{3,0,0,0}}};
    ASSERT_TRUE(RunParticleProgram(parameterProgram.compiled.program, cpu, 0, cpu.Count(), {0, asset.seed, 12, &parameters}, registers));
    for (uint32_t particle = 0; particle < counters[0]; ++particle)
    {
        const uint64_t id = compactedIds[particle][0] | (static_cast<uint64_t>(compactedIds[particle][1])<<32);
        const auto sourceIndex = static_cast<uint32_t>(id-0x100000001ull);
        ASSERT_LT(sourceIndex,cpu.Count());
        for (uint32_t component = 0; component < 3; ++component)
            EXPECT_NEAR(values[component*gpu.Capacity()+particle],cpu.Read(0,sourceIndex)[component],1e-5f);
    }
#endif
}

static void ValidateViewDepthSorting(const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device,
    const std::shared_ptr<NLS::Render::RHI::RHICommandPool>& pool, const NLS::VFX::EmitterShaderPrograms& shaders)
{
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    std::string error;
    ParticleSorter sorter;
    const auto attributes = CreateDefaultSystem().emitters[0].attributes;
    ASSERT_TRUE(sorter.Initialize(device,attributes,shaders.sort,error)) << error;
    const auto read = [&](const auto& buffer,void* bytes,size_t size)
    {
        rhi::RHIBufferReadbackDesc request;
        request.source = buffer; request.size = size; request.data = bytes;
        const auto result = device->BeginReadBuffer(request);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        if (!result.Succeeded()) return false;
        if (result.completion) return result.completion->Wait(30000000000ull).Succeeded();
        return true;
    };
    const auto submit = [&](const auto& commands)
    {
        commands->End();
        auto fence = device->CreateFence("VfxSortComplete");
        if (!fence) return false;
        fence->Reset();
        rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        const auto result = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && fence->Wait(30000000000ull);
    };
    for (const auto [capacity,count] : {std::pair{1u,0u},std::pair{1u,1u},std::pair{255u,255u},std::pair{256u,256u},
        std::pair{257u,257u},std::pair{2053u,1025u},std::pair{10003u,9001u}})
    {
        SCOPED_TRACE("sort capacity="+std::to_string(capacity)+" count="+std::to_string(count));
        ParticleDataSet cpu; cpu.Initialize(attributes,capacity);
        for (uint32_t index = 0; index < count; ++index)
        {
            ASSERT_TRUE(cpu.Spawn((uint64_t(index%4)<<32)|(count-index+20)));
            cpu.Write(0,index,{float(int(index%17)-8)*0.03125f,0,float(int(index*37%31)-15)*0.015625f,0});
        }
        if (count > 3)
        {
            cpu.Write(0,0,{0,0,std::numeric_limits<float>::quiet_NaN(),0});
            cpu.Write(0,1,{0,0,std::numeric_limits<float>::infinity(),0});
            cpu.Write(0,2,{0,0,-std::numeric_limits<float>::infinity(),0});
        }
        GpuDataSet gpu; ASSERT_TRUE(gpu.Initialize(device,cpu,error)) << error;
        ParticleRenderData gpuData;
        gpuData.attributes = attributes; gpuData.capacity = capacity; gpuData.countUpperBound = capacity;
        gpuData.values = gpu.AttributeBuffer(); gpuData.ids = gpu.IdBuffer(); gpuData.counters = gpu.CounterBuffer();
        auto cpuData = UploadCpuRenderData(device,cpu,error); ASSERT_TRUE(cpuData) << error;
        if (count == 1)
        {
            const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            auto malformed = gpuData; malformed.attributes[0].type = ValueType::Float2;
            EXPECT_FALSE(sorter.Prepare(malformed,identity,{0,0,1,0},error));
            malformed = gpuData; malformed.capacity = 2;
            EXPECT_FALSE(sorter.Prepare(malformed,identity,{0,0,1,0},error));
            EXPECT_FALSE(sorter.Prepare(gpuData,identity,{0,0,0,0},error));
        }
        // Both views are recorded before submission and retain their own permutation.
        auto commands = pool->CreateCommandBuffer("VfxSortViews"); ASSERT_TRUE(commands); commands->Begin();
        std::array<std::shared_ptr<const ParticleSortResources>,3> views;
        const std::array<float,16> transform{1,0,0,0,0,1,0,0,-2,0,0.5f,0.25f,0,0,0,1};
        for (uint32_t view = 0; view < views.size(); ++view)
        {
            views[view] = sorter.Prepare(view == 2 ? *cpuData : gpuData,transform,{0,0,view == 1 ? -1.0f : 1.0f,0},error);
            ASSERT_TRUE(views[view]) << error;
            ASSERT_TRUE(ParticleSorter::Record(*commands,*views[view],error)) << error;
        }
        ASSERT_NE(views[0]->keys,views[1]->keys);
        ASSERT_TRUE(submit(commands));
        for (uint32_t view = 0; view < views.size(); ++view)
        {
            std::vector<uint32_t> expected(count); std::iota(expected.begin(),expected.end(),0);
            const auto depth = [&](uint32_t index)
            {
                const auto position = cpu.Read(0,index);
                float value = (-2*position[0]+0.5f*position[2]+0.25f)*(view == 1 ? -1.0f : 1.0f);
                return std::isfinite(value) ? value : -(std::numeric_limits<float>::max)();
            };
            std::sort(expected.begin(),expected.end(),[&](uint32_t a,uint32_t b)
            { return depth(a) != depth(b) ? depth(a) > depth(b) : cpu.Id(a) < cpu.Id(b); });
            std::vector<std::array<uint32_t,4>> keys(views[view]->keys->GetDesc().size/16);
            ASSERT_TRUE(read(views[view]->keys,keys.data(),keys.size()*16));
            for (uint32_t slot = 0; slot < keys.size(); ++slot)
            {
                if (slot < count)
                {
                    ASSERT_EQ(keys[slot][3],expected[slot]) << "view=" << view << " slot=" << slot;
                    EXPECT_EQ((uint64_t(keys[slot][1])<<32)|keys[slot][2],cpu.Id(expected[slot]));
                }
                else ASSERT_EQ(keys[slot][3],~0u) << "Padding must follow all live particles";
            }
        }
        // Sorting never changes the source identity stream or indirect live count.
        std::vector<uint64_t> ids(capacity);
        ASSERT_TRUE(read(gpuData.ids,ids.data(),ids.size()*8));
        for (uint32_t index = 0; index < count; ++index) EXPECT_EQ(ids[index],cpu.Id(index));
        std::array<uint32_t,8> counters{};
        ASSERT_TRUE(read(gpuData.counters,counters.data(),sizeof(counters)));
        EXPECT_EQ(counters[0],count); EXPECT_EQ(counters[4],6u); EXPECT_EQ(counters[5],count);
        size_t scalars = 0;
        for (const auto& attribute : attributes) scalars += ComponentCount(attribute.type);
        std::vector<float> expected(scalars*capacity), actual(expected.size());
        size_t scalar = 0;
        for (uint32_t attribute = 0; attribute < attributes.size(); ++attribute)
            for (uint32_t component = 0; component < ComponentCount(attributes[attribute].type); ++component,++scalar)
                for (uint32_t particle = 0; particle < count; ++particle)
                    expected[scalar*capacity+particle] = cpu.Read(attribute,particle)[component];
        ASSERT_TRUE(read(gpuData.values,actual.data(),actual.size()*sizeof(float)));
        EXPECT_EQ(std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(float)),0);
    }

    // The permutation must affect alpha composition in the vertex shader.
    ParticleDataSet overlapping; overlapping.Initialize(attributes,3);
    ASSERT_TRUE(overlapping.Spawn(1)); ASSERT_TRUE(overlapping.Spawn(2));
    overlapping.Write(0,0,{0,0,0.25f,0}); overlapping.Write(0,1,{0,0,0.75f,0});
    overlapping.Write(2,0,{0,0,1,0.5f}); overlapping.Write(2,1,{1,0,0,0.5f});
    overlapping.Write(3,0,{0.8f,0,0,0}); overlapping.Write(3,1,{0.8f,0,0,0});
    GpuDataSet gpu; ASSERT_TRUE(gpu.Initialize(device,overlapping,error)) << error;
    std::array<std::vector<uint8_t>,3> images;
    for (uint32_t view = 0; view < images.size(); ++view)
    {
        SpriteRenderer renderer;
        rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {rhi::TextureFormat::RGBA8};
        ASSERT_TRUE(renderer.Initialize(device,attributes,shaders.spriteVertex,shaders.spriteFragment,targets,error,view ? &shaders.sort : nullptr)) << error;
        rhi::RHITextureDesc desc; desc.extent = {32,32,1}; desc.format = rhi::TextureFormat::RGBA8;
        desc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
        auto texture = device->CreateTexture(desc); ASSERT_TRUE(texture);
        rhi::RHITextureViewDesc target; target.format = desc.format;
        auto output = device->CreateTextureView(texture,target); ASSERT_TRUE(output);
        auto commands = pool->CreateCommandBuffer("VfxSortedBlend"); ASSERT_TRUE(commands); commands->Begin();
        SpriteView camera; if (view == 2) camera.cameraForward[2] = -1;
        rhi::RHIRenderPassDesc pass; pass.renderArea = {0,0,32,32};
        rhi::RHIRenderPassColorAttachmentDesc attachment; attachment.view = output; attachment.clearValue = {0,0,0,1};
        pass.colorAttachments = {attachment};
        const auto draw = renderer.Record(*commands,gpu,camera,pass,error); ASSERT_TRUE(draw) << error;
        ASSERT_TRUE(submit(commands));
        auto& pixels = images[view]; pixels.resize(32*32*4);
        const auto result = device->BeginReadPixels(texture,0,0,32,32,NLS::Render::Settings::EPixelDataFormat::RGBA,
            NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
        ASSERT_TRUE(result.Succeeded()) << result.message;
        if (result.completion) ASSERT_TRUE(result.completion->Wait(30000000000ull).Succeeded());
        const auto center = (16*32+16)*4;
        if (view == 1) EXPECT_GT(pixels[center+2],pixels[center]+40);
        else EXPECT_GT(pixels[center],pixels[center+2]+40);
    }
    EXPECT_EQ(images[0],images[2]); EXPECT_NE(images[0],images[1]);
}

static void ValidateBoundsAndVisibility(const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device,
    const std::shared_ptr<NLS::Render::RHI::RHICommandPool>& pool, const NLS::VFX::EmitterShaderPrograms& shaders,
    const std::vector<NLS::VFX::Attribute>& attributes)
{
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    std::string error;
    ParticleBoundsCalculator calculator;
    ASSERT_TRUE(calculator.Initialize(device,attributes,shaders.simulation.bounds,error)) << error;
    ParticleVisibility visibility;
    ASSERT_TRUE(visibility.Initialize(device,attributes,shaders.visibility,error)) << error;
    ParticleSorter sorter;
    ASSERT_TRUE(sorter.Initialize(device,attributes,shaders.sort,error)) << error;
    const auto read = [&](const auto& buffer,void* bytes,size_t size)
    {
        rhi::RHIBufferReadbackDesc request; request.source = buffer; request.data = bytes; request.size = size;
        const auto result = device->BeginReadBuffer(request);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
    };
    const auto submit = [&](const auto& commands)
    {
        commands->End(); auto fence = device->CreateFence("VfxVisibilityComplete"); if (!fence) return false;
        fence->Reset(); rhi::RHISubmitDesc submission; submission.commandBuffers = {commands}; submission.signalFence = fence;
        const auto result = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submission);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && fence->Wait(30000000000ull);
    };
    for (const auto [capacity,count] : {std::pair{1u,0u},std::pair{1u,1u},std::pair{137u,129u},std::pair{257u,257u},
        std::pair{2053u,1031u},std::pair{65539u,65537u}})
    {
        SCOPED_TRACE("visibility capacity="+std::to_string(capacity)+" count="+std::to_string(count));
        ParticleDataSet cpu; cpu.Initialize(attributes,capacity);
        for (uint32_t index = 0; index < count; ++index)
        {
            ASSERT_TRUE(cpu.Spawn((uint64_t(index%7)<<32)|(count-index+1)));
            cpu.Write(0,index,{float(int(index*17%63)-31)*0.125f,float(int(index*13%31)-15)*0.125f,
                float(int(index*7%17)-4)*0.125f,0});
            cpu.Write(3,index,{index%5 == 0 ? -1.5f : 0.125f,index%3 == 0 ? 2.0f : 0.25f,0,0});
        }
        if (count > 3)
        { cpu.Write(0,0,{0,0,std::numeric_limits<float>::quiet_NaN(),0}); cpu.Write(3,1,{std::numeric_limits<float>::infinity(),0,0,0}); }
        if (attributes[3].type == ValueType::Float2 && count > 3)
            cpu.Write(3,2,{0,std::numeric_limits<float>::quiet_NaN(),0,0});
        GpuDataSet gpu; ASSERT_TRUE(gpu.Initialize(device,cpu,error)) << error;
        ParticleRenderData data; data.attributes = attributes; data.capacity = capacity; data.countUpperBound = capacity;
        data.values = gpu.AttributeBuffer(); data.ids = gpu.IdBuffer(); data.counters = gpu.CounterBuffer();
        auto uploaded = UploadCpuRenderData(device,cpu,error); ASSERT_TRUE(uploaded) << error;
        auto commands = pool->CreateCommandBuffer("VfxBoundsViews"); ASSERT_TRUE(commands); commands->Begin();
        const auto bounds = calculator.Prepare(data,error); ASSERT_TRUE(bounds) << error;
        NLS::Render::Context::RecordComputeDispatchInputs(*commands,bounds->dispatches); data.bounds = bounds->bounds;
        std::array<SpriteView,4> cameras;
        cameras[1].localToWorld = {0,-0.25f,0,0.25f,-2,0,0,0.5f,0,0,0.5f,0.25f,0,0,0,1};
        cameras[1].cameraRight = {0,1,0,0}; cameras[1].cameraUp = {-1,0,0,0}; cameras[1].cameraForward = {0,0,-1,0};
        cameras[2].localToWorld[3] = 100; cameras[3].frustumCulling = false;
        std::array<std::shared_ptr<const ParticleVisibilityResources>,4> views;
        std::array<std::shared_ptr<const ParticleSortResources>,4> sorted;
        for (uint32_t view = 0; view < views.size(); ++view)
        {
            views[view] = visibility.Prepare(view == 3 ? *uploaded : data,cameras[view],error); ASSERT_TRUE(views[view]) << error;
            NLS::Render::Context::RecordComputeDispatchInputs(*commands,views[view]->dispatches);
            if (view == 1)
            {
                sorted[view] = sorter.Prepare(data,cameras[view].localToWorld,cameras[view].cameraForward,error,views[view]->keys);
                ASSERT_TRUE(sorted[view]) << error; ASSERT_TRUE(ParticleSorter::Record(*commands,*sorted[view],error)) << error;
            }
        }
        ASSERT_NE(views[0]->arguments,views[1]->arguments); ASSERT_NE(views[0]->keys,views[1]->keys);
        ASSERT_TRUE(submit(commands));
        const auto expectedBounds = CalculateParticleBounds(cpu);
        ParticleBounds actualBounds{}; ASSERT_TRUE(read(bounds->bounds,&actualBounds,sizeof(actualBounds)));
        EXPECT_EQ(actualBounds.minimum,expectedBounds.minimum); EXPECT_EQ(actualBounds.maximum,expectedBounds.maximum);
        EXPECT_NEAR(actualBounds.spriteRadius,expectedBounds.spriteRadius,1e-5f);
        EXPECT_EQ(actualBounds.particleCount,count); EXPECT_EQ(actualBounds.invalidCount,expectedBounds.invalidCount);
        for (uint32_t view = 0; view < views.size(); ++view)
        {
            const auto expected = CalculateParticleVisibility(cpu,cameras[view]);
            ParticleViewStatistics actual; ASSERT_TRUE(read(views[view]->arguments,&actual,sizeof(actual)));
            EXPECT_EQ(actual.particleCount,expected.particleCount); EXPECT_EQ(actual.visibleCount,expected.visibleCount);
            EXPECT_EQ(actual.culledCount,expected.culledCount); EXPECT_EQ(actual.invalidCount,expected.invalidCount);
            ASSERT_EQ(actual.instanceCount,expected.visibleCount); EXPECT_EQ(actual.vertexCount,6u);
            std::vector<std::array<uint32_t,4>> keys(views[view]->keys->GetDesc().size/16);
            ASSERT_TRUE(read(views[view]->keys,keys.data(),keys.size()*16));
            std::vector<uint8_t> visited(count); uint32_t last = 0;
            for (uint32_t slot = 0; slot < keys.size(); ++slot)
            {
                if (slot >= actual.visibleCount) { ASSERT_EQ(keys[slot][3],~0u); continue; }
                const uint32_t particle = keys[slot][3]; ASSERT_LT(particle,count); ASSERT_FALSE(visited[particle]); visited[particle] = 1;
                ParticleDataSet single; single.Initialize(attributes,1); ASSERT_TRUE(single.Spawn(cpu.Id(particle)));
                single.Write(0,0,cpu.Read(0,particle)); single.Write(3,0,cpu.Read(3,particle));
                ASSERT_EQ(CalculateParticleVisibility(single,cameras[view]).visibleCount,1u);
                if (view != 1 && slot) EXPECT_GT(particle,last); // Stable compaction before optional depth sorting.
                if (view == 1 && slot) EXPECT_LT(keys[slot-1],keys[slot]);
                last = particle;
            }
        }
        std::array<uint32_t,8> source{}; ASSERT_TRUE(read(data.counters,source.data(),sizeof(source)));
        EXPECT_EQ(source[0],count); EXPECT_EQ(source[5],count);
    }
}

static void ValidateGrowthAndSimulationStages(const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device,
    const std::shared_ptr<NLS::Render::RHI::RHICommandPool>& pool, const std::filesystem::path& root, NLS::Render::Context::Driver& driver)
{
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    namespace render = NLS::Render::Context;
    auto system = NLS::Tests::CreateStagedParticleTestAsset();
    CompiledSystem compiled; std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(system,compiled,diagnostics));
    const auto backend = device->GetAdapter()->GetBackendType();
    ASSERT_TRUE(CompilePlatformPrograms(compiled,std::span(&backend,1),root/"Stages",diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    std::string error; ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled,false),compiled,error)) << error;
    const auto& asset = system.emitters[0]; const auto& program = compiled.emitters[0];
    const auto& shaders = compiled.FindPlatform(backend)->emitters[0];
    ASSERT_EQ(shaders.simulation.stages.size(),5u); EXPECT_TRUE(program.stages[0].hlsl.empty());
    GpuEmitter gpu; ASSERT_TRUE(gpu.Initialize(device,asset,program,shaders.simulation,error)) << error;
    EXPECT_EQ(gpu.Data().Capacity(),3u);
    {
        auto commands = pool->CreateCommandBuffer("AbandonedGrowingStageFrame"); ASSERT_TRUE(commands); commands->Begin();
        auto abandoned = gpu.RecordFrame(*commands,1,0.1,error); ASSERT_TRUE(abandoned) << error;
        EXPECT_GT(abandoned->Data().Capacity(),3u); EXPECT_EQ(gpu.Data().Capacity(),3u); EXPECT_DOUBLE_EQ(gpu.Age(),0);
        EXPECT_FALSE(gpu.RecordFrame(*commands,1,0.1,error));
        commands->End(); abandoned.reset(); // Discard the unsubmitted transaction.
    }
    auto cpuSettings = asset; cpuSettings.target = SimulationTarget::CPU; cpuSettings.simulationStages.clear();
    CpuEmitter cpu; ASSERT_TRUE(cpu.Initialize(cpuSettings,program.spawn,program.update,error)) << error;
    std::unordered_map<std::string,Value> parameters;
    std::unordered_map<uint64_t,Value> positions;
    ParticleDataSet expected; expected.Initialize(asset.attributes,asset.capacity);
    const auto advance = [&](double delta)
    {
        const auto steps = (std::max)(1u,static_cast<uint32_t>(std::ceil(delta*60.0-1e-12)));
        for (uint32_t step = 0; step < steps; ++step)
        {
            EXPECT_TRUE(cpu.Tick(delta/steps)); expected = cpu.Data();
            for (uint32_t particle = 0; particle < expected.Count(); ++particle)
                if (const auto found = positions.find(expected.Id(particle)); found != positions.end()) expected.Write(0,particle,found->second);
            std::vector<Value> registers;
            for (uint32_t stage = 0; stage < asset.simulationStages.size(); ++stage)
            {
                const auto& settings = asset.simulationStages[stage]; if (!settings.enabled) continue;
                for (uint32_t iteration = 0; iteration < settings.iterations; ++iteration)
                    EXPECT_TRUE(RunParticleProgram(program.stages[stage].program, expected, 0, expected.Count(), {static_cast<float>(delta / steps), asset.seed, cpu.Statistics().simulationSteps - 1, &parameters, stage, iteration}, registers));
            }
            std::vector<uint8_t> alive(expected.Count());
            for (uint32_t particle = 0; particle < expected.Count(); ++particle)
            {
                positions[expected.Id(particle)] = expected.Read(0,particle);
                alive[particle] = expected.Read(4,particle)[0] < expected.Read(5,particle)[0] && expected.Read(5,particle)[0] > 0;
            }
            expected.Compact(alive);
        }
    };
    const auto read = [&](const auto& buffer,void* bytes,size_t size)
    {
        rhi::RHIBufferReadbackDesc request; request.source = buffer; request.data = bytes; request.size = size;
        const auto result = device->BeginReadBuffer(request); EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
    };
    const auto compare = [&](const ParticleRenderData& actual,const ParticleDataSet& reference)
    {
        std::array<uint32_t,8> counters{}; ASSERT_TRUE(read(actual.counters,counters.data(),sizeof(counters)));
        ASSERT_EQ(counters[0],reference.Count()); EXPECT_EQ(counters[5],reference.Count());
        std::vector<uint64_t> ids(actual.capacity); std::vector<float> values(actual.values->GetDesc().size/4);
        ASSERT_TRUE(read(actual.ids,ids.data(),ids.size()*8)); ASSERT_TRUE(read(actual.values,values.data(),values.size()*4));
        std::unordered_map<uint64_t,uint32_t> lookup;
        for (uint32_t index = 0; index < reference.Count(); ++index) lookup.emplace(reference.Id(index),index);
        for (uint32_t index = 0; index < counters[0]; ++index)
        {
            const auto found = lookup.find(ids[index]); ASSERT_NE(found,lookup.end()); size_t scalar = 0;
            for (uint32_t attribute = 0; attribute < asset.attributes.size(); ++attribute)
                for (uint32_t component = 0; component < ComponentCount(asset.attributes[attribute].type); ++component)
                    EXPECT_NEAR(values[scalar++*actual.capacity+index],reference.Read(attribute,found->second)[component],1e-5f)
                        << "id=" << ids[index] << " attribute=" << asset.attributes[attribute].name;
            if (reference.Count()) EXPECT_NEAR(values[10*actual.capacity+index],0.32f,1e-6f);
            lookup.erase(found);
        }
        EXPECT_TRUE(lookup.empty()); ParticleBounds bounds{}; ASSERT_TRUE(read(actual.bounds,&bounds,sizeof(bounds)));
        const auto expectedBounds = CalculateParticleBounds(reference); EXPECT_EQ(bounds.particleCount,reference.Count());
        for (uint32_t component = 0; component < 3; ++component)
        { EXPECT_NEAR(bounds.minimum[component],expectedBounds.minimum[component],1e-5f); EXPECT_NEAR(bounds.maximum[component],expectedBounds.maximum[component],1e-5f); }
    };
    uint64_t frame = 0; bool paused = false;
    const auto tick = [&](double delta)
    {
        auto commands = pool->CreateCommandBuffer("VfxGrowthStages"); if (!commands) return std::shared_ptr<const ParticleRenderData>{};
        commands->Begin(); auto packet = gpu.RecordFrame(*commands,++frame,delta,error);
        EXPECT_TRUE(packet) << error; if (!packet) return std::shared_ptr<const ParticleRenderData>{};
        const auto snapshot = packet->Data().RecordSnapshot(*commands,error); EXPECT_TRUE(snapshot) << error;
        commands->End(); auto fence = device->CreateFence("VfxGrowthStagesComplete"); if (!fence) return std::shared_ptr<const ParticleRenderData>{};
        fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        const auto submitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_TRUE(submitted.Succeeded()) << submitted.message;
        if (!submitted.Succeeded() || !gpu.CommitFrame(packet) || !fence->Wait(30000000000ull)) return std::shared_ptr<const ParticleRenderData>{};
        if (delta > 0 && !paused) advance(delta);
        if (snapshot) compare(*snapshot,expected); return snapshot;
    };
    const auto early = tick(1.0/60); ASSERT_TRUE(early) << error; const auto earlyReference = expected;
    EXPECT_GT(gpu.Data().Capacity(),3u); EXPECT_LT(gpu.Data().Capacity(),asset.capacity);
    uint32_t previous = gpu.Data().Capacity();
    for (const double delta : {0.04,0.09,0.12,0.2})
    {
        parameters["User.StageOffset"] = {static_cast<float>(delta),0.2f,0,0}; ASSERT_TRUE(gpu.SetParameter("User.StageOffset",parameters["User.StageOffset"]));
        ASSERT_TRUE(tick(delta)); EXPECT_GE(gpu.Data().Capacity(),previous); previous = gpu.Data().Capacity();
    }
    EXPECT_EQ(gpu.Data().Capacity(),asset.capacity); EXPECT_EQ(expected.Count(),asset.capacity);
    paused = true; gpu.SetPaused(true); cpu.SetPaused(true); const auto age = gpu.Age();
    ASSERT_TRUE(tick(0.4)); EXPECT_DOUBLE_EQ(gpu.Age(),age);
    paused = false; gpu.SetPaused(false); cpu.SetPaused(false);
    parameters["User.StageLifetime"] = {}; ASSERT_TRUE(gpu.SetParameter("User.StageLifetime",{}));
    ASSERT_TRUE(tick(1.0/60)); EXPECT_EQ(expected.Count(),0u); compare(*early,earlyReference);
    ASSERT_TRUE(gpu.Reset()); cpu.Reset(); positions.clear(); expected.Clear();
    parameters["User.StageLifetime"] = {10,0,0,0}; ASSERT_TRUE(gpu.SetParameter("User.StageLifetime",parameters["User.StageLifetime"]));
    ASSERT_TRUE(tick(0)); EXPECT_EQ(gpu.Data().Capacity(),asset.capacity);
    ASSERT_TRUE(tick(1.0/60)); compare(*early,earlyReference);

    // Execute a growing, odd-compaction frame through logical FrameGraph imports.
    auto graphEmitter = std::make_shared<GpuEmitter>(); ASSERT_TRUE(graphEmitter->Initialize(device,asset,program,shaders.simulation,error)) << error;
    auto renderer = std::make_shared<SpriteRenderer>(); rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {rhi::TextureFormat::RGBA8};
    ASSERT_TRUE(renderer->Initialize(device,asset.attributes,shaders.spriteVertex,shaders.spriteFragment,targets,error,&shaders.sort,&shaders.visibility,&shaders.simulation.bounds)) << error;
    render::RhiCommandListExecutor::Get().Initialize(render::RhiThreadMode::None,render::RhiCommandListExecutor::CreateDeviceTargets(device));
    auto commands = pool->CreateCommandBuffer("VfxGrowingGraph"); auto list = std::make_shared<render::RhiCommandList>();
    rhi::RHIFrameContext context; context.resourceStateTracker = rhi::CreateDefaultResourceStateTracker();
    render::RhiCommandListRecorder recorder(*list,*commands,context.resourceStateTracker); recorder.Begin();
    NLS::Render::FrameGraph::FrameGraphExecutionContext execution{driver,device.get(),&recorder,&context}; ::FrameGraph graph;
    const auto simulation = AddGpuSimulationPass(graph,graphEmitter,1,0.1);
    rhi::RHITextureDesc desc; desc.extent = {64,64,1}; desc.format = rhi::TextureFormat::RGBA8;
    desc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
    auto texture = device->CreateTexture(desc); ASSERT_TRUE(texture); rhi::RHITextureViewDesc viewDesc; viewDesc.format = desc.format;
    auto output = device->CreateTextureView(texture,viewDesc); ASSERT_TRUE(output);
    const auto color = graph.import<NLS::Render::FrameGraph::FrameGraphTexture>("GrowingSprites",desc,NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(texture,output));
    const auto draw = AddSpriteRenderPass(graph,simulation,renderer,SpriteView{},color,-1,rhi::LoadOp::Clear);
    graph.compile(); ASSERT_NO_THROW(graph.execute(&execution,&execution)); recorder.End();
    ASSERT_TRUE(list->TranslateNative(*commands,&error)) << error;
    auto fence = device->CreateFence("VfxGrowingGraphComplete"); ASSERT_TRUE(fence); fence->Reset();
    rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
    ASSERT_TRUE(device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit).Succeeded());
    list->CommitResourceStates(*commands); ASSERT_TRUE(simulation->Commit());
    ASSERT_TRUE(fence->Wait(30000000000ull)); EXPECT_GT(graphEmitter->Data().Capacity(),3u); ASSERT_TRUE(draw->draw);
    ParticleViewStatistics statistics; ASSERT_TRUE(read(draw->draw->arguments,&statistics,sizeof(statistics))); EXPECT_GT(statistics.visibleCount,0u);
    std::vector<uint8_t> pixels(64*64*4); const auto image = device->BeginReadPixels(texture,0,0,64,64,NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data()); ASSERT_TRUE(image.Succeeded());
    if (image.completion) ASSERT_TRUE(image.completion->Wait(30000000000ull).Succeeded());
    bool lit = false; for (size_t index = 0; index < pixels.size(); index += 4) lit |= pixels[index] > 0; EXPECT_TRUE(lit);
}

static void ValidateEmitterLifecycleAndRendering(bool vulkan)
{
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    namespace shader = NLS::Render::ShaderCompiler;
    namespace render = NLS::Render::Context;
    auto& executor = render::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another test owns the global RHI executor";
    executor.Initialize(render::RhiThreadMode::None,{});
    struct Shutdown { render::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    struct Window
    {
        GLFWwindow* handle = nullptr;
        ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } }
    } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan validation requires GLFW WSI initialization";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(128,128,"VFX device validation",nullptr,nullptr);
        if (!window.handle) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI window is unavailable"; }
    }
    NLS::Render::Settings::DriverSettings settings;
    settings.debugMode = false;
    settings.graphicsBackend = vulkan ? NLS::Render::Settings::EGraphicsBackend::VULKAN : NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.platformWindow = window.handle;
    auto device = NLS::Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested GPU backend is unavailable";
    const auto validationBefore = NLS::Render::Backend::GetVulkanValidationErrorCountForTesting();
    executor.SetTargets(render::RhiCommandListExecutor::CreateDeviceTargets(device));
    auto system = CreateDefaultSystem();
    auto& asset = system.emitters[0];
    asset.target = SimulationTarget::GPU; asset.capacity = 137;
    asset.spawnRate = 9000; asset.duration = 0.05; asset.bursts = {{0,9},{0.009,3},{0.025,2}};
    asset.attributes.back().defaultValue = {0.0473f,0,0,0};
    const auto parameter = AddNode(asset.spawn,Operation::Parameter,ValueType::Float3);
    FindNode(asset.spawn,parameter)->name = "User.Velocity";
    FindNode(asset.spawn,parameter)->value = {0,3,0,0};
    const auto write = AddNode(asset.spawn,Operation::WriteAttribute,ValueType::Float3);
    FindNode(asset.spawn,write)->name = "Particles.Velocity"; FindNode(asset.spawn,write)->inputs = {parameter};
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(system,compiled,diagnostics));
    const auto root = std::filesystem::temp_directory_path()/("NullusVfxEmitter_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
    shader::ShaderCompiler compiler;
    const auto compile = [&](const std::string& source, const char* entry, shader::ShaderStage stage = shader::ShaderStage::Compute)
    {
        const auto path = root/(std::string(entry)+".hlsl");
        { std::ofstream file(path); file << source; }
        shader::ShaderCompilationInput input;
        input.assetPath = path.string(); input.stage = stage;
        input.options.targetPlatform = vulkan ? shader::ShaderTargetPlatform::SPIRV : shader::ShaderTargetPlatform::DXIL;
        input.options.targetProfile = stage == shader::ShaderStage::Vertex ? "vs_6_0" : stage == shader::ShaderStage::Pixel ? "ps_6_0" : "cs_6_0";
        input.options.entryPoint = entry; input.options.artifactDirectory = (root/"Artifacts").string();
        const auto binary = compiler.Compile(input);
        EXPECT_EQ(binary.status,shader::ShaderCompilationStatus::Succeeded) << binary.diagnostics;
        rhi::RHIShaderModuleDesc module;
        module.stage = stage == shader::ShaderStage::Vertex ? rhi::ShaderStage::Vertex : stage == shader::ShaderStage::Pixel ? rhi::ShaderStage::Fragment : rhi::ShaderStage::Compute;
        module.targetBackend = vulkan ? rhi::NativeBackendType::Vulkan : rhi::NativeBackendType::DX12;
        module.entryPoint = entry; module.bytecode = binary.bytecode; return module;
    };
    const auto backend = device->GetAdapter()->GetBackendType();
    ASSERT_TRUE(CompilePlatformPrograms(compiled,std::span(&backend,1),root/"Cook",diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    std::string error;
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled,false),compiled,error)) << error;
    ASSERT_TRUE(ValidatePlatformPrograms(compiled,backend,error)) << error;
    const auto& program = compiled.emitters[0];
    EXPECT_TRUE(program.spawnHlsl.empty()); EXPECT_TRUE(program.updateHlsl.empty()); EXPECT_TRUE(program.compactionHlsl.empty());
    const auto& packaged = compiled.FindPlatform(backend)->emitters[0];
    const auto& shaders = packaged.simulation;
    GpuEmitter gpu;
    ASSERT_TRUE(gpu.Initialize(device,asset,program,shaders,error)) << error;
    CpuEmitter cpu;
    auto cpuAsset = asset; cpuAsset.target = SimulationTarget::CPU;
    ASSERT_TRUE(cpu.Initialize(cpuAsset,program.spawn,program.update,error)) << error;
    auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"VfxLifecycleValidation");
    ASSERT_NE(pool,nullptr);
    struct Submitted
    {
        std::shared_ptr<rhi::RHICommandBuffer> commands;
        std::shared_ptr<const GpuFramePacket> packet;
        std::shared_ptr<rhi::RHIFence> fence;
    };
    std::vector<Submitted> submitted;
    uint64_t frame = 0;
    const auto tick = [&](double delta)
    {
        auto commands = pool->CreateCommandBuffer("VfxLifecycleFrame");
        if (!commands) return false;
        commands->Begin();
        auto packet = gpu.RecordFrame(*commands,++frame,delta,error);
        EXPECT_NE(packet,nullptr) << error;
        if (!packet) return false;
        EXPECT_DOUBLE_EQ(gpu.Age(),cpu.Age());
        EXPECT_FALSE(gpu.RecordFrame(*commands,frame,delta,error));
        EXPECT_FALSE(gpu.Reset());
        commands->End();
        auto fence = device->CreateFence("VfxLifecycleFrameComplete");
        if (!fence) return false;
        fence->Reset();
        rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        const auto result = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        if (!result.Succeeded() || !gpu.CommitFrame(packet)) return false;
        EXPECT_FALSE(gpu.CommitFrame(packet));
        submitted.push_back({std::move(commands),std::move(packet),std::move(fence)});
        return cpu.Tick(delta);
    };
    const auto compare = [&]()
    {
        ASSERT_FALSE(submitted.empty());
        ASSERT_TRUE(submitted.back().fence->Wait(30000000000ull));
        const auto read = [&](std::shared_ptr<rhi::RHIBuffer> buffer, void* bytes, uint64_t size)
        {
            rhi::RHIBufferReadbackDesc desc;
            desc.source = std::move(buffer); desc.sourceState = desc.source->GetState(); desc.size = size;
            desc.data = bytes; desc.debugName = "VfxLifecycleReadback";
            const auto result = device->BeginReadBuffer(desc);
            EXPECT_TRUE(result.Succeeded()) << result.message;
            return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
        };
        std::array<uint32_t,8> counters{};
        ASSERT_TRUE(read(gpu.Data().CounterBuffer(),counters.data(),sizeof(counters)));
        ASSERT_EQ(counters[0],cpu.Data().Count()) << "frame " << frame;
        ParticleBounds bounds{}; ASSERT_TRUE(read(gpu.Data().BoundsBuffer(),&bounds,sizeof(bounds)));
        EXPECT_EQ(bounds.particleCount,cpu.Bounds().particleCount); EXPECT_EQ(bounds.invalidCount,cpu.Bounds().invalidCount);
        for (uint32_t component = 0; component < 3; ++component)
        {
            EXPECT_NEAR(bounds.minimum[component],cpu.Bounds().minimum[component],1e-5f);
            EXPECT_NEAR(bounds.maximum[component],cpu.Bounds().maximum[component],1e-5f);
        }
        EXPECT_EQ(counters[1],0u); EXPECT_EQ(counters[5],counters[0]);
        std::vector<std::array<uint32_t,2>> ids(gpu.Data().Capacity());
        std::vector<float> values(gpu.Data().ScalarCount()*gpu.Data().Capacity());
        ASSERT_TRUE(read(gpu.Data().IdBuffer(),ids.data(),ids.size()*sizeof(ids[0])));
        ASSERT_TRUE(read(gpu.Data().AttributeBuffer(),values.data(),values.size()*sizeof(float)));
        std::unordered_map<uint64_t,uint32_t> cpuIndices;
        for (uint32_t index = 0; index < cpu.Data().Count(); ++index) cpuIndices.emplace(cpu.Data().Id(index),index);
        for (uint32_t index = 0; index < counters[0]; ++index)
        {
            const auto id = ids[index][0] | (static_cast<uint64_t>(ids[index][1])<<32);
            const auto found = cpuIndices.find(id);
            ASSERT_NE(found,cpuIndices.end()) << "Missing ID " << id << " at frame " << frame;
            size_t scalar = 0;
            for (uint32_t attribute = 0; attribute < cpu.Data().Attributes().size(); ++attribute)
                for (uint32_t component = 0; component < ComponentCount(cpu.Data().Attributes()[attribute].type); ++component)
                    EXPECT_NEAR(values[scalar++*gpu.Data().Capacity()+index],cpu.Data().Read(attribute,found->second)[component],1e-5f)
                        << "frame=" << frame << " id=" << id << " attribute=" << cpu.Data().Attributes()[attribute].name;
            cpuIndices.erase(found);
        }
        EXPECT_TRUE(cpuIndices.empty());
        EXPECT_DOUBLE_EQ(gpu.Age(),cpu.Age());
        submitted.clear();
    };
    for (uint32_t index = 0; index < 8; ++index)
    {
        const Value velocity{static_cast<float>(index),3,0,0};
        ASSERT_TRUE(gpu.SetParameter("User.Velocity",velocity)); ASSERT_TRUE(cpu.SetParameter("User.Velocity",velocity));
        ASSERT_TRUE(tick(index%2 ? 0.031 : 1.0/60));
    }
    EXPECT_GT(cpu.Statistics().dropped,0u);
    EXPECT_GT(cpu.Statistics().died,0u);
    compare();
    gpu.SetPaused(true); cpu.SetPaused(true);
    ASSERT_TRUE(tick(0.2)); compare();
    gpu.SetPaused(false); cpu.SetPaused(false);
    ASSERT_TRUE(gpu.Deactivate()); cpu.Deactivate();
    ASSERT_TRUE(tick(0.1)); compare();
    EXPECT_TRUE(cpu.IsComplete());
    ASSERT_TRUE(gpu.Reset()); cpu.Reset();
    ASSERT_TRUE(tick(0.031)); compare();
    ASSERT_TRUE(gpu.Deactivate(true)); cpu.Deactivate(true);
    ASSERT_TRUE(tick(0)); compare();

    // The scene runtime owns mixed emitters and publishes immutable device data.
    // Advance several frames before reading the first one to catch buffer aliasing.
    auto mixed = std::make_shared<CompiledSystem>(compiled);
    auto cpuSettings = mixed->asset.emitters[0]; cpuSettings.target = SimulationTarget::CPU; cpuSettings.id += "-cpu";
    mixed->asset.emitters.push_back(cpuSettings); mixed->emitters.push_back(program);
    for (auto& platform : mixed->platforms) platform.emitters.push_back(platform.emitters[0]);
    SystemInstance world;
    ASSERT_TRUE(world.Initialize(mixed,error)) << error;
    auto proxy = std::make_shared<SystemRenderProxy>(mixed);
    auto prepare = [&]()
    {
        auto result = proxy->Prepare(device,world.Capture(),error);
        EXPECT_NE(result,nullptr) << error;
        if (result) EXPECT_EQ(proxy->Prepare(device,world.Capture(),error),result) << error;
        return result;
    };
    ASSERT_NE(prepare(),nullptr);
    ASSERT_TRUE(world.SetParameter("User.Velocity",{2,3,0,0})); ASSERT_NE(prepare(),nullptr);
    ASSERT_TRUE(world.Tick(0.031));
    auto firstWorldFrame = world.Capture();
    auto firstRenderFrame = prepare(); ASSERT_NE(firstRenderFrame,nullptr);
    for (uint32_t index = 0; index < 4; ++index)
    {
        ASSERT_TRUE(world.SetParameter("User.Velocity",{static_cast<float>(index+3),3,0,0})); ASSERT_NE(prepare(),nullptr);
        ASSERT_TRUE(world.Tick(0.031));
        const auto current = prepare(); ASSERT_NE(current,nullptr);
        EXPECT_NE(current->emitters[0]->values,firstRenderFrame->emitters[0]->values);
        EXPECT_NE(current->emitters[1]->values,firstRenderFrame->emitters[1]->values);
    }
    const auto compareFrozen = [&](const SystemRenderFrame& snapshot, const ParticleDataSet& expected)
    {
        ASSERT_EQ(snapshot.emitters.size(),2u);
        for (const auto& data : snapshot.emitters)
        {
            const auto read = [&](const auto& buffer, void* bytes, uint64_t size)
            {
                rhi::RHIBufferReadbackDesc desc; desc.source = buffer; desc.sourceState = buffer->GetState(); desc.size = size; desc.data = bytes;
                const auto result = device->BeginReadBuffer(desc);
                EXPECT_TRUE(result.Succeeded()) << result.message;
                return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
            };
            std::array<uint32_t,8> counts{}; ASSERT_TRUE(read(data->counters,counts.data(),sizeof(counts)));
            ASSERT_EQ(counts[0],expected.Count()); EXPECT_EQ(counts[4],6u); EXPECT_EQ(counts[5],expected.Count());
            ParticleBounds bounds{}; ASSERT_TRUE(read(data->bounds,&bounds,sizeof(bounds)));
            const auto expectedBounds = CalculateParticleBounds(expected);
            EXPECT_EQ(bounds.particleCount,expectedBounds.particleCount); EXPECT_EQ(bounds.invalidCount,expectedBounds.invalidCount);
            EXPECT_NEAR(bounds.spriteRadius,expectedBounds.spriteRadius,1e-5f);
            for (uint32_t component = 0; component < 3; ++component)
            {
                EXPECT_NEAR(bounds.minimum[component],expectedBounds.minimum[component],1e-5f);
                EXPECT_NEAR(bounds.maximum[component],expectedBounds.maximum[component],1e-5f);
            }
            std::vector<float> values(data->values->GetDesc().size/sizeof(float));
            std::vector<std::array<uint32_t,2>> ids(data->capacity);
            ASSERT_TRUE(read(data->values,values.data(),data->values->GetDesc().size));
            ASSERT_TRUE(read(data->ids,ids.data(),data->ids->GetDesc().size));
            std::unordered_map<uint64_t,uint32_t> expectedIds;
            for (uint32_t index = 0; index < expected.Count(); ++index) expectedIds.emplace(expected.Id(index),index);
            for (uint32_t index = 0; index < counts[0]; ++index)
            {
                const auto id = ids[index][0]|(static_cast<uint64_t>(ids[index][1])<<32);
                const auto found = expectedIds.find(id); ASSERT_NE(found,expectedIds.end());
                size_t scalar = 0;
                for (uint32_t attribute = 0; attribute < expected.Attributes().size(); ++attribute)
                    for (uint32_t component = 0; component < ComponentCount(expected.Attributes()[attribute].type); ++component)
                        EXPECT_NEAR(values[scalar++*data->capacity+index],expected.Read(attribute,found->second)[component],1e-5f);
                expectedIds.erase(found);
            }
            EXPECT_TRUE(expectedIds.empty());
        }
    };
    compareFrozen(*firstRenderFrame,*firstWorldFrame->cpu[1]);
    compareFrozen(*prepare(),*world.Capture()->cpu[1]);
    world.SetPaused(true); ASSERT_NE(prepare(),nullptr);
    const auto pausedFrame = world.Capture(); ASSERT_TRUE(world.Tick(0.5)); EXPECT_EQ(world.Capture(),pausedFrame);
    world.SetPaused(false); ASSERT_NE(prepare(),nullptr);
    world.Deactivate(); ASSERT_NE(prepare(),nullptr); ASSERT_TRUE(world.Tick(0.1));
    const auto drained = prepare(); ASSERT_NE(drained,nullptr); compareFrozen(*drained,*world.Capture()->cpu[1]);
    EXPECT_EQ(world.Capture()->cpu[1]->Count(),0u);
    world.Reset(); ASSERT_NE(prepare(),nullptr); ASSERT_TRUE(world.Tick(0.031));
    const auto resetFrame = prepare(); ASSERT_NE(resetFrame,nullptr); compareFrozen(*resetFrame,*world.Capture()->cpu[1]);
    world.Deactivate(true); const auto cleared = prepare(); ASSERT_NE(cleared,nullptr); compareFrozen(*cleared,*world.Capture()->cpu[1]);
    ASSERT_TRUE(world.Tick(0.01)); ASSERT_TRUE(world.Tick(0.01));
    EXPECT_FALSE(proxy->Prepare(device,world.Capture(),error)); EXPECT_NE(error.find("skipped"),std::string::npos);
    proxy.reset();
    compareFrozen(*firstRenderFrame,*firstWorldFrame->cpu[1]);

    auto renderer = std::make_shared<SpriteRenderer>();
    rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {rhi::TextureFormat::RGBA8};
    ASSERT_TRUE(renderer->Initialize(device,asset.attributes,packaged.spriteVertex,packaged.spriteFragment,targets,error,&packaged.sort,
        &packaged.visibility,&packaged.simulation.bounds)) << error;
    auto emitter = std::make_shared<GpuEmitter>();
    ASSERT_TRUE(emitter->Initialize(device,asset,program,shaders,error)) << error;
    NLS::Render::Settings::DriverSettings nullSettings;
    nullSettings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    render::Driver contextDriver(nullSettings);
    render::DriverTestAccess::PauseThreadedRenderingWorkers(contextDriver);
    executor.Initialize(render::RhiThreadMode::None,render::RhiCommandListExecutor::CreateDeviceTargets(device));
    // The device and executor are owned by this test; Driver only supplies the FrameGraph context.
    rhi::RHIFrameContext frameContext;
    frameContext.resourceStateTracker = rhi::CreateDefaultResourceStateTracker();
    auto nativeCommands = pool->CreateCommandBuffer("VfxSpriteFrameGraph");
    auto list = std::make_shared<render::RhiCommandList>();
    list->SetDebugName("VfxSpriteFrameGraph");
    render::RhiCommandListRecorder recorder(*list,*nativeCommands,frameContext.resourceStateTracker);
    recorder.Begin();
    NLS::Render::FrameGraph::FrameGraphExecutionContext execution{contextDriver,device.get(),&recorder,&frameContext};
    ::FrameGraph graph;
    const auto simulation = AddGpuSimulationPass(graph,emitter,1,1.0/60);
    std::vector<std::shared_ptr<rhi::RHITexture>> textures;
    std::vector<std::shared_ptr<SpriteRenderPass>> views;
    for (uint32_t viewIndex = 0; viewIndex < 3; ++viewIndex)
    {
        rhi::RHITextureDesc desc;
        desc.extent = {128,128,1}; desc.format = rhi::TextureFormat::RGBA8;
        desc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
        desc.debugName = "VfxSpriteReadback";
        auto texture = device->CreateTexture(desc); ASSERT_NE(texture,nullptr);
        rhi::RHITextureViewDesc viewDesc; viewDesc.format = desc.format;
        auto output = device->CreateTextureView(texture,viewDesc); ASSERT_NE(output,nullptr);
        const auto color = graph.import<NLS::Render::FrameGraph::FrameGraphTexture>("SpriteColor",desc,
            NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(texture,output));
        SpriteView view; view.localToWorld[11] = 0.5f;
        if (viewIndex) view.localToWorld[3] = 0.15f;
        if (viewIndex == 2) view.localToWorld[3] = 100;
        views.push_back(AddSpriteRenderPass(graph,simulation,renderer,view,color,-1,rhi::LoadOp::Clear));
        textures.push_back(std::move(texture));
    }
    graph.compile(); graph.execute(&execution,&execution); recorder.End();
    std::string diagnostic;
    ASSERT_TRUE(list->TranslateNative(*nativeCommands,&diagnostic)) << diagnostic;
    auto drawFence = device->CreateFence("VfxSpriteReadbackComplete"); drawFence->Reset();
    rhi::RHISubmitDesc drawSubmit; drawSubmit.commandBuffers = {nativeCommands}; drawSubmit.signalFence = drawFence;
    ASSERT_TRUE(device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(drawSubmit).Succeeded());
    list->CommitResourceStates(*nativeCommands);
    ASSERT_TRUE(simulation->Commit());
    ASSERT_TRUE(drawFence->Wait(30000000000ull));
    EXPECT_DOUBLE_EQ(emitter->Age(),1.0/60);
    std::array<std::vector<uint8_t>,3> images;
    for (uint32_t index = 0; index < textures.size(); ++index)
    {
        auto& pixels = images[index]; pixels.resize(128*128*4);
        const auto result = device->BeginReadPixels(textures[index],0,0,128,128,
            NLS::Render::Settings::EPixelDataFormat::RGBA,NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
        ASSERT_TRUE(result.Succeeded()) << result.message;
        if (result.completion) ASSERT_TRUE(result.completion->Wait(30000000000ull).Succeeded());
        uint32_t lit = 0;
        for (uint32_t pixel = 0; pixel < 128*128; ++pixel) if (pixels[pixel*4] > 0) ++lit;
        if (index == 2) EXPECT_EQ(lit,0u) << "A fully culled view must issue zero instances";
        else EXPECT_GT(lit,20u) << "Indirect Sprite draw produced no particle image";
    }
    EXPECT_NE(images[0],images[1]) << "Each view must use its own captured transform";
    ValidateViewDepthSorting(device,pool,packaged);
    ValidateBoundsAndVisibility(device,pool,packaged,asset.attributes);
    auto rectangularAttributes = asset.attributes; rectangularAttributes[3].type = ValueType::Float2;
    auto rectangularShaders = packaged;
    std::string boundsSource;
    ASSERT_TRUE(BuildGpuBoundsShader(rectangularAttributes,boundsSource,error)) << error;
    rectangularShaders.simulation.bounds.reduce = compile(boundsSource,"BoundsReduce");
    rectangularShaders.simulation.bounds.finish = compile(boundsSource,"BoundsFinish");
    ASSERT_TRUE(BuildGpuVisibilityShader(rectangularAttributes,boundsSource,error)) << error;
    rectangularShaders.visibility.cull = compile(boundsSource,"CullSprites");
    rectangularShaders.visibility.scan = compile(boundsSource,"ScanVisibility");
    rectangularShaders.visibility.scatter = compile(boundsSource,"ScatterVisibility");
    ValidateBoundsAndVisibility(device,pool,rectangularShaders,rectangularAttributes);
    ValidateGrowthAndSimulationStages(device,pool,root,contextDriver);

    const std::string indirectSource = R"(
RWStructuredBuffer<uint> Output : register(u0,space1);
RWStructuredBuffer<uint> Args : register(u1,space1);
[numthreads(1,1,1)] void PrepareIndirect(uint3 id : SV_DispatchThreadID)
{
    Output[0] = 0;
    Args[4] = 3; Args[5] = 2; Args[6] = 1;
    Args[8] = 6; Args[9] = 1; Args[10] = 0; Args[11] = 0; Args[12] = 0;
}
[numthreads(1,1,1)] void RunIndirect(uint3 id : SV_DispatchThreadID)
{
    InterlockedAdd(Output[0],1);
}
)";
    const auto createBuffer = [&](size_t size, rhi::BufferUsageFlags usage)
    {
        rhi::RHIBufferDesc desc; desc.size = size; desc.usage = usage; desc.debugName = "VfxIndirectValidation";
        return device->CreateBuffer(desc);
    };
    auto arguments = createBuffer(64,rhi::BufferUsageFlags::Storage|rhi::BufferUsageFlags::Indirect|rhi::BufferUsageFlags::CopySrc);
    auto output = createBuffer(4,rhi::BufferUsageFlags::Storage|rhi::BufferUsageFlags::CopySrc);
    ASSERT_NE(arguments,nullptr); ASSERT_NE(output,nullptr);
    rhi::RHIBindingLayoutDesc bindingLayout;
    bindingLayout.entries = {
        {"Output",rhi::BindingType::StorageBuffer,rhi::BindingPointMap::kPassDescriptorSet,0,1,rhi::ShaderStageMask::Compute,rhi::BindingPointMap::kPassBindingSpace,4},
        {"Args",rhi::BindingType::StorageBuffer,rhi::BindingPointMap::kPassDescriptorSet,1,1,rhi::ShaderStageMask::Compute,rhi::BindingPointMap::kPassBindingSpace,4}};
    auto layout = device->CreateBindingLayout(bindingLayout); ASSERT_NE(layout,nullptr);
    rhi::RHIPipelineLayoutDesc pipelineLayout;
    pipelineLayout.bindingLayouts.resize(rhi::BindingPointMap::kPassDescriptorSet+1);
    pipelineLayout.bindingLayouts[rhi::BindingPointMap::kPassDescriptorSet] = layout;
    auto computeLayout = device->CreatePipelineLayout(pipelineLayout); ASSERT_NE(computeLayout,nullptr);
    std::vector<std::shared_ptr<rhi::RHIComputePipeline>> pipelines;
    for (const auto* entry : {"PrepareIndirect","RunIndirect"})
    {
        rhi::RHIComputePipelineDesc pipeline;
        pipeline.pipelineLayout = computeLayout; pipeline.computeShader = device->CreateShaderModule(compile(indirectSource,entry));
        pipelines.push_back(device->CreateComputePipeline(pipeline)); ASSERT_NE(pipelines.back(),nullptr);
    }
    rhi::RHIBindingSetDesc bindings; bindings.layout = layout;
    for (uint32_t index = 0; index < 2; ++index)
    {
        rhi::RHIBindingSetEntry entry; entry.binding = index; entry.type = rhi::BindingType::StorageBuffer;
        entry.buffer = index ? arguments : output; entry.bufferRange = entry.buffer->GetDesc().size; entry.elementStride = 4;
        bindings.entries.push_back(std::move(entry));
    }
    auto binding = device->CreateBindingSet(bindings); ASSERT_NE(binding,nullptr);
    const std::array<uint32_t,6> indices{0,1,2,3,4,5};
    rhi::RHIBufferDesc indexDesc; indexDesc.size = sizeof(indices); indexDesc.usage = rhi::BufferUsageFlags::Index;
    rhi::RHIBufferUploadDesc upload; upload.data = indices.data(); upload.dataSize = sizeof(indices);
    auto indexBuffer = device->CreateBuffer(indexDesc,upload); ASSERT_NE(indexBuffer,nullptr);
    auto indirectCommands = pool->CreateCommandBuffer("VfxIndirectCommands"); ASSERT_NE(indirectCommands,nullptr);
    indirectCommands->Begin();
    rhi::RHIBarrierDesc before;
    for (const auto& buffer : {arguments,output})
        before.bufferBarriers.push_back({buffer,buffer->GetState(),rhi::ResourceState::ShaderWrite,rhi::PipelineStageMask::AllCommands,
            rhi::PipelineStageMask::ComputeShader,rhi::AccessMask::MemoryRead|rhi::AccessMask::MemoryWrite,rhi::AccessMask::ShaderWrite});
    indirectCommands->Barrier(before);
    indirectCommands->BindComputePipeline(pipelines[0]); indirectCommands->BindBindingSet(rhi::BindingPointMap::kPassDescriptorSet,binding);
    indirectCommands->Dispatch(1,1,1);
    rhi::RHIBarrierDesc ready;
    ready.bufferBarriers = {
        {arguments,rhi::ResourceState::ShaderWrite,rhi::ResourceState::IndirectArgument,rhi::PipelineStageMask::ComputeShader,
            rhi::PipelineStageMask::DrawIndirect,rhi::AccessMask::ShaderWrite,rhi::AccessMask::IndirectRead},
        {output,rhi::ResourceState::ShaderWrite,rhi::ResourceState::ShaderWrite,rhi::PipelineStageMask::ComputeShader,
            rhi::PipelineStageMask::ComputeShader,rhi::AccessMask::ShaderWrite,rhi::AccessMask::ShaderWrite}};
    indirectCommands->Barrier(ready);
    indirectCommands->BindComputePipeline(pipelines[1]); indirectCommands->BindBindingSet(rhi::BindingPointMap::kPassDescriptorSet,binding);
    EXPECT_FALSE(indirectCommands->DispatchIndirect(arguments,1).Succeeded());
    EXPECT_FALSE(indirectCommands->DispatchIndirect(arguments,UINT64_MAX).Succeeded());
    EXPECT_FALSE(indirectCommands->DispatchIndirect(output,0).Succeeded());
    ASSERT_TRUE(indirectCommands->DispatchIndirect(arguments,16).Succeeded());
    rhi::RHIBarrierDesc indexReady;
    indexReady.bufferBarriers = {{indexBuffer,indexBuffer->GetState(),rhi::ResourceState::IndexBuffer,rhi::PipelineStageMask::AllCommands,
        rhi::PipelineStageMask::VertexInput,rhi::AccessMask::MemoryWrite,rhi::AccessMask::IndexRead}};
    indirectCommands->Barrier(indexReady);
    rhi::RHITextureViewDesc viewDesc; viewDesc.format = rhi::TextureFormat::RGBA8;
    rhi::RHIRenderPassDesc pass; pass.renderArea = {0,0,128,128};
    rhi::RHIRenderPassColorAttachmentDesc color; color.view = device->CreateTextureView(textures[0],viewDesc);
    pass.colorAttachments = {color};
    indirectCommands->BeginRenderPass(pass); indirectCommands->SetViewport({0,0,128,128,0,1}); indirectCommands->SetScissor(pass.renderArea);
    indirectCommands->BindGraphicsPipeline(views[0]->draw->pipeline);
    indirectCommands->BindBindingSet(rhi::BindingPointMap::kPassDescriptorSet,views[0]->draw->bindings);
    indirectCommands->BindIndexBuffer({indexBuffer,0,rhi::IndexType::UInt32});
    ASSERT_TRUE(indirectCommands->DrawIndexedIndirect(arguments,32).Succeeded());
    indirectCommands->EndRenderPass(); indirectCommands->End();
    auto indirectFence = device->CreateFence("VfxIndirectComplete"); indirectFence->Reset();
    rhi::RHISubmitDesc indirectSubmit; indirectSubmit.commandBuffers = {indirectCommands}; indirectSubmit.signalFence = indirectFence;
    const auto indirectSubmitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(indirectSubmit);
#if defined(_WIN32)
    if (!indirectSubmitted.Succeeded() && !vulkan)
    {
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
        auto* native = static_cast<ID3D12Device*>(device->GetNativeDeviceInfo().device);
        if (native && SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&messages))))
            for (UINT64 index = 0; index < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
            {
                SIZE_T size = 0; if (FAILED(messages->GetMessage(index,nullptr,&size))) continue;
                std::vector<uint8_t> bytes(size); auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                if (SUCCEEDED(messages->GetMessage(index,message,&size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    ADD_FAILURE() << message->pDescription;
            }
    }
#endif
    ASSERT_TRUE(indirectSubmitted.Succeeded()) << indirectSubmitted.message;
    uint32_t dispatched = 99;
    rhi::RHIBufferReadbackDesc read;
    read.source = output; read.sourceState = output->GetState(); read.size = sizeof(dispatched); read.data = &dispatched;
    const auto readback = device->BeginReadBuffer(read); ASSERT_TRUE(readback.Succeeded()) << readback.message;
    if (readback.completion) ASSERT_TRUE(readback.completion->Wait(30000000000ull).Succeeded());
    EXPECT_EQ(dispatched,6u);
    if (readback.completion)
    {
        dispatched = 99;
        EXPECT_TRUE(readback.completion->Poll().Succeeded());
        EXPECT_EQ(dispatched,99u) << "A completed token must not write the destination a second time";
    }
    std::array<uint32_t,3> groupCounts{};
    read.source = arguments; read.sourceState = arguments->GetState(); read.sourceOffset = 16;
    read.size = sizeof(groupCounts); read.data = groupCounts.data();
    const auto partialReadback = device->BeginReadBuffer(read); ASSERT_TRUE(partialReadback.Succeeded()) << partialReadback.message;
    if (partialReadback.completion) ASSERT_TRUE(partialReadback.completion->Wait(30000000000ull).Succeeded());
    EXPECT_EQ(groupCounts,(std::array<uint32_t,3>{3,2,1}));
    read.sourceOffset = UINT64_MAX;
    EXPECT_FALSE(device->BeginReadBuffer(read).Succeeded());
    ASSERT_TRUE(indirectFence->Wait(30000000000ull));
    auto& pixels = images[0]; std::fill(pixels.begin(),pixels.end(),0);
    const auto indexedReadback = device->BeginReadPixels(textures[0],0,0,128,128,NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
    ASSERT_TRUE(indexedReadback.Succeeded()) << indexedReadback.message;
    if (indexedReadback.completion) ASSERT_TRUE(indexedReadback.completion->Wait(30000000000ull).Succeeded());
    EXPECT_TRUE(std::any_of(pixels.begin(),pixels.end(),[offset = uint32_t{0}](uint8_t value) mutable
        { return (offset++%4 == 0) && value > 0; })) << "Indexed indirect draw produced no Sprite";
    if (vulkan) EXPECT_EQ(NLS::Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}

TEST(VfxGpuTests, Dx12EmitterLifecycleAndFrameGraphSpritesMatchCpu)
{
#if defined(_WIN32)
    ValidateEmitterLifecycleAndRendering(false);
#else
    GTEST_SKIP() << "DX12 device validation runs on Windows";
#endif
}

TEST(VfxGpuTests, VulkanEmitterLifecycleAndFrameGraphSpritesMatchCpu)
{
    ValidateEmitterLifecycleAndRendering(true);
}

namespace
{
void ValidateGpuEventExchange(bool vulkan)
{
#if !defined(_WIN32)
    if (!vulkan) GTEST_SKIP() << "DX12 device validation runs on Windows";
#endif
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    namespace render = NLS::Render::Context;
    auto& executor = render::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another test owns the global RHI executor";
    executor.Initialize(render::RhiThreadMode::None,{});
    struct Shutdown
    {
        render::RhiCommandListExecutor& executor;
        ~Shutdown() { executor.Shutdown(); }
    } shutdown{executor};
    struct Window
    {
        GLFWwindow* handle = nullptr;
        ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } }
    } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan validation requires GLFW WSI initialization";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(64,64,"VFX events",nullptr,nullptr);
        if (!window.handle) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI window is unavailable"; }
    }
    NLS::Render::Settings::DriverSettings settings;
    settings.debugMode = false;
    settings.graphicsBackend = vulkan ? NLS::Render::Settings::EGraphicsBackend::VULKAN : NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.platformWindow = window.handle;
    auto device = NLS::Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested compute device is unavailable";
    executor.SetTargets(render::RhiCommandListExecutor::CreateDeviceTargets(device));

    // The publisher spawns particles at random positions. The receiver has no
    // spawn script of its own, so every particle it owns comes from a spawn
    // event the other emitter published on the GPU.
    auto system = CreateDefaultSystem("GPU event exchange");
    auto& publisher = system.emitters[0];
    publisher.target = SimulationTarget::GPU;
    publisher.capacity = 32; publisher.initialCapacity = 8;
    publisher.spawnRate = 0; publisher.bursts = {{0,4}};
    publisher.duration = 1.0e6;
    publisher.update.nodes.clear(); publisher.update.nextNodeId = 1;
    auto receiver = publisher;
    receiver.id = NLS::Guid::New().ToString();
    receiver.spawn.nodes.clear(); receiver.spawn.nextNodeId = 1;
    receiver.update.nodes.clear(); receiver.update.nextNodeId = 1;
    receiver.bursts.clear();
    receiver.eventGenerationMask = 0;
    receiver.eventHandlers.clear();
    EmitterAsset::EventHandler handler;
    handler.id = NLS::Guid::New().ToString();
    handler.name = "Receive spawn events";
    handler.receiver.eventKind = ParticleEventSelection::Spawn;
    handler.receiver.spawnCount = 1;
    handler.receiver.executionMode = EventExecutionMode::SpawnedParticles;
    receiver.eventHandlers.push_back(handler);
    system.emitters.push_back(receiver);

    CompiledSystem compiled; std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(system,compiled,diagnostics)) << (diagnostics.empty()?"":diagnostics.front().message);
    const auto backend = device->GetAdapter()->GetBackendType();
    const auto root = std::filesystem::temp_directory_path()/("NullusVfxGpuEvents_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
    ASSERT_TRUE(CompilePlatformPrograms(compiled,std::span(&backend,1),root/"Cook",diagnostics)) << (diagnostics.empty()?"":diagnostics[0].message);
    // A fresh compiler invocation must load native bytecode from disk. This
    // also detects keys accidentally including the already-populated platforms.
    CompiledSystem diskDecoded; std::string decodingError;
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled),diskDecoded,decodingError)) << decodingError;
    PlatformCompileCacheStats cacheStats;
    auto reloaded = compiled;
    ASSERT_TRUE(CompilePlatformPrograms(reloaded,std::span(&backend,1),root/"Cook",diagnostics,nullptr,&cacheStats));
    EXPECT_EQ(cacheStats.hits,1u); EXPECT_EQ(cacheStats.shaderCompilations,0u);
    EXPECT_EQ(reloaded.platforms[0].emitters[1].simulation.receiveEvents.bytecode,
        compiled.platforms[0].emitters[1].simulation.receiveEvents.bytecode);
    std::string error;
    auto shared = std::make_shared<CompiledSystem>(compiled);
    SystemInstance world;
    ASSERT_TRUE(world.Initialize(shared,error)) << error;
    auto proxy = std::make_shared<SystemRenderProxy>(shared);
    ASSERT_NE(proxy->Prepare(device,world.Capture(),error),nullptr) << error;
    ASSERT_TRUE(world.Tick(1.0/60.0));
    const auto snapshot = proxy->Prepare(device,world.Capture(),error);
    ASSERT_NE(snapshot,nullptr) << error;
    ASSERT_EQ(snapshot->emitters.size(),2u);
    const auto read = [&](std::shared_ptr<rhi::RHIBuffer> buffer,void* bytes,uint64_t size)
    {
        rhi::RHIBufferReadbackDesc desc;
        desc.source = buffer; desc.sourceState = buffer->GetState(); desc.size = size; desc.data = bytes;
        const auto result = device->BeginReadBuffer(desc);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        if (result.completion) EXPECT_TRUE(result.completion->Wait(30000000000ull).Succeeded());
    };
    std::array<uint32_t,8> publisherCounts{}; read(snapshot->emitters[0]->counters,publisherCounts.data(),sizeof(publisherCounts));
    std::array<uint32_t,8> receiverCounts{}; read(snapshot->emitters[1]->counters,receiverCounts.data(),sizeof(receiverCounts));
    EXPECT_GT(publisherCounts[0],0u) << "the publisher must spawn particles";
    EXPECT_EQ(receiverCounts[0],publisherCounts[0]) << "the receiver must create one particle per published event";
    if (!publisherCounts[0]) return;
    std::vector<float> publisherValues(snapshot->emitters[0]->values->GetDesc().size/sizeof(float));
    std::vector<float> receiverValues(snapshot->emitters[1]->values->GetDesc().size/sizeof(float));
    read(snapshot->emitters[0]->values,publisherValues.data(),snapshot->emitters[0]->values->GetDesc().size);
    read(snapshot->emitters[1]->values,receiverValues.data(),snapshot->emitters[1]->values->GetDesc().size);
    const auto publisherStride = snapshot->emitters[0]->capacity;
    const auto receiverStride = snapshot->emitters[1]->capacity;
    for (uint32_t index = 0; index < publisherCounts[0]; ++index)
        for (uint32_t component = 0; component < 3; ++component)
            EXPECT_NEAR(receiverValues[component*receiverStride+index],publisherValues[component*publisherStride+index],1e-5f)
                << "the receiver must spawn at the published event position, particle " << index << " component " << component;
    // No new burst on the next substep: the old event count must not replay.
    ASSERT_TRUE(world.Tick(1.0/30.0));
    auto next = proxy->Prepare(device,world.Capture(),error);
    ASSERT_NE(next,nullptr) << error;
    std::array<uint32_t,8> nextCounts{}; read(next->emitters[1]->counters,nextCounts.data(),sizeof(nextCounts));
    EXPECT_EQ(nextCounts[0],receiverCounts[0]);
    // The immutable old publication remains readable after advancing the source.
    std::array<uint32_t,8> oldCounts{}; read(snapshot->emitters[1]->counters,oldCounts.data(),sizeof(oldCounts));
    EXPECT_EQ(oldCounts[0],receiverCounts[0]);
    const auto runModes = [&](bool cpuSource)
    {
        auto modes = CreateDefaultSystem("Independent GPU handlers");
        auto publisher2 = modes.emitters.front();
        publisher2.target = cpuSource ? SimulationTarget::CPU : SimulationTarget::GPU;
        publisher2.capacity = 32; publisher2.initialCapacity = 8; publisher2.spawnRate = 0;
        publisher2.bursts = {{0,4}}; publisher2.duration = 1.0e6;
        publisher2.eventGenerationMask = EventBit(ParticleEventSelection::Spawn);
        publisher2.update.nodes.clear(); publisher2.update.nextNodeId = 1;
        auto receiver2 = publisher2; receiver2.id = NLS::Guid::New().ToString(); receiver2.target = SimulationTarget::GPU;
        receiver2.bursts = {{0,2}}; receiver2.eventGenerationMask = 0; receiver2.eventHandlers.clear();
        const auto handler = [&](EventExecutionMode mode,float increment,uint32_t maxEvents)
        {
            EmitterAsset::EventHandler h; h.id = NLS::Guid::New().ToString(); h.name = "Handler";
            h.receiver.sourceEmitter = publisher2.id; h.receiver.eventKind = ParticleEventSelection::Spawn;
            h.receiver.executionMode = mode; h.receiver.maxEventsPerFrame = maxEvents; h.receiver.spawnCount = 1;
            auto& graph = h.graph; graph.id = NLS::Guid::New().ToString(); graph.stage = ScriptStage::Event;
            const auto constant = AddNode(graph,Operation::Constant); FindNode(graph,constant)->value = {increment,0,0,0};
            if (mode == EventExecutionMode::SpawnedParticles)
            {
                const auto write = AddNode(graph,Operation::WriteAttribute); FindNode(graph,write)->name = "Emitter.ReceiveSpawnCount";
                FindNode(graph,write)->inputs = {constant};
                h.receiver.randomAge = 0.025f;
                const auto badAge = AddNode(graph,Operation::Constant); FindNode(graph,badAge)->value = {4000,0,0,0};
                const auto ageWrite = AddNode(graph,Operation::WriteAttribute); FindNode(graph,ageWrite)->name = "Emitter.ReceiveRandomAge";
                FindNode(graph,ageWrite)->inputs = {badAge};
            }
            else
            {
                const auto attribute = AddNode(graph,Operation::Attribute); FindNode(graph,attribute)->name = "Particles.Size";
                auto amount = constant;
                if (mode == EventExecutionMode::SingleParticle)
                {
                    const auto random = AddNode(graph,Operation::Random); FindNode(graph,random)->value = {1,0,0,0};
                    amount = AddNode(graph,Operation::Add); FindNode(graph,amount)->inputs = {constant,random};
                }
                const auto sum = AddNode(graph,Operation::Add); FindNode(graph,sum)->inputs = {attribute,amount};
                const auto write = AddNode(graph,Operation::WriteAttribute); FindNode(graph,write)->name = "Particles.Size";
                FindNode(graph,write)->inputs = {sum};
            }
            return h;
        };
        receiver2.eventHandlers.push_back(handler(EventExecutionMode::EveryParticle,1,1));
        receiver2.eventHandlers.push_back(handler(EventExecutionMode::SingleParticle,10,0));
        receiver2.eventHandlers.push_back(handler(EventExecutionMode::SpawnedParticles,2,1));
        // Event aging must bind typed update parameters with their integer bits intact.
        auto& motion = receiver2.update;
        const auto speed = AddNode(motion,Operation::Parameter,ValueType::Int32);
        FindNode(motion,speed)->name = "User.EventSpeed"; FindNode(motion,speed)->value = {std::bit_cast<float>(int32_t{2}),0,0,0};
        const auto floating = AddNode(motion,Operation::Convert); FindNode(motion,floating)->inputs = {speed};
        const auto dt = AddNode(motion,Operation::DeltaTime);
        const auto travel = AddNode(motion,Operation::Multiply); FindNode(motion,travel)->inputs = {floating,dt};
        const auto oldPosition = AddNode(motion,Operation::Attribute,ValueType::Float3); FindNode(motion,oldPosition)->name = "Particles.Position";
        const auto newPosition = AddNode(motion,Operation::Add,ValueType::Float3); FindNode(motion,newPosition)->inputs = {oldPosition,travel};
        const auto positionWrite = AddNode(motion,Operation::WriteAttribute,ValueType::Float3);
        FindNode(motion,positionWrite)->name = "Particles.Position"; FindNode(motion,positionWrite)->inputs = {newPosition};
        // Reverse asset order: the publisher must still execute first on each substep.
        modes.emitters = {receiver2,publisher2};
        CompiledSystem program;
        ASSERT_TRUE(CompileSystem(modes,program,diagnostics)) << (diagnostics.empty()?"":diagnostics[0].message);
        ASSERT_TRUE(CompilePlatformPrograms(program,std::span(&backend,1),root/(cpuSource?"CpuModes":"GpuModes"),diagnostics))
            << (diagnostics.empty()?"":diagnostics[0].message);
        auto frozen = std::make_shared<CompiledSystem>(program);
        SystemInstance instance; ASSERT_TRUE(instance.Initialize(frozen,error)) << error;
        SystemRenderProxy renderProxy(frozen); ASSERT_NE(renderProxy.Prepare(device,instance.Capture(),error),nullptr) << error;
        ASSERT_TRUE(instance.Tick(1.0/30.0));
        auto view = renderProxy.Prepare(device,instance.Capture(),error); ASSERT_NE(view,nullptr) << error;
        std::array<uint32_t,8> counts{}; read(view->emitters[0]->counters,counts.data(),sizeof(counts));
        ASSERT_EQ(counts[0],4u) << "two scheduled births plus one payload-driven batch of two";
        std::vector<float> values(view->emitters[0]->values->GetDesc().size/sizeof(float));
        read(view->emitters[0]->values,values.data(),view->emitters[0]->values->GetDesc().size);
        std::vector<uint64_t> ids(view->emitters[0]->capacity);
        read(view->emitters[0]->ids,ids.data(),ids.size()*sizeof(uint64_t));
        uint32_t sizeOffset = 0;
        for (const auto& attr : program.asset.emitters[0].attributes)
        { if (attr.name == "Particles.Size") break; sizeOffset += ComponentCount(attr.type); }
        const auto& singleGraph = receiver2.eventHandlers[1].graph;
        const auto randomNode = std::find_if(singleGraph.nodes.begin(),singleGraph.nodes.end(),[](const auto& node) { return node.operation == Operation::Random; });
        ASSERT_NE(randomNode,singleGraph.nodes.end());
        const auto stream = RandomHash(static_cast<uint32_t>(randomNode->id)) ^ RandomHash(static_cast<uint32_t>(randomNode->id>>32));
        uint32_t ageOffset = 0, payloadOffset = 0;
        for (const auto& attr : program.asset.emitters[0].attributes)
        { if (attr.name == "Particles.Age") break; ageOffset += ComponentCount(attr.type); }
        for (const auto& attr : program.asset.emitters[0].attributes)
        { if (attr.name == "Particles.EventPosition") break; payloadOffset += ComponentCount(attr.type); }
        for (uint32_t i = 0; i < counts[0]; ++i)
        {
            if (ids[i] > 2)
            {
                const auto stride = view->emitters[0]->capacity;
                const auto age = values[ageOffset*stride+i];
                EXPECT_GE(age,1.0f/60.0f); EXPECT_LE(age,1.0f/60.0f+0.025f);
                for (uint32_t c = 0; c < 3; ++c)
                    EXPECT_NEAR(values[c*stride+i]-values[(payloadOffset+c)*stride+i],2*age,1e-5f);
            }
            const auto expected = ids[i]<=2 ? 11.05f+(RandomFloat(ids[i],receiver2.seed,stream,ids[i],0)*2-1) : 0.05f;
            EXPECT_NEAR(values[sizeOffset*view->emitters[0]->capacity+i],expected,1e-5f)
                << "EveryParticle must respect its budget and SingleParticle must use the full ID and event random tick, id=" << ids[i];
        }
    };
    runModes(false); runModes(true);
    const auto runBudget = [&](bool death)
    {
        auto bounded = CreateDefaultSystem(death?"GPU death events":"GPU event overflow");
        auto source = bounded.emitters.front(); source.target = SimulationTarget::GPU;
        source.capacity = 320; source.initialCapacity = 8; source.spawnRate = 0; source.duration = 1.0e6;
        source.bursts = {{0,death?3u:300u}}; source.update.nodes.clear(); source.update.nextNodeId = 1;
        source.eventGenerationMask = EventBit(death?ParticleEventSelection::Death:ParticleEventSelection::Spawn);
        if (death) for (auto& attribute : source.attributes)
            if (attribute.name == "Particles.Lifetime") attribute.defaultValue[0] = 0.001f;
        auto target = source; target.id = NLS::Guid::New().ToString(); target.capacity = 600; target.bursts.clear();
        target.eventGenerationMask = 0; target.eventHandlers.clear();
        for (auto& attribute : target.attributes) if (attribute.name == "Particles.Lifetime") attribute.defaultValue[0] = 2;
        EmitterAsset::EventHandler h; h.id = NLS::Guid::New().ToString(); h.name = "Bounded receiver";
        h.receiver.sourceEmitter = source.id; h.receiver.eventKind = death?ParticleEventSelection::Death:ParticleEventSelection::Spawn;
        h.receiver.spawnCount = 2; target.eventHandlers.push_back(h);
        bounded.emitters = {target,source}; CompiledSystem program;
        ASSERT_TRUE(CompileSystem(bounded,program,diagnostics)) << (diagnostics.empty()?"":diagnostics[0].message);
        ASSERT_TRUE(CompilePlatformPrograms(program,std::span(&backend,1),root/(death?"Death":"Overflow"),diagnostics))
            << (diagnostics.empty()?"":diagnostics[0].message);
        auto frozen = std::make_shared<CompiledSystem>(program);
        SystemInstance instance; ASSERT_TRUE(instance.Initialize(frozen,error)) << error;
        SystemRenderProxy renderProxy(frozen); ASSERT_NE(renderProxy.Prepare(device,instance.Capture(),error),nullptr) << error;
        ASSERT_TRUE(instance.Tick(1.0/30.0));
        auto view = renderProxy.Prepare(device,instance.Capture(),error); ASSERT_NE(view,nullptr) << error;
        std::array<uint32_t,8> counts{}; read(view->emitters[0]->counters,counts.data(),sizeof(counts));
        EXPECT_EQ(counts[0],death?6u:512u) << "bounded append buffers must drop surplus without out-of-bounds writes";
        instance.Reset();
        auto reset = renderProxy.Prepare(device,instance.Capture(),error); ASSERT_NE(reset,nullptr) << error;
        read(reset->emitters[0]->counters,counts.data(),sizeof(counts)); EXPECT_EQ(counts[0],0u);
    };
    runBudget(false); runBudget(true);


    // A corrupt record is a miss; it must rebuild valid native shaders.
    for (const auto& file : std::filesystem::directory_iterator(root/"Cook"/"ShaderMaps"))
        if (file.path().extension() == ".vfxprogram") { std::ofstream broken(file.path(),std::ios::trunc); broken << "incomplete"; }
    ASSERT_TRUE(CompilePlatformPrograms(reloaded,std::span(&backend,1),root/"Cook",diagnostics,nullptr,&cacheStats));
    EXPECT_EQ(cacheStats.misses,1u); EXPECT_GT(cacheStats.shaderCompilations,0u);
    auto changed = compiled; changed.emitters[0].spawnHlsl += "\n// A changed generated source invalidates the package key.\n";
    ASSERT_TRUE(CompilePlatformPrograms(changed,std::span(&backend,1),root/"Cook",diagnostics,nullptr,&cacheStats));
    EXPECT_EQ(cacheStats.misses,1u); EXPECT_GT(cacheStats.shaderCompilations,0u);

}
}
TEST(VfxGpuTests, Dx12GpuEmitterReceivesSpawnEventsFromAnotherGpuEmitter) { ValidateGpuEventExchange(false); }
TEST(VfxGpuTests, VulkanGpuEmitterReceivesSpawnEventsFromAnotherGpuEmitter) { ValidateGpuEventExchange(true); }

namespace
{
/**
 * Compiles the CPU-side program for the same graph the GPU runs and compares
 * every attribute, so a new module cannot diverge between the two targets.
 */
void ValidateAuthoredModuleParity()
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 device validation runs on Windows";
#else
    using namespace NLS;
    using namespace NLS::VFX;
    namespace author = VFX::Authoring;
    namespace rhi = Render::RHI;
    namespace shader = Render::ShaderCompiler;
    namespace context = Render::Context;
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) { GTEST_SKIP() << "Another fixture owns the RHI executor"; }
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};

    Render::Settings::DriverSettings settings;
    settings.debugMode = true;
    settings.graphicsBackend = Render::Settings::EGraphicsBackend::DX12;
    auto device = Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) { GTEST_SKIP() << "DX12 compute device is unavailable"; }
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));

    // A system whose spawn and update stacks both use the newly added modules.
    auto system = author::CreateSystem();
    auto& emitter = system.emitters.front();
    auto& spawnStack = *author::FindStack(emitter,author::Stage::ParticleSpawn);
    spawnStack.modules.resize(1);
    spawnStack.modules[0].inputs["Position"].value = {0,0,0,0};
    spawnStack.modules[0].inputs["Velocity"].value = {0,0,0,0};
    auto sphere = author::CreateModule("SphereLocation");
    sphere.inputs["Radius"].value = {2,0,0,0}; sphere.inputs["Surface Only"].value = {1,0,0,0};
    spawnStack.modules.push_back(sphere);
    auto cone = author::CreateModule("AddVelocityInCone");
    cone.inputs["Speed"].value = {4,0,0,0}; cone.inputs["Angle"].value = {15,0,0,0};
    spawnStack.modules.push_back(cone);
    // Second batch: a torus spawn shell plus an angular velocity injection.
    auto torus = author::CreateModule("TorusLocation");
    torus.inputs["Center"].value = {0,0,0,0};
    torus.inputs["Major Radius"].value = {1,0,0,0};
    torus.inputs["Minor Radius"].value = {0.25f,0,0,0};
    torus.inputs["Axis"].value = {0,1,0,0};
    spawnStack.modules.push_back(torus);
    auto& updateStack = *author::FindStack(emitter,author::Stage::ParticleUpdate);
    updateStack.modules.clear();
    auto gravity = author::CreateModule("AccelerationForce");
    gravity.inputs["Acceleration"].value = {0,-9.81f,0,0};
    updateStack.modules.push_back(gravity);
    auto limit = author::CreateModule("LimitForce");
    limit.inputs["Force Limit"].value = {20,0,0,0};
    updateStack.modules.push_back(limit);
    auto vortex = author::CreateModule("VortexVelocity");
    vortex.inputs["Vortex Origin"].value = {0,0,0,0};
    vortex.inputs["Vortex Axis"].value = {0,1,0,0};
    vortex.inputs["Vortex Strength"].value = {2,0,0,0};
    updateStack.modules.push_back(vortex);
    auto rotation = author::CreateModule("SpriteRotationRate");
    rotation.inputs["Rotation Rate"].value = {180,0,0,0};
    updateStack.modules.push_back(rotation);
    auto frame = author::CreateModule("SubUVAnimation");
    frame.inputs["First Frame"].value = {0,0,0,0}; frame.inputs["Frame Count"].value = {4,0,0,0};
    frame.inputs["Random Start"].value = {0,0,0,0};
    updateStack.modules.push_back(frame);
    // Third batch: quaternion composition, per-frame counter and travelled
    // distance. These are the numerically interesting additions, so they must
    // agree between the CPU interpreter and the device pipeline.
    auto meshSpin = author::CreateModule("MeshRotationRate");
    meshSpin.inputs["Axis"].value = {0,0,1,0};
    meshSpin.inputs["Rate"].value = {90,0,0,0};
    updateStack.modules.push_back(meshSpin);
    updateStack.modules.push_back(author::CreateModule("FrameCounter"));
    auto travelled = author::CreateModule("TrackDistanceTraveled");
    travelled.inputs["Scale"].value = {1,0,0,0};
    updateStack.modules.push_back(travelled);
    updateStack.modules.push_back(author::CreateModule("SolveForcesAndVelocity"));

    CompiledSystem compiled; std::vector<author::Issue> issues; std::string error;
    if (!author::Compile(system,compiled,issues))
    { ADD_FAILURE() << (issues.empty() ? "" : issues.front().message); return; }
    auto asset = compiled.asset.emitters[0];
    // The new modules must have introduced these columns on the emitter.
    const auto hasAttribute = [&](std::string_view name)
    { return std::any_of(asset.attributes.begin(),asset.attributes.end(),
        [&](const Attribute& a) { return a.name == name; }); };
    EXPECT_TRUE(hasAttribute("Particles.Rotation"));
    EXPECT_TRUE(hasAttribute("Particles.SubImageIndex"));

    ParticleDataSet cpu;
    cpu.Initialize(asset.attributes,137);
    for (uint32_t index = 0; index < 129; ++index)
        if (!cpu.Spawn(static_cast<uint64_t>(index)+0x100000001ull))
        { ADD_FAILURE() << "cpu spawn failed"; return; }
    GpuDataSet gpu;
    if (!gpu.Initialize(device,cpu,error)) { ADD_FAILURE() << error; return; }

    const auto root = std::filesystem::temp_directory_path()/("NullusVfxModuleParity_"+Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};

    shader::ShaderCompiler compiler;
    auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"VfxModuleParity");
    if (!pool) { ADD_FAILURE() << "command pool unavailable"; return; }
    auto commands = pool->CreateCommandBuffer("VfxModuleParity");
    if (!commands) { ADD_FAILURE() << "command buffer unavailable"; return; }
    commands->Begin();
    std::vector<std::shared_ptr<GpuDispatchResources>> keepAlive;
    std::vector<Value> registers;
    for (const auto* graph : {&asset.spawn,&asset.update})
    {
        const auto built = CompileGraph(*graph,asset.attributes);
        if (!built.Succeeded()) { ADD_FAILURE() << "graph compile failed"; return; }
        const auto path = root/(graph->id+".hlsl");
        { std::ofstream file(path); file << built.compiled.hlsl; }
        shader::ShaderCompilationInput input;
        input.assetPath = path.string(); input.stage = shader::ShaderStage::Compute;
        input.options.targetPlatform = shader::ShaderTargetPlatform::DXIL;
        input.options.targetProfile = "cs_6_0"; input.options.entryPoint = "Main";
        input.options.artifactDirectory = (root/"Artifacts").string();
        const auto output = compiler.Compile(input);
        if (output.status != shader::ShaderCompilationStatus::Succeeded)
        { ADD_FAILURE() << output.diagnostics; return; }
        rhi::RHIShaderModuleDesc module;
        module.stage = rhi::ShaderStage::Compute; module.targetBackend = rhi::NativeBackendType::DX12;
        module.entryPoint = "Main"; module.bytecode = output.bytecode;
        auto pipeline = gpu.CreatePipeline(module,error);
        if (!pipeline) { ADD_FAILURE() << error; return; }
        const float delta = graph->stage == ScriptStage::ParticleSpawn ? 0 : 1.0f/60;
        auto dispatch = gpu.Record(*commands,pipeline,cpu.Count(),delta,asset.seed,1,{},error);
        if (!dispatch) { ADD_FAILURE() << error; return; }
        keepAlive.push_back(std::move(dispatch));
        if (!RunParticleProgram(built.compiled.program, cpu, 0, cpu.Count(), {delta, asset.seed, 1, nullptr}, registers))
        { ADD_FAILURE() << "cpu program failed"; return; }
    }
    commands->End();
    auto fence = device->CreateFence("VfxModuleParityComplete");
    if (!fence) { ADD_FAILURE() << "fence unavailable"; return; }
    fence->Reset();
    rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
    const auto submitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
    if (!submitted.Succeeded()) { ADD_FAILURE() << submitted.message; return; }
    if (!fence->Wait(30000000000ull)) { ADD_FAILURE() << "fence timeout"; return; }

    std::vector<float> values(gpu.ScalarCount()*gpu.Capacity());
    rhi::RHIBufferReadbackDesc read;
    read.source = gpu.AttributeBuffer(); read.sourceState = rhi::ResourceState::ShaderWrite;
    read.size = values.size()*sizeof(float); read.data = values.data(); read.debugName = "VfxModuleParityReadback";
    const auto readback = device->BeginReadBuffer(read);
    if (!readback.Succeeded()) { ADD_FAILURE() << readback.message; return; }
    if (readback.completion && !readback.completion->Wait(30000000000ull).Succeeded())
    { ADD_FAILURE() << "readback timeout"; return; }
    size_t offset = 0;
    for (uint32_t attribute = 0; attribute < cpu.Attributes().size(); ++attribute)
    {
        for (uint32_t component = 0; component < ComponentCount(cpu.Attributes()[attribute].type); ++component)
        {
            for (uint32_t particle = 0; particle < cpu.Count(); ++particle)
                EXPECT_NEAR(values[offset+particle],cpu.Read(attribute,particle)[component],1e-5f)
                    << cpu.Attributes()[attribute].name << ":" << particle << ":" << component;
            offset += gpu.Capacity();
        }
    }
#endif
}
}
TEST(VfxGpuTests, Dx12AuthoredShapeForceAndSubUvModulesMatchCpu)
{
    ValidateAuthoredModuleParity();
}


namespace
{
void ValidateStructuredRuntime(bool vulkan)
{
    using namespace NLS::VFX;namespace author=NLS::VFX::Authoring;
    namespace rhi=NLS::Render::RHI;namespace context=NLS::Render::Context;
    auto& executor=context::RhiCommandListExecutor::Get();
    if(executor.IsInitialized())GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown {context::RhiCommandListExecutor& executor;~Shutdown(){executor.Shutdown();}} shutdown{executor};
    struct Window {GLFWwindow* handle=nullptr;~Window(){if(handle){glfwDestroyWindow(handle);glfwTerminate();}}} window;
    if(vulkan)
    {
        if(!glfwInit())GTEST_SKIP() << "Vulkan requires GLFW WSI";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle=glfwCreateWindow(64,64,"VFX structured runtime",nullptr,nullptr);
        if(!window.handle){glfwTerminate();GTEST_SKIP() << "Vulkan WSI unavailable";}
    }
    NLS::Render::Settings::DriverSettings settings;settings.debugMode=false;settings.platformWindow=window.handle;
    settings.graphicsBackend=vulkan?NLS::Render::Settings::EGraphicsBackend::VULKAN:NLS::Render::Settings::EGraphicsBackend::DX12;
    auto device=NLS::Render::Backend::CreateRhiDevice(settings);
    if(!device || !device->IsBackendReady())GTEST_SKIP() << "Requested compute backend unavailable";
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    auto module=Tests::StructuredModule();author::SourceLibrary library;std::string error;
    ASSERT_TRUE(library.Add(module,{},error)) << error;
    auto source=Tests::StructuredSystem(module,SimulationTarget::CPU);
    source.emitters.push_back(Tests::StructuredSystem(module,SimulationTarget::GPU).emitters.front());
    CompiledSystem authored;std::vector<author::Issue> issues;std::vector<author::SourceDependency> dependencies;
    ASSERT_TRUE(author::Compile(source,library,authored,dependencies,issues)) << (issues.empty()?"":issues[0].message);
    auto runtime=authored.asset;
    auto stage=CreateSimulationStage("Structured DI stage");stage.graph=runtime.emitters[1].update;
    stage.graph.id=NLS::Guid::New().ToString();stage.graph.stage=ScriptStage::SimulationStage;
    runtime.emitters[1].simulationStages.push_back(stage);
    EmitterAsset::EventHandler handler;handler.id=NLS::Guid::New().ToString();handler.name="Structured event";
    handler.receiver.sourceEmitter=runtime.emitters[0].id;handler.receiver.eventKind=ParticleEventSelection::Spawn;
    handler.receiver.executionMode=EventExecutionMode::EveryParticle;handler.receiver.maxEventsPerFrame=1;
    handler.graph=runtime.emitters[1].update;handler.graph.id=NLS::Guid::New().ToString();handler.graph.stage=ScriptStage::Event;
    runtime.emitters[1].eventHandlers.push_back(handler);
    CompiledSystem compiled;std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(runtime,compiled,diagnostics)) << (diagnostics.empty()?"":diagnostics[0].message);
    const auto root=std::filesystem::temp_directory_path()/("NullusVfxStructured_"+NLS::Guid::New().ToString());
    struct Cleanup {std::filesystem::path root;~Cleanup(){std::error_code ec;std::filesystem::remove_all(root,ec);}} cleanup{root};
    const auto backend=device->GetAdapter()->GetBackendType();
    const std::array targets{backend,rhi::NativeBackendType::Metal};
    PlatformCompileCacheStats stats;
    ASSERT_TRUE(CompilePlatformPrograms(compiled,targets,root,diagnostics,nullptr,&stats)) << (diagnostics.empty()?"":diagnostics[0].message);
    EXPECT_EQ(stats.misses,2u);
    auto cached=compiled;
    ASSERT_TRUE(CompilePlatformPrograms(cached,targets,root,diagnostics,nullptr,&stats));
    EXPECT_EQ(stats.hits,2u);EXPECT_EQ(stats.shaderCompilations,0u);
    CompiledSystem cooked;
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled,false),cooked,error)) << error;
    EXPECT_TRUE(cooked.emitters[1].spawnHlsl.empty());
    auto frozen=std::make_shared<CompiledSystem>(cooked);SystemInstance world;
    ASSERT_TRUE(world.Initialize(frozen,error)) << error;
    auto proxy=std::make_unique<SystemRenderProxy>(frozen);
    ASSERT_NE(proxy->Prepare(device,world.Capture(),error),nullptr) << error;
    const auto read=[&](const std::shared_ptr<rhi::RHIBuffer>& buffer,void* data,uint64_t size)
    {
        rhi::RHIBufferReadbackDesc desc;desc.source=buffer;desc.sourceState=buffer->GetState();desc.size=size;desc.data=data;
        auto result=device->BeginReadBuffer(desc);ASSERT_TRUE(result.Succeeded()) << result.message;
        if(result.completion)ASSERT_TRUE(result.completion->Wait(30000000000ull).Succeeded());
    };
    const auto compare=[&](const std::shared_ptr<const SystemFrame>& frame,const std::shared_ptr<const SystemRenderFrame>& render)
    {
        const auto& cpu=*frame->cpu[0];const auto& gpu=*render->emitters[1];
        std::array<uint32_t,8> counts{};read(gpu.counters,counts.data(),sizeof(counts));ASSERT_EQ(counts[0],cpu.Count());ASSERT_EQ(counts[0],4u);
        std::vector<uint64_t> ids(gpu.capacity);read(gpu.ids,ids.data(),ids.size()*sizeof(uint64_t));
        std::vector<uint32_t> values(gpu.values->GetDesc().size/sizeof(uint32_t));read(gpu.values,values.data(),gpu.values->GetDesc().size);
        for(uint32_t particle=0;particle<counts[0];++particle)
        {
            uint32_t cpuParticle=0;while(cpuParticle<cpu.Count() && cpu.Id(cpuParticle)!=ids[particle])++cpuParticle;
            ASSERT_LT(cpuParticle,cpu.Count());uint32_t scalar=0;
            for(const auto& attr:cpu.Attributes())
            {
                const auto expected=cpu.Read(cpu.FindAttribute(attr.name),cpuParticle);
                for(uint32_t c=0;c<ComponentCount(attr.type);++c,++scalar)
                {
                    const auto actual=values[scalar*gpu.capacity+particle];
                    if(IsIntegralType(attr.type))EXPECT_EQ(actual,std::bit_cast<uint32_t>(expected[c])) << attr.name;
                    else EXPECT_NEAR(std::bit_cast<float>(actual),expected[c],1e-5f) << attr.name;
                }
            }
        }
    };
    ASSERT_TRUE(world.Tick(1.0/60.0));auto initialFrame=world.Capture();auto initial=proxy->Prepare(device,initialFrame,error);
    ASSERT_NE(initial,nullptr) << error;compare(initialFrame,initial);
    EXPECT_FLOAT_EQ(world.CpuEmitters()[0]->Data().Read(world.CpuEmitters()[0]->Data().FindAttribute("Particles.EnabledAsFloat"),0)[0],1.0f);
    const std::array<Value,3> points{{{10,20,30,0},{40,50,60,0},{70,80,90,0}}};
    ASSERT_TRUE(world.SetDataInterface("User.Points",ValueType::Float3,points));
    ASSERT_NE(proxy->Prepare(device,world.Capture(),error),nullptr) << error;
    ASSERT_TRUE(world.Tick(1.0/60.0));auto changed=proxy->Prepare(device,world.Capture(),error);ASSERT_NE(changed,nullptr) << error;
    compare(world.Capture(),changed);
    EXPECT_FLOAT_EQ(world.CpuEmitters()[0]->Data().Read(world.CpuEmitters()[0]->Data().FindAttribute("Particles.Position"),0)[0],120);
    const std::array<Value,2> curve{{{0,4,0,0},{1,12,0,0}}};
    ASSERT_TRUE(world.SetDataInterface("User.WeightCurve",ValueType::Float4,curve));
    ASSERT_NE(proxy->Prepare(device,world.Capture(),error),nullptr) << error;
    ASSERT_TRUE(world.SetParameters({{"User.Lookup.state.index",IntValue(2)},{"User.Lookup.state.weight",{0.5f,0,0,0}},
        {"User.Lookup.position",{1,2,3,0}}}));
    ASSERT_NE(proxy->Prepare(device,world.Capture(),error),nullptr) << error;
    ASSERT_TRUE(world.Tick(1.0/60.0));changed=proxy->Prepare(device,world.Capture(),error);ASSERT_NE(changed,nullptr) << error;
    compare(world.Capture(),changed);
    EXPECT_FLOAT_EQ(world.CpuEmitters()[0]->Data().Read(world.CpuEmitters()[0]->Data().FindAttribute("Particles.Position"),0)[0],561);
    for (const auto index : {int32_t{-1},INT32_MAX,int32_t{0}})
    {
        ASSERT_TRUE(world.SetTypedParameter("User.Lookup.state.index",ValueType::Int32,IntValue(index)));
        ASSERT_TRUE(world.SetTypedParameter("User.Lookup.state.weight",ValueType::Float,{index==0?1.25f:-1.0f,0,0,0}));
        ASSERT_NE(proxy->Prepare(device,world.Capture(),error),nullptr) << error;
        ASSERT_TRUE(world.Tick(1.0/60.0));changed=proxy->Prepare(device,world.Capture(),error);ASSERT_NE(changed,nullptr) << error;
        compare(world.Capture(),changed);
        const auto& cpu=world.CpuEmitters()[0]->Data();
        EXPECT_EQ(ReadInt(cpu.Read(cpu.FindAttribute("Particles.LookupResult.code"),0)),index==INT32_MAX?-7:INT32_MIN);
        EXPECT_FLOAT_EQ(cpu.Read(cpu.FindAttribute("Particles.EnabledAsFloat"),0)[0],0.0f);
    }
    proxy.reset();compare(initialFrame,initial);
}
}
TEST(VfxGpuTests, Dx12StructuredModulesAndRuntimeDataInterfacesMatchCpu) {ValidateStructuredRuntime(false);}
TEST(VfxGpuTests, VulkanStructuredModulesAndRuntimeDataInterfacesMatchCpu) {ValidateStructuredRuntime(true);}
