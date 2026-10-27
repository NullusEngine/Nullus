#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <GLFW/glfw3.h>
#include <Guid.h>
#include <Json/json.hpp>
#include <VFX/MeshRenderer.h>
#include <VFX/CpuEmitter.h>
#include <VFX/GpuEmitter.h>
#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxShaderCompiler.h>
#include <Rendering/Geometry/Vertex.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>

namespace
{
using namespace NLS::VFX;
namespace rhi = NLS::Render::RHI;
namespace context = NLS::Render::Context;
SystemAsset MeshSystem()
{
    auto asset = CreateDefaultSystem("Mesh particles"); auto& emitter = asset.emitters[0];
    emitter.renderer = RendererType::Mesh; emitter.mesh.meshes = {{"4e16656a-cc1e-44e0-befc-b00108056221",1}};
    emitter.capacity = 257; emitter.initialCapacity = 3; emitter.spawn.nodes.clear(); emitter.update.nodes.clear();
    emitter.spawnRate = 0; emitter.bursts = {{0,1}}; emitter.sprite.circularMask = false;
    emitter.attributes[0].defaultValue = {0,0,0.5f,0}; emitter.attributes[1].defaultValue = {};
    emitter.attributes[2].defaultValue = {1,1,1,1};
    emitter.attributes.push_back({"Particles.Scale",ValueType::Float3,{1,1,1,0}});
    emitter.attributes.push_back({"Particles.MeshOrientation",ValueType::Float4,{0,0,0,1}});
    emitter.attributes.push_back({"Particles.DynamicMaterialParameter",ValueType::Float4,{0.2f,0.8f,0.4f,1}});
    return asset;
}

struct TestMesh final : rhi::RHIMesh
{
    std::shared_ptr<rhi::RHIBuffer> vertices, indices;
    uint32_t indexCount = 6;
    uint32_t GetVertexCount() const override { return 4; }
    uint32_t GetIndexCount() const override { return indexCount; }
    uint32_t GetVertexStride() const override { return sizeof(NLS::Render::Geometry::Vertex); }
    NLS::Render::Settings::EPrimitiveMode GetPrimitiveMode() const override { return NLS::Render::Settings::EPrimitiveMode::TRIANGLES; }
    rhi::IndexType GetIndexType() const override { return rhi::IndexType::UInt32; }
    std::shared_ptr<rhi::RHIBuffer> GetVertexBuffer() const override { return vertices; }
    std::shared_ptr<rhi::RHIBuffer> GetIndexBuffer() const override { return indices; }
};

void ValidateMeshDevice(bool vulkan)
{
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    struct Window { GLFWwindow* handle = nullptr; ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } } } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan WSI unavailable";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(64,64,"Mesh particles",nullptr,nullptr);
        if (!window.handle) GTEST_SKIP() << "Vulkan WSI window unavailable";
    }
    NLS::Render::Settings::DriverSettings settings; settings.debugMode = false; settings.platformWindow = window.handle;
    settings.graphicsBackend = vulkan ? NLS::Render::Settings::EGraphicsBackend::VULKAN : NLS::Render::Settings::EGraphicsBackend::DX12;
    auto device = NLS::Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested device unavailable";
    const auto validationBefore = NLS::Render::Backend::GetVulkanValidationErrorCountForTesting();
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto readBuffer = [&](const rhi::RHIBufferReadbackDesc& request)
    {
        const auto result = device->BeginReadBuffer(request);
        EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
    };
    auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"MeshParticles"); ASSERT_TRUE(pool);
    const auto root = std::filesystem::temp_directory_path()/("NullusMeshParticles_"+NLS::Guid::New().ToString());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path,e); } } cleanup{root};
    auto asset = MeshSystem(); asset.emitters[0].target = SimulationTarget::GPU;
    CompiledSystem program; std::vector<Diagnostic> diagnostics; std::string error;
    ASSERT_TRUE(CompileSystem(asset,program,diagnostics));
    const auto backend = device->GetAdapter()->GetBackendType();
    ASSERT_TRUE(CompilePlatformPrograms(program,std::span(&backend,1),root,diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    // Runtime consumes the compiled mesh entry and bindings without source graphs/HLSL.
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(program,false),program,error)) << error;
    ASSERT_TRUE(ValidatePlatformPrograms(program,backend,error)) << error;
    const auto& modules = program.platforms[0].emitters[0]; const auto& emitterAsset = program.asset.emitters[0];
    auto mesh = std::make_shared<TestMesh>();
    std::array<NLS::Render::Geometry::Vertex,4> vertices{};
    const float positions[4][2]{{-0.6f,-0.15f},{0.6f,-0.15f},{0.6f,0.15f},{-0.6f,0.15f}};
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        vertices[i].position[0] = positions[i][0]; vertices[i].position[1] = positions[i][1];
        vertices[i].texCoords[0] = (i == 1 || i == 2) ? 1.0f : 0.0f; vertices[i].texCoords[1] = i >= 2 ? 1.0f : 0.0f;
    }
    const std::array<uint32_t,6> indices{0,2,1,0,3,2};
    const auto buffer = [&](const void* data, uint64_t bytes, rhi::BufferUsageFlags usage)
    {
        rhi::RHIBufferDesc desc; desc.size = bytes; desc.usage = usage; desc.memoryUsage = rhi::MemoryUsage::CPUToGPU;
        rhi::RHIBufferUploadDesc upload; upload.data = data; upload.dataSize = bytes; return device->CreateBuffer(desc,upload);
    };
    mesh->vertices = buffer(vertices.data(),sizeof(vertices),rhi::BufferUsageFlags::Vertex);
    mesh->indices = buffer(indices.data(),sizeof(indices),rhi::BufferUsageFlags::Index);
    ASSERT_TRUE(mesh->vertices && mesh->indices);
    auto geometry = std::make_shared<MeshRenderGeometry>();
    geometry->levels.push_back({mesh,1.0f});
    geometry->bounds = {0,0,0,std::hypot(0.6f,0.15f)};
    rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {rhi::TextureFormat::RGBA8};
    auto renderer = std::make_shared<MeshRenderer>();
    ASSERT_TRUE(renderer->Initialize(device,emitterAsset.attributes,modules.spriteVertex,modules.spriteFragment,targets,error,
        emitterAsset.mesh,modules.sort,modules.visibility,modules.simulation.bounds,emitterAsset.sprite,true)) << error;
    struct Output
    {
        std::vector<uint8_t> pixels;
        std::shared_ptr<const SpriteDrawResources> draw;
        std::shared_ptr<const ParticleRenderData> cpu;
    };
    const auto render = [&](const ParticleDataSet* cpu, const GpuDataSet* gpu, SpriteView view, std::shared_ptr<const MeshRenderGeometry> shape,
        const ParticleRenderer* target = nullptr, std::vector<std::shared_ptr<const MeshRenderGeometry>> shapes = {}, std::vector<SpriteMaterial> materials = {})
    {
        Output output;
        rhi::RHITextureDesc desc; desc.extent = {64,64,1}; desc.format = rhi::TextureFormat::RGBA8;
        desc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
        auto color = device->CreateTexture(desc); rhi::RHITextureViewDesc viewDesc; viewDesc.format = desc.format;
        auto colorView = color ? device->CreateTextureView(color,viewDesc) : nullptr; auto commands = pool->CreateCommandBuffer("MeshDraw");
        EXPECT_TRUE(colorView && commands); if (!colorView || !commands) return output;
        SpriteDrawContext context;
        if (shapes.empty()) context.meshes = {std::move(shape)}; else context.meshes = std::move(shapes);
        context.meshMaterials = std::move(materials);
        rhi::RHIRenderPassDesc pass; pass.renderArea = {0,0,64,64}; rhi::RHIRenderPassColorAttachmentDesc attachment;
        attachment.view = colorView; attachment.clearValue = {0,0,0,1}; pass.colorAttachments = {attachment};
        commands->Begin();
        if (cpu)
        {
            output.cpu = UploadCpuRenderData(device,*cpu,error); EXPECT_TRUE(output.cpu) << error; if (!output.cpu) return output;
            output.draw = (target ? *target : *renderer).Record(*commands,*output.cpu,view,pass,error,context);
        }
        else output.draw = (target ? *target : *renderer).Record(*commands,*gpu,view,pass,error,context);
        EXPECT_TRUE(output.draw) << error; if (!output.draw) return output;
        commands->End(); auto fence = device->CreateFence("MeshDrawComplete"); EXPECT_TRUE(fence); if (!fence) return output;
        fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        const auto submitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit); EXPECT_TRUE(submitted.Succeeded()) << submitted.message;
        if (!submitted.Succeeded()) return output;
        EXPECT_TRUE(fence->Wait(30000000000ull));
        output.pixels.resize(64*64*4);
        const auto result = device->BeginReadPixels(color,0,0,64,64,NLS::Render::Settings::EPixelDataFormat::RGBA,
            NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,output.pixels.data());
        EXPECT_TRUE(result.Succeeded()) << result.message;
        if (result.completion) EXPECT_TRUE(result.completion->Wait(30000000000ull).Succeeded());
        return output;
    };
    auto cpuSettings = emitterAsset; cpuSettings.target = SimulationTarget::CPU;
    CpuEmitter cpu; ASSERT_TRUE(cpu.Initialize(cpuSettings,program.emitters[0].spawn,program.emitters[0].update,error)) << error;
    ASSERT_TRUE(cpu.Tick(1.0/60)); ASSERT_EQ(cpu.Data().Count(),1u);
    SpriteView view;
    const auto horizontal = render(&cpu.Data(),nullptr,view,geometry); ASSERT_EQ(horizontal.pixels.size(),64*64*4);
    EXPECT_NEAR(horizontal.pixels[(32*64+32)*4],51,2); EXPECT_NEAR(horizontal.pixels[(32*64+32)*4+1],204,2);
    EXPECT_GT(horizontal.pixels[(32*64+48)*4+1],190); EXPECT_EQ(horizontal.pixels[(48*64+32)*4+1],0);
    GpuEmitter gpu; ASSERT_TRUE(gpu.Initialize(device,emitterAsset,program.emitters[0],modules.simulation,error)) << error;
    auto commands = pool->CreateCommandBuffer("MeshSimulation"); commands->Begin();
    auto frame = gpu.RecordFrame(*commands,1,1.0/60,error); ASSERT_TRUE(frame) << error; commands->End();
    auto fence = device->CreateFence("MeshSimComplete"); fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
    ASSERT_TRUE(device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit).Succeeded()); ASSERT_TRUE(gpu.CommitFrame(frame)); ASSERT_TRUE(fence->Wait(30000000000ull));
    const auto simulated = render(nullptr,&frame->Data(),view,geometry); EXPECT_EQ(simulated.pixels,horizontal.pixels);
    auto rotated = cpu.Data(); rotated.Write(rotated.FindAttribute("Particles.MeshOrientation"),0,{0,0,8,8});
    const auto vertical = render(&rotated,nullptr,view,geometry); ASSERT_EQ(vertical.pixels.size(),horizontal.pixels.size());
    EXPECT_EQ(vertical.pixels[(32*64+48)*4+1],0); EXPECT_GT(vertical.pixels[(48*64+32)*4+1],190);
    rotated.Write(rotated.FindAttribute("Particles.Scale"),0,{-0.25f,2,1,0});
    const auto scaled = render(&rotated,nullptr,view,geometry); EXPECT_NE(scaled.pixels,vertical.pixels);
    // A second view derives zero indexed instances without changing the frozen frame.
    auto outside = view; outside.localToWorld[3] = 100;
    const auto culled = render(nullptr,&frame->Data(),outside,geometry); ASSERT_TRUE(culled.draw);
    ParticleViewStatistics statistics;
    rhi::RHIBufferReadbackDesc read; read.source = culled.draw->arguments; read.size = sizeof(statistics); read.data = &statistics;
    ASSERT_TRUE(readBuffer(read)); EXPECT_EQ(statistics.particleCount,1u); EXPECT_EQ(statistics.instanceCount,0u); EXPECT_EQ(statistics.vertexCount,6u);
    read.source = horizontal.draw->arguments; ASSERT_TRUE(readBuffer(read)); EXPECT_EQ(statistics.instanceCount,1u);
    const auto readBounds = [&](const Output& output, const ParticleDataSet& source, const Value& bounds, const SpriteView& camera)
    {
        ParticleBounds actual; rhi::RHIBufferReadbackDesc request; request.source = output.draw->visibility->bounds; request.size = sizeof(actual); request.data = &actual;
        EXPECT_TRUE(readBuffer(request)); const auto expected = CalculateMeshParticleBounds(source,emitterAsset.mesh,std::vector<Value>{bounds});
        EXPECT_EQ(actual.particleCount,expected.particleCount); EXPECT_EQ(actual.invalidCount,expected.invalidCount);
        for (size_t c = 0; c < 3; ++c) { EXPECT_NEAR(actual.minimum[c],expected.minimum[c],1e-4f); EXPECT_NEAR(actual.maximum[c],expected.maximum[c],1e-4f); }
        ParticleViewStatistics counts; request.source = output.draw->arguments; request.size = sizeof(counts); request.data = &counts;
        EXPECT_TRUE(readBuffer(request)); const auto reference = CalculateMeshParticleVisibility(source,emitterAsset.mesh,std::vector<Value>{bounds},camera,std::vector<uint32_t>{6u});
        EXPECT_EQ(counts.visibleCount,reference.visibleCount); EXPECT_EQ(counts.culledCount,reference.culledCount); EXPECT_EQ(counts.invalidCount,reference.invalidCount);
        uint32_t firstInstance = 123; request.sourceOffset = 8*sizeof(uint32_t); request.size = 4; request.data = &firstInstance;
        EXPECT_TRUE(readBuffer(request)); EXPECT_EQ(firstInstance,0u);
    };
    readBounds(horizontal,cpu.Data(),geometry->bounds,view); readBounds(vertical, [&]() { auto d=cpu.Data(); d.Write(d.FindAttribute("Particles.MeshOrientation"),0,{0,0,8,8}); return d; }(),geometry->bounds,view);
    // Exercise both scan group boundaries, NaN, off-origin centers, negative
    // scale and an affine transform. Results are checked against CPU geometry.
    ParticleDataSet many; many.Initialize(emitterAsset.attributes,257);
    for (uint32_t i = 0; i < 257; ++i)
    {
        ASSERT_TRUE(many.Spawn(uint64_t(i)+(1ull<<34))); many.Write(0,i,{float(i%19)*0.4f-3, float(i%5)*0.3f-0.8f,0.5f,0});
        many.Write(many.FindAttribute("Particles.Scale"),i,{i%2 ? -0.3f : 0.6f,0.4f,0.2f,0});
        many.Write(many.FindAttribute("Particles.MeshOrientation"),i,i%3 ? Value{0,0,3,3} : Value{});
    }
    many.Write(many.FindAttribute("Particles.MeshOrientation"),3,{std::numeric_limits<float>::quiet_NaN(),0,0,1});
    auto offsetVertices = vertices;
    for (auto& v : offsetVertices) { v.position[0] += 0.7f; v.position[1] -= 0.2f; v.position[2] += 0.1f; }
    auto offsetMesh = std::make_shared<TestMesh>(); offsetMesh->indices = mesh->indices;
    offsetMesh->vertices = buffer(offsetVertices.data(),sizeof(offsetVertices),rhi::BufferUsageFlags::Vertex); ASSERT_TRUE(offsetMesh->vertices);
    auto offCenter = std::make_shared<MeshRenderGeometry>();
    offCenter->levels.push_back({offsetMesh,1.0f});
    offCenter->bounds = {0.7f,-0.2f,0.1f,geometry->bounds[3]};
    auto affine = view; affine.localToWorld[0] = -1.5f; affine.localToWorld[1] = 0.2f; affine.localToWorld[5] = 0.5f;
    for (bool culling : {true,false})
    {
        affine.frustumCulling = culling; const auto result = render(&many,nullptr,affine,offCenter); ASSERT_TRUE(result.draw);
        readBounds(result,many,offCenter->bounds,affine);
        GpuDataSet uploaded; ASSERT_TRUE(uploaded.Initialize(device,many,error)) << error;
        const auto gpuResult = render(nullptr,&uploaded,affine,offCenter); ASSERT_TRUE(gpuResult.draw); readBounds(gpuResult,many,offCenter->bounds,affine);
        EXPECT_EQ(result.pixels,gpuResult.pixels);
    }
    ParticleDataSet empty; empty.Initialize(emitterAsset.attributes,3);
    const auto emptyOutput = render(&empty,nullptr,view,geometry); ASSERT_TRUE(emptyOutput.draw); readBounds(emptyOutput,empty,geometry->bounds,view);
    const auto pixel = [](const Output& output, int x, int y, int channel) { return output.pixels[(y*64+x)*4+channel]; };
    // Per-view LOD selection from the projected bounds; explicit indices clamp
    // to the captured chain for every slot.
    auto narrow = std::make_shared<TestMesh>();
    auto narrowVertices = vertices;
    for (auto& vertex : narrowVertices) vertex.position[0] *= 0.15f;
    narrow->indices = mesh->indices;
    narrow->vertices = buffer(narrowVertices.data(),sizeof(narrowVertices),rhi::BufferUsageFlags::Vertex); ASSERT_TRUE(narrow->vertices);
    auto loded = std::make_shared<MeshRenderGeometry>();
    loded->levels.push_back({mesh,1.0f}); loded->levels.push_back({narrow,0.5f});
    loded->bounds = {0,0,0,0.7f};
    const auto lodRenderer = [&](int32_t lod)
    {
        auto settings = emitterAsset.mesh; settings.lodIndex = lod;
        auto target = std::make_shared<MeshRenderer>();
        EXPECT_TRUE(target->Initialize(device,emitterAsset.attributes,modules.spriteVertex,modules.spriteFragment,targets,error,
            settings,modules.sort,modules.visibility,modules.simulation.bounds,emitterAsset.sprite,false)) << error;
        return target;
    };
    auto wideView = view; wideView.viewProjection[5] = 4.0f;
    auto zoomedOut = view; zoomedOut.viewProjection[5] = 1.2f;
    auto autoLod = lodRenderer(-1);
    const auto autoWide = render(&cpu.Data(),nullptr,wideView,nullptr,autoLod.get(),{loded}); ASSERT_TRUE(autoWide.draw);
    EXPECT_GT(pixel(autoWide,42,32,1),190);
    const auto autoNarrow = render(&cpu.Data(),nullptr,zoomedOut,nullptr,autoLod.get(),{loded}); ASSERT_TRUE(autoNarrow.draw);
    EXPECT_EQ(pixel(autoNarrow,42,32,1),0);
    auto forcedWide = lodRenderer(0);
    const auto forcedWideImage = render(&cpu.Data(),nullptr,zoomedOut,nullptr,forcedWide.get(),{loded}); ASSERT_TRUE(forcedWideImage.draw);
    EXPECT_GT(pixel(forcedWideImage,42,32,1),190);
    auto forcedNarrow = lodRenderer(1);
    const auto forcedNarrowImage = render(&cpu.Data(),nullptr,wideView,nullptr,forcedNarrow.get(),{loded}); ASSERT_TRUE(forcedNarrowImage.draw);
    EXPECT_EQ(pixel(forcedNarrowImage,42,32,1),0);
    // Two mesh slots share one particle frame; the vertex stage filters each draw
    // and every slot binds its own material snapshot and indirect arguments.
    auto slotSettings = program.asset; auto& slotEmitter = slotSettings.emitters[0];
    slotEmitter.attributes.push_back({"Particles.MeshIndex",ValueType::Float,{}});
    for (auto& attribute : slotEmitter.attributes)
        if (attribute.name == "Particles.DynamicMaterialParameter") attribute.defaultValue = {1,1,1,1};
    slotEmitter.mesh.meshIndexAttribute = "Particles.MeshIndex";
    slotEmitter.mesh.meshes = {slotEmitter.mesh.meshes[0],slotEmitter.mesh.meshes[0]};
    CompiledSystem slotProgram; std::vector<Diagnostic> slotDiagnostics;
    ASSERT_TRUE(CompileSystem(slotSettings,slotProgram,slotDiagnostics)) << (slotDiagnostics.empty() ? "" : slotDiagnostics[0].message);
    const auto slotBackend = device->GetAdapter()->GetBackendType();
    ASSERT_TRUE(CompilePlatformPrograms(slotProgram,std::span(&slotBackend,1),root,slotDiagnostics)) << (slotDiagnostics.empty() ? "" : slotDiagnostics[0].message);
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(slotProgram,false),slotProgram,error)) << error;
    const auto& slotAsset = slotProgram.asset.emitters[0]; const auto& slotModules = slotProgram.platforms[0].emitters[0];
    auto slotRenderer = std::make_shared<MeshRenderer>();
    ASSERT_TRUE(slotRenderer->Initialize(device,slotAsset.attributes,slotModules.spriteVertex,slotModules.spriteFragment,targets,error,
        slotAsset.mesh,slotModules.sort,slotModules.visibility,slotModules.simulation.bounds,slotAsset.sprite,false)) << error;
    auto slotLeft = std::make_shared<TestMesh>(), slotRight = std::make_shared<TestMesh>();
    std::array<NLS::Render::Geometry::Vertex,4> leftVertices = vertices, rightVertices = vertices;
    for (auto& vertex : leftVertices) { vertex.position[0] = vertex.position[0] > 0 ? 0.1f : -0.1f; vertex.position[0] -= 0.35f; }
    for (auto& vertex : rightVertices) { vertex.position[0] = vertex.position[0] > 0 ? 0.1f : -0.1f; vertex.position[0] += 0.35f; }
    slotLeft->indices = mesh->indices; slotRight->indices = mesh->indices;
    slotLeft->vertices = buffer(leftVertices.data(),sizeof(leftVertices),rhi::BufferUsageFlags::Vertex);
    slotRight->vertices = buffer(rightVertices.data(),sizeof(rightVertices),rhi::BufferUsageFlags::Vertex);
    ASSERT_TRUE(slotLeft->vertices && slotRight->vertices);
    auto leftGeometry = std::make_shared<MeshRenderGeometry>(); leftGeometry->levels.push_back({slotLeft,1.0f}); leftGeometry->bounds = {0,0,0,0.2f};
    auto rightGeometry = std::make_shared<MeshRenderGeometry>(); rightGeometry->levels.push_back({slotRight,1.0f}); rightGeometry->bounds = {0,0,0,0.2f};
    ParticleDataSet slots; slots.Initialize(slotAsset.attributes,2);
    ASSERT_TRUE(slots.Spawn(7)); ASSERT_TRUE(slots.Spawn(8));
    for (uint32_t index = 0; index < 2; ++index)
    {
        slots.Write(0,index,{0,0,0.5f,0});
        slots.Write(slots.FindAttribute("Particles.Color"),index,{1,1,1,1});
        slots.Write(slots.FindAttribute("Particles.Scale"),index,{1,1,1,0});
        slots.Write(slots.FindAttribute("Particles.MeshOrientation"),index,{0,0,0,1});
        slots.Write(slots.FindAttribute("Particles.MeshIndex"),index,{static_cast<float>(index),0,0,0});
    }
    std::vector<SpriteMaterial> slotMaterials(2);
    slotMaterials[0].tint = {1,0,0,1}; slotMaterials[1].tint = {0,0,1,1};
    const auto slotImage = render(&slots,nullptr,view,nullptr,slotRenderer.get(),{leftGeometry,rightGeometry},slotMaterials);
    ASSERT_TRUE(slotImage.draw); ASSERT_EQ(slotImage.draw->meshSlots.size(),2u);
    EXPECT_EQ(slotImage.draw->meshSlots[0].argumentOffset,48u); EXPECT_EQ(slotImage.draw->meshSlots[1].argumentOffset,68u);
    EXPECT_GT(pixel(slotImage,21,32,0),200); EXPECT_LT(pixel(slotImage,21,32,2),20);
    EXPECT_GT(pixel(slotImage,43,32,2),200); EXPECT_LT(pixel(slotImage,43,32,0),20);
    EXPECT_LT(pixel(slotImage,32,32,0)+pixel(slotImage,32,32,1)+pixel(slotImage,32,32,2),20);
    ParticleViewStatistics slotStatistics;
    rhi::RHIBufferReadbackDesc slotRead; slotRead.source = slotImage.draw->arguments; slotRead.size = sizeof(slotStatistics); slotRead.data = &slotStatistics;
    ASSERT_TRUE(readBuffer(slotRead));
    EXPECT_EQ(slotStatistics.particleCount,2u); EXPECT_EQ(slotStatistics.visibleCount,2u); EXPECT_EQ(slotStatistics.vertexCount,6u);
    std::array<uint32_t,10> slotArguments{};
    slotRead.sourceOffset = 48; slotRead.size = sizeof(slotArguments); slotRead.data = slotArguments.data();
    ASSERT_TRUE(readBuffer(slotRead));
    EXPECT_EQ(slotArguments[0],6u); EXPECT_EQ(slotArguments[1],2u);
    EXPECT_EQ(slotArguments[5],6u); EXPECT_EQ(slotArguments[6],2u);
    GpuDataSet gpuSlots; ASSERT_TRUE(gpuSlots.Initialize(device,slots,error)) << error;
    const auto gpuSlotImage = render(nullptr,&gpuSlots,view,nullptr,slotRenderer.get(),{leftGeometry,rightGeometry},slotMaterials);
    ASSERT_TRUE(gpuSlotImage.draw); EXPECT_EQ(gpuSlotImage.pixels,slotImage.pixels);
    // Slot textures and samplers must also be independent of the emitter material.
    const auto solidTexture = [&](uint32_t rgba)
    {
        rhi::RHITextureDesc desc; desc.extent = {1,1,1}; desc.format = rhi::TextureFormat::RGBA8;
        rhi::RHITextureUploadDesc upload; upload.data = &rgba; upload.dataSize = upload.rowPitch = upload.slicePitch = 4;
        return device->CreateTexture(desc,upload);
    };
    auto texturedMaterials = slotMaterials;
    for (auto& material : texturedMaterials) material.tint = {1,1,1,1};
    texturedMaterials[0].texture = solidTexture(0xff0000ffu); texturedMaterials[1].texture = solidTexture(0xffff0000u);
    ASSERT_TRUE(texturedMaterials[0].texture && texturedMaterials[1].texture);
    const auto texturedSlots = render(&slots,nullptr,view,nullptr,slotRenderer.get(),{leftGeometry,rightGeometry},texturedMaterials);
    ASSERT_TRUE(texturedSlots.draw); EXPECT_EQ(texturedSlots.pixels,slotImage.pixels);
    EXPECT_NE(texturedSlots.draw->meshSlots[0].texture->GetTexture(),texturedSlots.draw->meshSlots[1].texture->GetTexture());
    // Exercise both packed uint4 index-count groups, the final slot, 256-thread
    // boundaries, nearest-index clamping and nonfinite inputs on actual devices.
    auto maximumAsset = slotSettings;
    auto& maximumEmitter = maximumAsset.emitters[0];
    maximumEmitter.mesh.meshes.resize(kMaxMeshSlots,maximumEmitter.mesh.meshes.front());
    CompiledSystem maximumProgram;
    ASSERT_TRUE(CompileSystem(maximumAsset,maximumProgram,slotDiagnostics));
    ASSERT_TRUE(CompilePlatformPrograms(maximumProgram,std::span(&slotBackend,1),root,slotDiagnostics)) << (slotDiagnostics.empty() ? "" : slotDiagnostics[0].message);
    const auto& maximumModules = maximumProgram.platforms[0].emitters[0];
    MeshRenderer maximumRenderer;
    ASSERT_TRUE(maximumRenderer.Initialize(device,maximumEmitter.attributes,maximumModules.spriteVertex,maximumModules.spriteFragment,targets,error,
        maximumEmitter.mesh,maximumModules.sort,maximumModules.visibility,maximumModules.simulation.bounds,maximumEmitter.sprite,true)) << error;
    auto triangle = std::make_shared<TestMesh>(); triangle->vertices = mesh->vertices; triangle->indices = mesh->indices; triangle->indexCount = 3;
    auto triangleGeometry = std::make_shared<MeshRenderGeometry>(); triangleGeometry->levels.push_back({triangle,1}); triangleGeometry->bounds = geometry->bounds;
    std::vector<std::shared_ptr<const MeshRenderGeometry>> maximumShapes(kMaxMeshSlots,geometry);
    maximumShapes.back() = triangleGeometry;
    ParticleDataSet maximumData; maximumData.Initialize(maximumEmitter.attributes,257);
    const std::array<float,12> meshIndices{-1,0,0.49f,0.5f,1,4,7,8,1e30f,
        std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};
    for (uint32_t index = 0; index < 257; ++index)
    {
        ASSERT_TRUE(maximumData.Spawn(index+100));
        maximumData.Write(0,index,{0,0,0.5f,0});
        maximumData.Write(maximumData.FindAttribute("Particles.MeshIndex"),index,{meshIndices[index%meshIndices.size()],0,0,0});
    }
    const auto maximumImage = render(&maximumData,nullptr,view,nullptr,&maximumRenderer,maximumShapes);
    ASSERT_TRUE(maximumImage.draw); ASSERT_EQ(maximumImage.draw->meshSlots.size(),kMaxMeshSlots);
    GpuDataSet maximumGpu; ASSERT_TRUE(maximumGpu.Initialize(device,maximumData,error)) << error;
    const auto maximumGpuImage = render(nullptr,&maximumGpu,view,nullptr,&maximumRenderer,maximumShapes);
    ASSERT_TRUE(maximumGpuImage.draw); EXPECT_EQ(maximumImage.pixels,maximumGpuImage.pixels);
    std::vector<Value> maximumBounds(kMaxMeshSlots,geometry->bounds);
    const auto referenceBounds = CalculateMeshParticleBounds(maximumData,maximumEmitter.mesh,maximumBounds);
    ParticleBounds maximumActualBounds;
    slotRead = {}; slotRead.source = maximumImage.draw->calculatedBounds->bounds; slotRead.size = sizeof(maximumActualBounds); slotRead.data = &maximumActualBounds;
    ASSERT_TRUE(readBuffer(slotRead)); EXPECT_EQ(maximumActualBounds.particleCount,referenceBounds.particleCount);
    EXPECT_EQ(maximumActualBounds.invalidCount,referenceBounds.invalidCount);
    for (size_t component = 0; component < 3; ++component)
    { EXPECT_NEAR(maximumActualBounds.minimum[component],referenceBounds.minimum[component],1e-5f); EXPECT_NEAR(maximumActualBounds.maximum[component],referenceBounds.maximum[component],1e-5f); }
    const auto referenceVisibility = CalculateMeshParticleVisibility(maximumData,maximumEmitter.mesh,maximumBounds,view,std::vector<uint32_t>(kMaxMeshSlots,6));
    slotRead.source = maximumImage.draw->arguments; slotRead.size = sizeof(slotStatistics); slotRead.data = &slotStatistics;
    ASSERT_TRUE(readBuffer(slotRead)); EXPECT_EQ(slotStatistics.visibleCount,referenceVisibility.visibleCount);
    EXPECT_EQ(slotStatistics.invalidCount,referenceVisibility.invalidCount);
    std::array<uint32_t,kMaxMeshSlots*5> maximumArguments{};
    slotRead.sourceOffset = 48; slotRead.size = sizeof(maximumArguments); slotRead.data = maximumArguments.data();
    ASSERT_TRUE(readBuffer(slotRead));
    for (size_t slot = 0; slot < kMaxMeshSlots; ++slot)
    {
        EXPECT_EQ(maximumArguments[slot*5],slot+1 == kMaxMeshSlots ? 3u : 6u);
        EXPECT_EQ(maximumArguments[slot*5+1],referenceVisibility.visibleCount);
        EXPECT_EQ(maximumArguments[slot*5+2],0u); EXPECT_EQ(maximumArguments[slot*5+3],0u); EXPECT_EQ(maximumArguments[slot*5+4],0u);
    }
    // Draw resources, not the caller's mesh handle, own geometry through completion.
    std::weak_ptr<rhi::RHIBuffer> retained = mesh->vertices; geometry.reset(); offCenter.reset(); mesh.reset();
    EXPECT_FALSE(retained.expired()); EXPECT_TRUE(horizontal.draw->meshSlots[0].mesh->GetIndexBuffer());
    if (vulkan) EXPECT_EQ(NLS::Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}
}

TEST(VfxMeshTests, SettingsRoundTripAndCompilerRejectsInvalidBindings)
{
    auto asset = MeshSystem(); auto& emitter = asset.emitters[0]; emitter.mesh.defaultScale = {-2,3,0.5f,0}; emitter.mesh.defaultOrientation = {0,0,5,5};
    SystemAsset copy; std::string error; ASSERT_TRUE(DeserializeSystem(SerializeSystem(asset),copy,error)) << error;
    EXPECT_EQ(copy.emitters[0].renderer,RendererType::Mesh); EXPECT_EQ(copy.emitters[0].mesh.defaultScale,emitter.mesh.defaultScale);
    ASSERT_EQ(copy.emitters[0].mesh.meshes.size(),1u);
    EXPECT_EQ(copy.emitters[0].mesh.meshes[0].guid,emitter.mesh.meshes[0].guid); EXPECT_EQ(copy.emitters[0].mesh.meshes[0].localId,emitter.mesh.meshes[0].localId);
    EXPECT_EQ(copy.emitters[0].mesh.meshIndexAttribute,"Particles.MeshIndex"); EXPECT_EQ(copy.emitters[0].mesh.lodIndex,emitter.mesh.lodIndex);
    CompiledSystem program; std::vector<Diagnostic> diagnostics; EXPECT_TRUE(CompileSystem(copy,program,diagnostics));
    copy.emitters[0].attributes.back().name = "Unused"; copy.emitters[0].attributes[6].type = ValueType::Float2;
    EXPECT_FALSE(CompileSystem(copy,program,diagnostics)); EXPECT_NE(diagnostics[0].message.find("Scale"),std::string::npos);
    auto json = nlohmann::json::parse(SerializeSystem(asset)); json["emitters"][0]["renderer"] = 99;
    EXPECT_FALSE(DeserializeSystem(json.dump(),copy,error)); EXPECT_EQ(copy.emitters[0].renderer,RendererType::Mesh);
    json = nlohmann::json::parse(SerializeSystem(CreateDefaultSystem())); json["emitters"][0].erase("renderer"); json["emitters"][0].erase("mesh");
    ASSERT_TRUE(DeserializeSystem(json.dump(),copy,error)); EXPECT_EQ(copy.emitters[0].renderer,RendererType::Sprite);
    auto attributes = asset.emitters[0].attributes;
    attributes[3].type = ValueType::Float3; // Mesh ignores Sprite Size.
    attributes[6].type = ValueType::Float; // Scalar mesh scale is broadcast.
    std::string source;
    EXPECT_TRUE(BuildMeshShader(attributes,source,error,emitter.mesh)) << error;
    EXPECT_TRUE(BuildGpuBoundsShader(attributes,source,error,&emitter.mesh)) << error;
    EXPECT_TRUE(BuildGpuVisibilityShader(attributes,source,error,&emitter.mesh)) << error;
    auto fallback = emitter.mesh; fallback.scaleAttribute = "Missing.Scale"; fallback.orientationAttribute.clear();
    EXPECT_TRUE(BuildMeshShader(attributes,source,error,fallback)) << error;
    attributes[7].type = ValueType::Float3;
    EXPECT_FALSE(BuildMeshShader(attributes,source,error,emitter.mesh));
    auto slots = asset; auto& mesh = slots.emitters[0].mesh;
    mesh.meshes.resize(kMaxMeshSlots,mesh.meshes.front()); mesh.materials.resize(kMaxMeshSlots);
    mesh.materials.back() = {"7b9eb982-9b8e-4c24-91b5-3dcb6303fbad",42}; mesh.meshIndexAttribute = "Particles.Shape"; mesh.lodIndex = 255;
    ASSERT_TRUE(DeserializeSystem(SerializeSystem(slots),copy,error)) << error;
    EXPECT_EQ(copy.emitters[0].mesh.meshes.size(),kMaxMeshSlots); EXPECT_EQ(copy.emitters[0].mesh.materials.back().guid,mesh.materials.back().guid);
    EXPECT_EQ(copy.emitters[0].mesh.meshIndexAttribute,"Particles.Shape"); EXPECT_EQ(copy.emitters[0].mesh.lodIndex,255);
    EXPECT_EQ(ResolveMeshSlotMaterialSettings(slots.emitters[0].sprite,mesh,kMaxMeshSlots-1).material.guid,mesh.materials.back().guid);
    mesh.meshes.push_back(mesh.meshes.front()); EXPECT_FALSE(CompileSystem(slots,program,diagnostics));
    mesh.meshes.pop_back(); slots.emitters[0].attributes.push_back({"Particles.Shape",ValueType::Float2,{}});
    EXPECT_FALSE(CompileSystem(slots,program,diagnostics));
    // Legacy single-mesh source assets migrate without changing their identity.
    json = nlohmann::json::parse(SerializeSystem(asset));
    auto& legacy = json["emitters"][0]["mesh"]; legacy["asset"] = legacy["meshes"][0]; legacy.erase("meshes");
    ASSERT_TRUE(DeserializeSystem(json.dump(),copy,error)) << error; EXPECT_EQ(copy.emitters[0].mesh.meshes.size(),1u);
}
TEST(VfxMeshTests, LodSelectionUsesRowMajorProjectionScaleAndMinimumLevel)
{
    MeshRenderGeometry geometry; geometry.bounds = {0,0,0,1};
    geometry.levels = {{nullptr,1},{nullptr,0.5f},{nullptr,0.25f}};
    SpriteView view;
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,-1),0u);
    view.viewProjection[14] = 1; view.viewProjection[15] = 0;
    view.localToWorld[11] = 2;
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,-1),1u);
    view.localToWorld[11] = 8;
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,-1),2u);
    view.localToWorld[11] = 2; view.localToWorld[0] = -3;
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,-1),0u);
    geometry.firstLod = 2;
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,0),0u);
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,2),0u);
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,3),1u);
    EXPECT_EQ(geometry.ChooseLod(view.viewProjection,view.localToWorld,view.cameraUp,255),2u);
}
TEST(VfxMeshTests, Dx12IndexedInstancesAndBounds) { ValidateMeshDevice(false); }
TEST(VfxMeshTests, VulkanIndexedInstancesAndBounds) { ValidateMeshDevice(true); }
