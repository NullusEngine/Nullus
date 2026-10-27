#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <GLFW/glfw3.h>
#include <Guid.h>
#include <VFX/GpuDataSet.h>
#include <VFX/SpriteRenderer.h>
#include <VFXCompiler/VfxCompiler.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>

namespace
{
void ValidateSpritePixels(bool vulkan)
{
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    namespace context = NLS::Render::Context;
    namespace shader = NLS::Render::ShaderCompiler;
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    struct Window { GLFWwindow* handle = nullptr; ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } } } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan WSI unavailable";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(64,64,"Sprite validation",nullptr,nullptr);
        if (!window.handle) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI window unavailable"; }
    }
    NLS::Render::Settings::DriverSettings settings; settings.debugMode = false;
    settings.graphicsBackend = vulkan ? NLS::Render::Settings::EGraphicsBackend::VULKAN : NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.platformWindow = window.handle;
    auto device = NLS::Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested GPU is unavailable";
    const auto validationBefore = NLS::Render::Backend::GetVulkanValidationErrorCountForTesting();
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"SpritePixels"); ASSERT_TRUE(pool);
    const auto root = std::filesystem::temp_directory_path()/("NullusSprite_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root,error); } } cleanup{root};
    auto attributes = CreateDefaultSystem().emitters[0].attributes;
    attributes[3].type = ValueType::Float2;
    attributes.push_back({"Particles.Rotation",ValueType::Float,{}});
    attributes.push_back({"Particles.SubImageIndex",ValueType::Float,{}});
    // Per-particle facing/alignment are optional columns; declaring them here
    // exercises the module-driven orientation path end to end.
    attributes.push_back({"Particles.SpriteFacing",ValueType::Float3,{}});
    attributes.push_back({"Particles.SpriteAlignment",ValueType::Float3,{}});
    const std::array<const char*,4> dynamicNames{"Particles.DynamicMaterialParameter","Particles.DynamicMaterialParameter1","Particles.DynamicMaterialParameter2","Particles.DynamicMaterialParameter3"};
    for (const auto* name : dynamicNames) attributes.push_back({name,ValueType::Float4,{1,1,1,1}});
    std::string source,error; ASSERT_TRUE(BuildSpriteShader(attributes,source,error)) << error;
    const auto file = root/"sprite.hlsl"; { std::ofstream output(file); output << source; }
    std::array<rhi::RHIShaderModuleDesc,2> shaders;
    shader::ShaderCompiler compiler;
    for (uint32_t i = 0; i < shaders.size(); ++i)
    {
        shader::ShaderCompilationInput input; input.assetPath = file.string();
        input.stage = i ? shader::ShaderStage::Pixel : shader::ShaderStage::Vertex;
        input.options.targetPlatform = vulkan ? shader::ShaderTargetPlatform::SPIRV : shader::ShaderTargetPlatform::DXIL;
        input.options.entryPoint = i ? "SpritePS" : "SpriteVS"; input.options.targetProfile = i ? "ps_6_0" : "vs_6_0";
        input.options.artifactDirectory = (root/"Artifacts").string();
        const auto binary = compiler.Compile(input);
        ASSERT_EQ(binary.status,shader::ShaderCompilationStatus::Succeeded) << binary.diagnostics;
        shaders[i].stage = i ? rhi::ShaderStage::Fragment : rhi::ShaderStage::Vertex;
        shaders[i].targetBackend = device->GetAdapter()->GetBackendType(); shaders[i].entryPoint = input.options.entryPoint; shaders[i].bytecode = binary.bytecode;
    }
    const std::array<uint8_t,16> atlas{255,0,0,128, 0,255,0,128, 0,0,255,128, 255,255,0,128};
    rhi::RHITextureDesc textureDesc; textureDesc.extent = {2,2,1}; textureDesc.format = rhi::TextureFormat::RGBA8;
    rhi::RHITextureUploadDesc upload; upload.data = atlas.data(); upload.dataSize = atlas.size(); upload.rowPitch = 8; upload.slicePitch = 16;
    upload.extent = textureDesc.extent;
    auto texture = device->CreateTexture(textureDesc,upload); ASSERT_TRUE(texture);
    SpriteDrawContext material; material.material.texture = texture;
    material.material.sampler.minFilter = rhi::TextureFilter::Nearest; material.material.sampler.magFilter = rhi::TextureFilter::Nearest;
    struct Layer
    {
        SpriteSettings settings;
        Value color{1,1,1,1}, size{1.5f,1.5f,0,0}, velocity{0,1,0,0};
        // Zero means "declared but not driven"; the renderer-wide alignment stays in charge.
    Value facing{0,0,0,0}, alignment{0,0,0,0};
        float frame = 0, rotation = 0, z = 0.25f;
        std::array<Value,4> dynamic{{Value{1,1,1,1},Value{1,1,1,1},Value{1,1,1,1},Value{1,1,1,1}}};
        bool gpu = false;
        Layer() { settings.circularMask = false; settings.subImagesX = settings.subImagesY = 2; }
    };
    const auto render = [&](const std::vector<Layer>& layers, SpriteDrawContext drawContext, float depth = 1, bool sampleDepth = false,
        Value background = {0.1f,0.1f,0.1f,1})
    {
        std::vector<uint8_t> pixels;
        rhi::RHITextureDesc colorDesc; colorDesc.extent = {64,64,1}; colorDesc.format = rhi::TextureFormat::RGBA8;
        colorDesc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
        const auto color = device->CreateTexture(colorDesc);
        rhi::RHITextureViewDesc view; view.format = colorDesc.format;
        const auto output = color ? device->CreateTextureView(color,view) : nullptr;
        auto depthDesc = colorDesc; depthDesc.format = rhi::TextureFormat::Depth32F;
        depthDesc.usage = rhi::TextureUsageFlags::DepthStencilAttachment|rhi::TextureUsageFlags::Sampled;
        const auto depthTexture = device->CreateTexture(depthDesc); view.format = depthDesc.format;
        const auto depthView = depthTexture ? device->CreateTextureView(depthTexture,view) : nullptr;
        auto commands = pool->CreateCommandBuffer("SpritePixelDraw");
        EXPECT_TRUE(output && depthView && commands); if (!output || !depthView || !commands) return pixels;
        commands->Begin();
        rhi::RHIRenderPassDesc pass; pass.renderArea = {0,0,64,64};
        rhi::RHIRenderPassColorAttachmentDesc attachment; attachment.view = output;
        attachment.clearValue = {background[0],background[1],background[2],background[3]};
        pass.colorAttachments = {attachment}; rhi::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
        depthAttachment.view = depthView; depthAttachment.clearValue.depth = depth; pass.depthStencilAttachment = depthAttachment;
        commands->BeginRenderPass(pass); commands->EndRenderPass();
        pass.colorAttachments[0].loadOp = rhi::LoadOp::Load; pass.depthStencilAttachment->depthLoadOp = rhi::LoadOp::Load;
        if (sampleDepth) drawContext.sceneDepth = depthView;
        std::vector<std::shared_ptr<const SpriteDrawResources>> draws;
        for (const auto& layer : layers)
        {
            ParticleDataSet cpu; cpu.Initialize(attributes,1); EXPECT_TRUE(cpu.Spawn(1));
            cpu.Write(0,0,{0,0,layer.z,0}); cpu.Write(1,0,layer.velocity); cpu.Write(2,0,layer.color); cpu.Write(3,0,layer.size);
            cpu.Write(cpu.FindAttribute("Particles.Rotation"),0,{layer.rotation,0,0,0});
            cpu.Write(cpu.FindAttribute("Particles.SubImageIndex"),0,{layer.frame,0,0,0});
            cpu.Write(cpu.FindAttribute("Particles.SpriteFacing"),0,layer.facing);
            cpu.Write(cpu.FindAttribute("Particles.SpriteAlignment"),0,layer.alignment);
            for (size_t i = 0; i < dynamicNames.size(); ++i) cpu.Write(cpu.FindAttribute(dynamicNames[i]),0,layer.dynamic[i]);
            SpriteRenderer renderer; rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {colorDesc.format};
            targets.hasDepth = true; targets.depthFormat = depthDesc.format;
            const bool initialized = renderer.Initialize(device,attributes,shaders[0],shaders[1],targets,error,nullptr,nullptr,nullptr,layer.settings);
            EXPECT_TRUE(initialized) << error; if (!initialized) return pixels;
            pass.depthStencilAttachment->readOnlyDepthStencil = !renderer.WritesDepth();
            SpriteView camera; camera.frustumCulling = false;
            std::shared_ptr<const SpriteDrawResources> draw;
            if (layer.gpu)
            {
                GpuDataSet data; const bool uploaded = data.Initialize(device,cpu,error); EXPECT_TRUE(uploaded) << error;
                if (!uploaded) return pixels;
                draw = renderer.Record(*commands,data,camera,pass,error,drawContext);
            }
            else
            {
                const auto data = UploadCpuRenderData(device,cpu,error); EXPECT_TRUE(data) << error; if (!data) return pixels;
                draw = renderer.Record(*commands,*data,camera,pass,error,drawContext);
            }
            EXPECT_TRUE(draw) << error; if (!draw) return pixels;
            EXPECT_EQ(draw->pipeline->GetDesc().depthStencilState.depthWrite,renderer.WritesDepth());
            draws.push_back(std::move(draw));
        }
        commands->End(); auto fence = device->CreateFence("SpritePixelsComplete"); EXPECT_TRUE(fence); if (!fence) return pixels;
        fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        const auto submitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_TRUE(submitted.Succeeded()) << submitted.message; if (!submitted.Succeeded()) return pixels;
        EXPECT_TRUE(fence->Wait(30000000000ull));
        pixels.resize(64*64*4);
        const auto read = device->BeginReadPixels(color,0,0,64,64,NLS::Render::Settings::EPixelDataFormat::RGBA,
            NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data()); EXPECT_TRUE(read.Succeeded()) << read.message;
        if (read.completion) EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        return pixels;
    };
    const auto expectCenter = [&](const auto& pixels, const Value& expected)
    {
        ASSERT_EQ(pixels.size(),64*64*4);
        for (size_t channel = 0; channel < 4; ++channel) EXPECT_NEAR(pixels[(32*64+32)*4+channel],expected[channel],3) << "channel " << channel;
    };
    for (const bool gpu : {false,true})
    {
        SCOPED_TRACE(gpu ? "GPU SoA" : "CPU frozen data");
        Layer layer; layer.gpu = gpu;
        expectCenter(render({layer},material),{141,13,13,255});
        layer.frame = 2; expectCenter(render({layer},material),{13,13,141,255});
        layer.frame = 0.5f; layer.settings.blendSubImages = true;
        expectCenter(render({layer},material),{77,77,13,255});
        layer.frame = 100; expectCenter(render({layer},material),{141,141,13,255});
        layer.frame = -10; expectCenter(render({layer},material),{141,13,13,255});
        layer.frame = 0; layer.settings.blend = SpriteBlendMode::Additive;
        expectCenter(render({layer},material),{154,26,26,255});
        layer.settings.blend = SpriteBlendMode::Premultiplied;
        expectCenter(render({layer},material),{141,13,13,255});
        auto tinted = material; tinted.material.tint = {0.5f,1,1,0.5f}; layer.settings.emissive = 2;
        expectCenter(render({layer},tinted),{83,19,19,255});
        layer.settings.emissive = 1; layer.settings.blend = SpriteBlendMode::Opaque;
        expectCenter(render({layer},material),{255,0,0,255});
        Layer behind = layer; behind.frame = 2; behind.z = 0.75f;
        layer.settings.blend = SpriteBlendMode::Masked; layer.settings.alphaCutoff = 0.6f;
        expectCenter(render({layer,behind},material),{0,0,255,255});
        layer.settings.alphaCutoff = 0.4f;
        expectCenter(render({layer,behind},material),{255,0,0,255});
        // Read-only DSV and depth SRV are simultaneously bound. Projection is
        // first orthographic, then perspective; distances use view-space units.
        layer = Layer{}; layer.gpu = gpu; layer.settings.softDistance = 0.5f;
        expectCenter(render({layer},{},0.5f,true,{0,0,0,1}),{128,128,128,255});
        SpriteDrawContext perspective; perspective.depthProjection = {-1.1f,-0.11f,-1,0};
        const float a = 0.11f/(1.1f-0.25f), b = 0.11f/(1.1f-0.5f);
        layer.settings.softDistance = (b-a)*2;
        expectCenter(render({layer},perspective,0.5f,true,{0,0,0,1}),{128,128,128,255});
        layer.settings.softDistance = 0; layer.settings.subImagesX = layer.settings.subImagesY = 1;
        layer.size = {1.5f,0.25f,0,0};
        const auto horizontal = render({layer},{},1,false,{0,0,0,1}); ASSERT_EQ(horizontal.size(),64*64*4);
        layer.rotation = 1.57079632679f;
        const auto rotated = render({layer},{},1,false,{0,0,0,1}); ASSERT_EQ(rotated.size(),horizontal.size());
        layer.rotation = 0; layer.settings.alignment = SpriteAlignment::Velocity; layer.velocity = {1,0,0,0};
        const auto aligned = render({layer},{},1,false,{0,0,0,1}); EXPECT_EQ(rotated,aligned);
        EXPECT_GT(horizontal[(32*64+48)*4],200); EXPECT_EQ(rotated[(32*64+48)*4],0);
        EXPECT_EQ(horizontal[(48*64+32)*4],0); EXPECT_GT(rotated[(48*64+32)*4],200);
        layer.velocity = {}; const auto stationary = render({layer},{},1,false,{0,0,0,1}); EXPECT_EQ(horizontal,stationary);
        // A per-particle facing vector drives orientation on its own, with no
        // velocity and no renderer-wide alignment. +X matches the rotated case.
        layer.settings.alignment = SpriteAlignment::Camera; layer.facing = {1,0,0,0};
        const auto attributeAligned = render({layer},{},1,false,{0,0,0,1}); EXPECT_EQ(rotated,attributeAligned);
        // An alignment axis de-projects the facing vector on the CPU and the
        // GPU: aiming the facing vector along that axis removes its component,
        // so the resulting screen vector differs from the raw facing case.
        layer.alignment = {1,0,0,0};
        const auto deprojected = render({layer},{},1,false,{0,0,0,1}); EXPECT_NE(attributeAligned,deprojected);
        EXPECT_GT(attributeAligned[(48*64+32)*4],200); EXPECT_EQ(attributeAligned[(32*64+48)*4],0);
    }
    // A fragment from an imported ParticleSprite pass uses the same vertex ABI
    // with its own material constants/texture/sampler and four particle inputs.
    const std::string custom = R"(
#include "NullusShaderLibrary/ParticleSprite.hlsl"
cbuffer CustomMaterial : register(b0,space2) { float4 _Tint; float _Strength; float3 _Padding; };
Texture2D<float4> _Detail : register(t0,space2);
SamplerState _DetailSampler : register(s0,space2);
float4 EvaluateParticleMaterial(ParticleMaterialInput input)
{
    return input.baseColor*_Tint*float4(input.dynamicParameter0.x,input.dynamicParameter1.y,input.dynamicParameter2.z,input.dynamicParameter3.w)*
        _Strength*_Detail.Sample(_DetailSampler,input.uv);
}
)";
    const auto customFile = root/"particle-material.hlsl"; { std::ofstream output(customFile); output << custom; }
    shader::ShaderCompilationInput input; input.assetPath = customFile.string(); input.stage = shader::ShaderStage::Pixel;
    input.options.entryPoint = "ParticleMaterialPS"; input.options.targetProfile = "ps_6_0";
    input.options.targetPlatform = vulkan ? shader::ShaderTargetPlatform::SPIRV : shader::ShaderTargetPlatform::DXIL;
    input.options.includeDirectories = {(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders").string()};
    input.options.artifactDirectory = (root/"MaterialArtifacts").string();
    const auto compiled = compiler.Compile(input); ASSERT_EQ(compiled.status,shader::ShaderCompilationStatus::Succeeded) << compiled.diagnostics;
    auto reflectionInput = input; reflectionInput.options.targetPlatform = shader::ShaderTargetPlatform::DXIL;
    const auto reflectionBinary = vulkan ? compiler.Compile(reflectionInput) : compiled;
    ASSERT_EQ(reflectionBinary.status,shader::ShaderCompilationStatus::Succeeded) << reflectionBinary.diagnostics;
    const auto reflection = compiler.Reflect(reflectionInput,reflectionBinary);
    ASSERT_TRUE(NLS::Render::Resources::ValidateParticleSpriteReflection(reflection,error)) << error;
    auto incompatible = reflection;
    for (auto& buffer : incompatible.constantBuffers) if (buffer.name == "VfxSpriteView") buffer.byteSize = 16;
    EXPECT_FALSE(NLS::Render::Resources::ValidateParticleSpriteReflection(incompatible,error));
    incompatible = reflection;
    NLS::Render::Resources::ShaderPropertyDesc writable; writable.name = "UnsupportedOutput";
    writable.kind = NLS::Render::Resources::ShaderResourceKind::StorageBuffer; writable.bindingSpace = 2;
    incompatible.properties.push_back(writable);
    EXPECT_FALSE(NLS::Render::Resources::ValidateParticleSpriteReflection(incompatible,error));
    incompatible.properties.back().kind = NLS::Render::Resources::ShaderResourceKind::Value;
    EXPECT_FALSE(NLS::Render::Resources::ValidateParticleSpriteReflection(incompatible,error));
    auto program = std::make_shared<SpriteMaterialProgram>(); program->identity = "TestMaterial";
    rhi::RHIShaderModuleDesc fragment; fragment.stage = rhi::ShaderStage::Fragment;
    fragment.targetBackend = device->GetAdapter()->GetBackendType(); fragment.entryPoint = "ParticleMaterialPS"; fragment.bytecode = compiled.bytecode;
    program->fragments = {fragment};
    program->layout.entries = {
        {"CustomMaterial",rhi::BindingType::UniformBuffer,1,0,1,rhi::ShaderStageMask::Fragment,2,0},
        {"_Detail",rhi::BindingType::Texture,1,0,1,rhi::ShaderStageMask::Fragment,2,0},
        {"_DetailSampler",rhi::BindingType::Sampler,1,0,1,rhi::ShaderStageMask::Fragment,2,0}};
    SpriteDrawContext customContext; customContext.material.program = program;
    const std::array<float,8> constants{1,1,1,1,1,0,0,0};
    SpriteMaterialConstant constant; constant.bytes.resize(sizeof(constants)); std::memcpy(constant.bytes.data(),constants.data(),sizeof(constants));
    customContext.material.constants = {constant};
    customContext.material.textures = {{0,{}}}; customContext.material.samplers = {{0,{}}};
    const uint32_t green = 0xff00ff00;
    auto detailDesc = textureDesc; detailDesc.extent = {1,1,1};
    auto detailUpload = upload; detailUpload.data = &green; detailUpload.dataSize = detailUpload.rowPitch = detailUpload.slicePitch = 4; detailUpload.extent = detailDesc.extent;
    const auto detail = device->CreateTexture(detailDesc,detailUpload); ASSERT_TRUE(detail);
    for (const bool gpu : {false,true})
    {
        SCOPED_TRACE(gpu ? "Custom material GPU" : "Custom material CPU");
        Layer layer; layer.gpu = gpu; layer.settings.subImagesX = layer.settings.subImagesY = 1;
        layer.dynamic[0][0] = 0.2f; layer.dynamic[1][1] = 0.4f; layer.dynamic[2][2] = 0.6f; layer.dynamic[3][3] = 0.8f;
        expectCenter(render({layer},customContext,1,false,{0,0,0,1}),{41,82,122,255});
        auto changed = customContext; const float strength = 0.5f;
        std::memcpy(changed.material.constants[0].bytes.data()+16,&strength,sizeof(strength));
        expectCenter(render({layer},changed,1,false,{0,0,0,1}),{10,20,31,255});
        expectCenter(render({layer},customContext,1,false,{0,0,0,1}),{41,82,122,255});
        changed = customContext; changed.material.textures[0].texture = detail;
        expectCenter(render({layer},changed,1,false,{0,0,0,1}),{0,82,0,255});
    }
    if (vulkan) EXPECT_EQ(NLS::Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}

/**
 * Ribbon strip pixel validation.
 *
 * Places several particles along Y so the strip must appear as a contiguous
 * vertical band. A sprite renderer would draw one quad per particle; the ribbon
 * connects consecutive particles, so the interior of the band has to be covered.
 */
static void ValidateRibbonPixels(bool vulkan)
{
    using namespace NLS::VFX;
    using namespace NLS;
    namespace context = Render::Context;
    namespace rhi = Render::RHI;
    namespace shader = Render::ShaderCompiler;
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    struct Window { GLFWwindow* handle = nullptr; ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } } } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan WSI unavailable";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(64,64,"Ribbon validation",nullptr,nullptr);
        if (!window.handle) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI window unavailable"; }
    }
    NLS::Render::Settings::DriverSettings settings; settings.debugMode = false;
    settings.graphicsBackend = vulkan ? NLS::Render::Settings::EGraphicsBackend::VULKAN : NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.platformWindow = window.handle;
    auto device = NLS::Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested GPU is unavailable";
    const auto validationBefore = NLS::Render::Backend::GetVulkanValidationErrorCountForTesting();
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"RibbonPixels"); ASSERT_TRUE(pool);
    const auto root = std::filesystem::temp_directory_path()/("NullusRibbon_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root,error); } } cleanup{root};

    // Ribbon attributes: the default set plus the scalar width column.
    auto attributes = CreateDefaultSystem().emitters[0].attributes;
    attributes.push_back({"Particles.RibbonWidth",ValueType::Float,{0.5f,0,0,0}});
    VFX::RibbonSettings ribbonSettings;
    ribbonSettings.blend = VFX::SpriteBlendMode::Additive;
    ribbonSettings.widthAttribute = "Particles.RibbonWidth";
    std::string source,error;
    ASSERT_TRUE(BuildRibbonShader(attributes,source,error,ribbonSettings)) << error;
    const auto file = root/"ribbon.hlsl"; { std::ofstream output(file); output << source; }
    std::array<rhi::RHIShaderModuleDesc,2> shaders;
    shader::ShaderCompiler compiler;
    for (uint32_t i = 0; i < shaders.size(); ++i)
    {
        shader::ShaderCompilationInput input; input.assetPath = file.string();
        input.stage = i ? shader::ShaderStage::Pixel : shader::ShaderStage::Vertex;
        input.options.targetPlatform = vulkan ? shader::ShaderTargetPlatform::SPIRV : shader::ShaderTargetPlatform::DXIL;
        input.options.entryPoint = i ? "RibbonPS" : "RibbonVS"; input.options.targetProfile = i ? "ps_6_0" : "vs_6_0";
        input.options.artifactDirectory = (root/"Artifacts").string();
        const auto binary = compiler.Compile(input);
        ASSERT_EQ(binary.status,shader::ShaderCompilationStatus::Succeeded) << binary.diagnostics;
        shaders[i].stage = i ? rhi::ShaderStage::Fragment : rhi::ShaderStage::Vertex;
        shaders[i].targetBackend = device->GetAdapter()->GetBackendType();
        shaders[i].entryPoint = input.options.entryPoint; shaders[i].bytecode = binary.bytecode;
    }

    // Four particles in a vertical line. The strip connects them, so every row
    // between the first and last particle must receive coverage.
    constexpr uint32_t particleCount = 4;
    ParticleDataSet cpu; cpu.Initialize(attributes,particleCount);
    const auto position = cpu.FindAttribute("Particles.Position");
    const auto color = cpu.FindAttribute("Particles.Color");
    const auto width = cpu.FindAttribute("Particles.RibbonWidth");
    for (uint32_t index = 0; index < particleCount; ++index)
    {
        ASSERT_TRUE(cpu.Spawn(0x1000u+index));
        // Vertical span from -0.6 to +0.6 in world units.
        cpu.Write(position,index,{0,-0.6f+0.4f*index,0,0});
        cpu.Write(color,index,{1,1,1,1});
        cpu.Write(width,index,{0.25f,0,0,0});
    }

    rhi::RHITextureDesc colorDesc; colorDesc.extent = {64,64,1}; colorDesc.format = rhi::TextureFormat::RGBA8;
    colorDesc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
    const auto target = device->CreateTexture(colorDesc);
    rhi::RHITextureViewDesc view; view.format = colorDesc.format;
    const auto output = target ? device->CreateTextureView(target,view) : nullptr;
    auto depthDesc = colorDesc; depthDesc.format = rhi::TextureFormat::Depth32F;
    depthDesc.usage = rhi::TextureUsageFlags::DepthStencilAttachment;
    const auto depthTexture = device->CreateTexture(depthDesc); view.format = depthDesc.format;
    const auto depthView = depthTexture ? device->CreateTextureView(depthTexture,view) : nullptr;
    ASSERT_TRUE(output && depthView);

    VFX::ParticleRenderer renderer; rhi::RHIRenderTargetLayoutDesc targets;
    targets.colorFormats = {colorDesc.format}; targets.hasDepth = true; targets.depthFormat = depthDesc.format;
    ASSERT_TRUE(renderer.Initialize(device,attributes,shaders[0],shaders[1],targets,error,nullptr,nullptr,nullptr,
        VFX::ResolveRibbonDrawSettings(ribbonSettings))) << error;

    auto commands = pool->CreateCommandBuffer("RibbonPixelDraw"); ASSERT_TRUE(commands);
    commands->Begin();
    rhi::RHIRenderPassDesc pass; pass.renderArea = {0,0,64,64};
    rhi::RHIRenderPassColorAttachmentDesc attachment; attachment.view = output;
    attachment.clearValue = {0,0,0,1}; pass.colorAttachments = {attachment};
    rhi::RHIRenderPassDepthStencilAttachmentDesc depthAttachment; depthAttachment.view = depthView;
    depthAttachment.clearValue.depth = 1; pass.depthStencilAttachment = depthAttachment;
    commands->BeginRenderPass(pass); commands->EndRenderPass();
    pass.colorAttachments[0].loadOp = rhi::LoadOp::Load; pass.depthStencilAttachment->depthLoadOp = rhi::LoadOp::Load;
    const auto data = UploadCpuRenderData(device,cpu,error); ASSERT_TRUE(data) << error;
    VFX::SpriteView camera; camera.frustumCulling = false;
    const auto draw = renderer.Record(*commands,*data,camera,pass,error);
    ASSERT_TRUE(draw) << error;
    commands->End();
    auto fence = device->CreateFence("RibbonPixelsComplete"); ASSERT_TRUE(fence);
    fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
    const auto submitted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
    ASSERT_TRUE(submitted.Succeeded()) << submitted.message;
    ASSERT_TRUE(fence->Wait(30000000000ull));
    std::vector<uint8_t> pixels(64*64*4);
    const auto read = device->BeginReadPixels(target,0,0,64,64,NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
    ASSERT_TRUE(read.Succeeded()) << read.message;
    if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());

    // Count lit pixels per row. A connected strip lights a contiguous run of
    // rows; isolated sprites would leave the row midpoints dark.
    uint32_t litRows = 0;
    const auto rowLit = [&](const uint32_t y)
    {
        for (uint32_t x = 0; x < 64; ++x)
            if (pixels[(y*64+x)*4] > 8 || pixels[(y*64+x)*4+1] > 8 || pixels[(y*64+x)*4+2] > 8) return true;
        return false;
    };
    for (uint32_t y = 0; y < 64; ++y) if (rowLit(y)) ++litRows;
    EXPECT_GT(litRows,8u) << "the ribbon must cover a vertical band of rows, not a single row";
    // Contiguity: no fully dark row may sit between two lit rows.
    int firstLit = -1, lastLit = -1;
    for (int y = 0; y < 64; ++y) if (rowLit(static_cast<uint32_t>(y))) { if (firstLit < 0) firstLit = y; lastLit = y; }
    ASSERT_GE(firstLit,0) << "the ribbon produced no pixels";
    uint32_t gaps = 0;
    for (int y = firstLit; y <= lastLit; ++y) if (!rowLit(static_cast<uint32_t>(y))) ++gaps;
    EXPECT_EQ(gaps,0u) << "the strip must be contiguous; a gap means the segments are not connected";
    if (vulkan) EXPECT_EQ(NLS::Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}

}

TEST(VfxSpriteTests, Dx12RibbonStripCoversAContiguousBand)
{
#if defined(_WIN32)
    ValidateRibbonPixels(false);
#else
    GTEST_SKIP() << "DX12 validation runs on Windows";
#endif
}
TEST(VfxSpriteTests, VulkanRibbonStripCoversAContiguousBand) { ValidateRibbonPixels(true); }
TEST(VfxSpriteTests, Dx12TextureSubUvBlendDepthAndAlignment)
{
#if defined(_WIN32)
    ValidateSpritePixels(false);
#else
    GTEST_SKIP() << "DX12 validation runs on Windows";
#endif
}
TEST(VfxSpriteTests, VulkanTextureSubUvBlendDepthAndAlignment) { ValidateSpritePixels(true); }
