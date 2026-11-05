#include <gtest/gtest.h>

#include <Engine/Rendering/ParticleSceneRenderer.h>
#include <Engine/Rendering/SpriteSceneRenderer.h>
#include <Rendering/Context/DriverInternal.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/Context/TransparentSubmission.h>
#include <Rendering/Entities/Camera.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/RHI/Core/RHIMesh.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>
#include <Rendering/Tooling/MaterialVisualEvidence.h>
#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxShaderCompiler.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
namespace R = NLS::Render;
namespace E = NLS::Engine;
namespace C = R::Context;
namespace H = R::RHI;

// Real vertex/index buffers exercise the shared indirect mesh draw recorder. Component/asset
// extraction is covered separately from this submission-boundary pixel test.
struct MixedTriangle final : H::RHIMesh
{
    std::shared_ptr<H::RHIBuffer> vertices, indices, arguments;
    std::shared_ptr<H::RHIBuffer> GetVertexBuffer() const override { return vertices; }
    std::shared_ptr<H::RHIBuffer> GetIndexBuffer() const override { return indices; }
    uint32_t GetVertexCount() const override { return 3; }
    uint32_t GetIndexCount() const override { return 3; }
    R::Settings::EPrimitiveMode GetPrimitiveMode() const override { return R::Settings::EPrimitiveMode::TRIANGLES; }
    uint32_t GetVertexStride() const override { return 3 * sizeof(float); }
    H::IndexType GetIndexType() const override { return H::IndexType::UInt32; }
};

TEST(SpriteMixedGpu, Dx12TranslatedCameraInterleavesMeshParticlesAndBothSpriteBatchPaths)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Windows DX12 acceptance";
#else
    auto& executor = C::RhiCommandListExecutor::Get();
    ASSERT_FALSE(executor.IsInitialized());
    executor.Initialize(C::RhiThreadMode::None, {});
    struct Shutdown { C::RhiCommandListExecutor& value; ~Shutdown() { value.Shutdown(); } } shutdown{executor};
    R::Settings::DriverSettings settings;
    settings.graphicsBackend = R::Settings::EGraphicsBackend::DX12;
    settings.debugMode = true;
    auto device = R::Backend::CreateRhiDevice(settings);
    ASSERT_TRUE(device && device->IsBackendReady());
    executor.SetTargets(C::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto root = std::filesystem::path(NLS_BUILD_DIR) / "sprite-mixed-gpu-artifacts";
    std::filesystem::create_directories(root);

    const auto compile = [&](const std::filesystem::path& source, bool pixel, const char* entry)
    {
        R::ShaderCompiler::ShaderCompilationInput input;
        input.assetPath = source.string();
        input.stage = pixel ? R::ShaderCompiler::ShaderStage::Pixel : R::ShaderCompiler::ShaderStage::Vertex;
        input.options.targetPlatform = R::ShaderCompiler::ShaderTargetPlatform::DXIL;
        input.options.entryPoint = entry;
        input.options.targetProfile = pixel ? "ps_6_0" : "vs_6_0";
        input.options.artifactDirectory = root.string();
        const auto output = R::ShaderCompiler::ShaderCompiler{}.Compile(input);
        EXPECT_EQ(output.status, R::ShaderCompiler::ShaderCompilationStatus::Succeeded) << output.diagnostics;
        H::RHIShaderModuleDesc module;
        module.stage = pixel ? H::ShaderStage::Fragment : H::ShaderStage::Vertex;
        module.targetBackend = H::NativeBackendType::DX12;
        module.entryPoint = entry;
        module.bytecode = output.bytecode;
        return module;
    };
    auto program = std::make_shared<E::Rendering::SpriteShaderSnapshot>();
    program->identity = 0x534d4958;
    const auto shader = std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/NullusShaderLibrary/SpriteUnlit.hlsl";
    program->shaders = {compile(shader, false, "SpriteVS"), compile(shader, true, "SpritePS")};
    program->materialLayout.entries.push_back({"MaterialProperties", H::BindingType::UniformBuffer,
        H::BindingPointMap::kMaterialDescriptorSet, 0, 1, H::ShaderStageMask::Fragment, H::BindingPointMap::kMaterialBindingSpace, 0});
    E::Rendering::SpriteShaderSnapshot::Constant tint;
    const std::array<float,4> white{1,1,1,1};
    tint.bytes.resize(sizeof(white));
    std::memcpy(tint.bytes.data(), white.data(), sizeof(white));
    program->constants.push_back(std::move(tint));
    const std::array<uint8_t,4> texel{255,255,255,255};
    H::RHITextureDesc textureDesc;
    textureDesc.extent = {1,1,1}; textureDesc.format = H::TextureFormat::RGBA8;
    H::RHITextureUploadDesc upload;
    upload.data = texel.data(); upload.dataSize = 4; upload.rowPitch = upload.slicePitch = 4; upload.extent = textureDesc.extent;
    auto texture = device->CreateTexture(textureDesc, upload);
    ASSERT_TRUE(texture);
    H::RHITextureViewDesc textureView; textureView.format = textureDesc.format;
    auto spriteTexture = device->CreateTextureView(texture, textureView);
    ASSERT_TRUE(spriteTexture);
    auto sprite = std::make_shared<NLS::Core::Assets::SpriteArtifact>();
    sprite->sourceWidth = sprite->sourceHeight = sprite->width = sprite->height = 1;
    sprite->pixelsPerUnit = 1;
    auto farSprite = std::make_shared<E::Rendering::SpriteRenderProxy>();
    farSprite->sprite = sprite; farSprite->program = program; farSprite->texture = spriteTexture;
    farSprite->ownerId = 101; farSprite->color = {1,0,0,0.5f};
    farSprite->world = NLS::Maths::Matrix4::Translation({0,0,-1});
    auto nearSprite = std::make_shared<E::Rendering::SpriteRenderProxy>(*farSprite);
    nearSprite->ownerId = 102; nearSprite->color = {1,1,1,0.5f};
    nearSprite->world = NLS::Maths::Matrix4::Translation({0,0,1});

    NLS::Maths::Transform cameraTransform;
    R::Entities::Camera camera(&cameraTransform);
    camera.SetProjectionMode(R::Settings::EProjectionMode::ORTHOGRAPHIC);
    camera.SetSize(1); camera.SetPosition({0,0,2}); camera.SetRotation({0,1,0,0}); camera.CacheMatrices(64,64);
    const auto depthOf = [&](float z) { return -(camera.GetViewMatrix() * NLS::Maths::Vector4(0,0,z,1)).z; };
    ASSERT_GT(depthOf(-1), depthOf(0)); ASSERT_GT(depthOf(0), depthOf(0.5f)); ASSERT_GT(depthOf(0.5f), depthOf(1));
    R::Data::FrameDescriptor frame; frame.camera = &camera; frame.renderWidth = frame.renderHeight = 64;
    const auto frozen = R::FrameGraph::FreezeFrameDescriptor(frame);

    const auto meshShader = root / "Triangle.hlsl";
    std::ofstream(meshShader) << R"(
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position,1); }
float4 PSMain() : SV_Target0 { return float4(0,1,0,0.5); }
)";
    const auto clip = camera.GetGpuProjectionMatrix() * camera.GetViewMatrix() * NLS::Maths::Vector4(0,0,0,1);
    const float meshDepth = clip.z / clip.w;
    const std::array<float,9> vertices{-.75f,-.75f,meshDepth, 0,.75f,meshDepth, .75f,-.75f,meshDepth};
    auto mesh = std::make_shared<MixedTriangle>();
    H::RHIBufferDesc vertexDesc; vertexDesc.size = sizeof(vertices); vertexDesc.usage = H::BufferUsageFlags::Vertex;
    vertexDesc.memoryUsage = H::MemoryUsage::CPUToGPU;
    H::RHIBufferUploadDesc vertexUpload; vertexUpload.data = vertices.data(); vertexUpload.dataSize = sizeof(vertices);
    mesh->vertices = device->CreateBuffer(vertexDesc, vertexUpload); ASSERT_TRUE(mesh->vertices);
    const std::array<uint32_t,3> indices{0,1,2};
    vertexDesc.size = sizeof(indices); vertexDesc.usage = H::BufferUsageFlags::Index;
    vertexUpload.data = indices.data(); vertexUpload.dataSize = sizeof(indices);
    mesh->indices = device->CreateBuffer(vertexDesc, vertexUpload); ASSERT_TRUE(mesh->indices);
    const std::array<uint32_t,5> arguments{3,1,0,0,0};
    vertexDesc.size = sizeof(arguments); vertexDesc.usage = H::BufferUsageFlags::Indirect;
    vertexUpload.data = arguments.data(); vertexUpload.dataSize = sizeof(arguments);
    mesh->arguments = device->CreateBuffer(vertexDesc, vertexUpload); ASSERT_TRUE(mesh->arguments);
    H::RHIGraphicsPipelineDesc meshPipeline;
    meshPipeline.pipelineLayout = device->CreatePipelineLayout({});
    meshPipeline.vertexShader = device->CreateShaderModule(compile(meshShader, false, "VSMain"));
    meshPipeline.fragmentShader = device->CreateShaderModule(compile(meshShader, true, "PSMain"));
    meshPipeline.vertexBuffers = {{0, 3 * sizeof(float), false}};
    meshPipeline.vertexAttributes = {{0,0,0,3 * sizeof(float),H::RHIVertexSemantic::Position,0,H::RHIVertexFormat::Float3}};
    meshPipeline.rasterState.cullEnabled = false;
    meshPipeline.depthStencilState.depthTest = true; meshPipeline.depthStencilState.depthWrite = false;
    meshPipeline.depthStencilState.depthCompare = R::Settings::EComparaisonAlgorithm::LESS_EQUAL;
    meshPipeline.blendState.enabled = true;
    H::RHIRenderTargetBlendStateDesc blend; blend.blendEnable = true; meshPipeline.blendState.renderTargets = {blend};
    meshPipeline.renderTargetLayout.colorFormats = {H::TextureFormat::RGBA8};
    meshPipeline.renderTargetLayout.hasDepth = true; meshPipeline.renderTargetLayout.depthFormat = H::TextureFormat::Depth32F;
    auto pipeline = device->CreateGraphicsPipeline(meshPipeline); ASSERT_TRUE(pipeline);

    auto system = NLS::VFX::CreateDefaultSystem("Mixed transparency");
    auto& emitter = system.emitters.front();
    emitter.target = NLS::VFX::SimulationTarget::CPU;
    emitter.spawn.nodes.clear(); emitter.update.nodes.clear();
    emitter.spawnRate = 0; emitter.duration = 1; emitter.looping = false; emitter.bursts = {{0,1}}; emitter.capacity = 4;
    emitter.sortByViewDepth = false; emitter.frustumCulling = false;
    emitter.attributes[0].defaultValue = {0,0,0.5f,0};
    emitter.attributes[1].defaultValue = {};
    emitter.attributes[2].defaultValue = {0,0,1,0.5f};
    emitter.attributes[3].defaultValue = {1,0,0,0};
    emitter.sprite.circularMask = false; emitter.sprite.blend = NLS::VFX::SpriteBlendMode::Translucent;
    auto compiled = std::make_shared<NLS::VFX::CompiledSystem>();
    std::vector<NLS::VFX::Diagnostic> diagnostics;
    ASSERT_TRUE(NLS::VFX::CompileSystem(system, *compiled, diagnostics));
    const auto backend = H::NativeBackendType::DX12;
    ASSERT_TRUE(NLS::VFX::CompilePlatformPrograms(*compiled, std::span(&backend,1), root / "Particles", diagnostics))
        << (diagnostics.empty() ? std::string{} : diagnostics.front().message);
    NLS::VFX::SystemInstance simulation; std::string error;
    ASSERT_TRUE(simulation.Initialize(compiled, error)) << error;
    ASSERT_TRUE(simulation.Tick(1.0 / 60));
    E::Rendering::ParticleViewInput particles;
    particles.proxy = std::make_shared<NLS::VFX::SystemRenderProxy>(compiled);
    particles.frame = simulation.Capture(); particles.ownerId = 301;
    ASSERT_TRUE(particles.frame && particles.frame->cpu[0]); ASSERT_EQ(particles.frame->cpu[0]->Count(), 1);
    E::Rendering::ParticleSceneRenderer particleRenderer;
    E::Rendering::SpriteSceneRenderer spriteRenderer;
    std::vector<uint8_t> firstPixels;
    for (bool cpu : {false,true})
    {
        H::RHITextureDesc outputDesc; outputDesc.extent = {64,64,1}; outputDesc.format = H::TextureFormat::RGBA8;
        outputDesc.usage = H::TextureUsageFlags::ColorAttachment | H::TextureUsageFlags::CopySrc;
        auto output = device->CreateTexture(outputDesc); ASSERT_TRUE(output);
        textureView.format = outputDesc.format;
        auto outputView = device->CreateTextureView(output, textureView); ASSERT_TRUE(outputView);
        outputDesc.format = H::TextureFormat::Depth32F; outputDesc.usage = H::TextureUsageFlags::DepthStencilAttachment;
        auto depth = device->CreateTexture(outputDesc); ASSERT_TRUE(depth);
        textureView.format = outputDesc.format;
        auto depthView = device->CreateTextureView(depth, textureView); ASSERT_TRUE(depthView);
        C::RenderScenePackage package; package.renderWidth = package.renderHeight = 64;
        package.externalSceneOutputColorView = outputView; package.externalSceneOutputDepthView = depthView;
        package.clearColorValue = {0,0,0,1};
        package.visibleDrawCount = package.drawCommandCount = package.transparentDrawCount = package.materialBatchCount = 1;
        package.hasVisibleDraws = package.hasTransparentPass = package.containsCommandInputs = true;
        C::RenderPassCommandInput meshPass; meshPass.kind = C::RenderPassCommandKind::Transparent;
        meshPass.renderWidth = meshPass.renderHeight = 64;
        meshPass.debugName = "MixedTriangleMesh"; meshPass.usesColorAttachment = meshPass.usesDepthStencilAttachment = true;
        meshPass.clearColor = meshPass.clearDepth = meshPass.writesDepthStencilAttachment = true;
        meshPass.colorAttachmentViews = {outputView}; meshPass.depthStencilAttachmentView = depthView;
        C::RecordedDrawCommandInput meshDraw; meshDraw.pipeline = pipeline; meshDraw.mesh = mesh; meshDraw.instanceCount = 1;
        meshDraw.indirectArguments = mesh->arguments;
        meshDraw.transparentSortKey = R::Sprites::SpriteSortKey{0,0,depthOf(0),201}; meshDraw.resourceOwner = mesh;
        meshPass.recordedDrawCommands = {meshDraw}; meshPass.drawCount = 1; package.passCommandInputs = {meshPass};
        ASSERT_TRUE(particleRenderer.AppendPass(device, package, frozen, {particles}, error)) << error;
        ASSERT_TRUE(spriteRenderer.AppendPass(device, package, frozen, {nearSprite,farSprite}, error, cpu)) << error;
        EXPECT_EQ(spriteRenderer.GetStatistics().batches, 2);
        EXPECT_EQ(spriteRenderer.GetStatistics().orderingBreaks, 1);
        C::FinalizeTransparentSubmission(package);
        std::vector<uint64_t> orderedOwners;
        for (const auto& pass : package.passCommandInputs)
            for (const auto& draw : pass.recordedDrawCommands)
                if (draw.transparentSortKey) orderedOwners.push_back(draw.transparentSortKey->stableId);
        EXPECT_EQ(orderedOwners, (std::vector<uint64_t>{101,201,301,102}));
        auto pool = device->CreateCommandPool(H::QueueType::Graphics, "SpriteMixedAcceptance");
        auto commands = pool->CreateCommandBuffer("SpriteMixedAcceptance"); commands->Begin();
        for (const auto& input : package.passCommandInputs)
        {
            if (C::Detail::HasResourceVisibilityTransitions(input))
                EXPECT_TRUE(C::Detail::RecordResourceVisibilityTransitions(*commands, input));
            EXPECT_EQ(C::Detail::RecordComputeDispatches(*commands, input.computeDispatchInputs), input.computeDispatchInputs.size());
            H::RHIRenderPassDesc pass; pass.renderArea = {0,0,64,64};
            H::RHIRenderPassColorAttachmentDesc color; color.view = outputView; color.clearValue = {0,0,0,1};
            color.loadOp = input.clearColor ? H::LoadOp::Clear : H::LoadOp::Load; pass.colorAttachments = {color};
            H::RHIRenderPassDepthStencilAttachmentDesc depthAttachment; depthAttachment.view = depthView;
            depthAttachment.clearValue.depth = 1; depthAttachment.depthLoadOp = input.clearDepth ? H::LoadOp::Clear : H::LoadOp::Load;
            pass.depthStencilAttachment = depthAttachment;
            commands->BeginRenderPass(pass); commands->SetViewport({0,0,64,64,0,1}); commands->SetScissor({0,0,64,64});
            EXPECT_EQ(C::Detail::RecordPreparedDrawCommandsForPass(commands.get(), input), input.drawCount);
            commands->EndRenderPass();
        }
        commands->End(); auto fence = device->CreateFence("SpriteMixedAcceptance"); fence->Reset();
        H::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        const auto submitted = device->GetQueue(H::QueueType::Graphics)->SubmitChecked(submit);
        ASSERT_TRUE(submitted.Succeeded()) << submitted.message; ASSERT_TRUE(fence->Wait(30000000000ull));
        std::vector<uint8_t> pixels(64 * 64 * 4);
        const auto read = device->BeginReadPixels(output, 0,0,64,64, R::Settings::EPixelDataFormat::RGBA,
            R::Settings::EPixelDataType::UNSIGNED_BYTE, pixels.data());
        ASSERT_TRUE(read.Succeeded()) << read.message;
        if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        const size_t center = (32 * 64 + 32) * 4;
        EXPECT_NEAR(pixels[center],143,2); EXPECT_NEAR(pixels[center+1],159,2); EXPECT_NEAR(pixels[center+2],191,2);
        std::string diagnostic;
        EXPECT_TRUE(R::Tooling::WriteMaterialVisualEvidencePng((root / (cpu ? "mixed-cpu.png" : "mixed-instanced.png")).string(),
            pixels.data(),64,64,4,256,&diagnostic)) << diagnostic;
        if (cpu) EXPECT_EQ(pixels, firstPixels); else firstPixels = std::move(pixels);
    }
#endif
}
}
