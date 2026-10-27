#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <GLFW/glfw3.h>
#include "Core/ServiceLocator.h"
#include "Assets/ArtifactManifest.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Guid.h"
#include "Math/Vector4.h"
#include "Rendering/Assets/MaterialAsset.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "MaterialCompiler/MaterialShaderCompiler.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/Resources/Loaders/MaterialLoader.h"
#include "Rendering/Resources/ShaderReflectionMerge.h"
#include "Rendering/Resources/SpirvShaderReflection.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/MaterialRuntimeResource.h"
#include "Rendering/Resources/MaterialInstance.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Resources/TextureCube.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

namespace
{
void ValidateMaterialGpu(bool vulkan)
{
    namespace R = NLS::Render;
    namespace GPU = R::RHI;
    namespace SC = R::ShaderCompiler;
    struct Window
    {
        GLFWwindow* handle = nullptr;
        ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } }
    } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan WSI unavailable";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(32,32,"Material validation",nullptr,nullptr);
        if (!window.handle) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI window unavailable"; }
    }
    R::Settings::DriverSettings settings;
    settings.graphicsBackend = vulkan ? R::Settings::EGraphicsBackend::VULKAN : R::Settings::EGraphicsBackend::DX12;
    settings.platformWindow = window.handle; settings.debugMode = false;
    R::Context::Driver driver(settings);
    R::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    const auto device = R::Context::DriverRendererAccess::GetExplicitDevice(driver);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested material GPU device unavailable";
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Core::ResourceManagement::ShaderManager manager;
    NLS::Core::ServiceLocator::Provide(manager);
    struct Services
    {
        ~Services() { NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
            NLS::Core::ServiceLocator::Remove<R::Context::Driver>(); }
    } services;
    const auto root = std::filesystem::temp_directory_path()/("NullusMaterialGpu_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path,ec); } } cleanup{root};
    const auto source = root/"Material.hlsl";
    { std::ofstream file(source); file << R"(
#if defined(LAYOUT_B)
cbuffer MaterialProperties : register(b3,space2) { float4 _Tint : packoffset(c2); };
#else
cbuffer MaterialProperties : register(b0,space2) { float4 _Tint; };
#endif
Texture2D _Map : register(t0,space2);
#if defined(CUBE_MODE)
TextureCube _Environment : register(t1,space2);
#endif
SamplerState sampler_Map : register(s0,space2);
struct Input { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output VSMain(Input input) { Output o; o.position=float4(input.position,1); o.uv=input.uv; return o; }
float4 PSMain(Output input) : SV_Target0 {
#if defined(CUBE_MODE)
return (_Environment.SampleLevel(sampler_Map,float3(1,0,0),1)+_Environment.SampleLevel(sampler_Map,float3(0,0,-1),1))*0.5*_Tint;
#else
return _Map.Sample(sampler_Map,input.uv)*_Tint;
#endif
}
)"; }
    std::ifstream inputSource(source);
    const std::string hlsl{std::istreambuf_iterator<char>(inputSource),std::istreambuf_iterator<char>()};
    const auto parsed = R::ShaderLab::ParseShaderLabSource(
        "Shader \"Material/Test\" { SubShader { Pass { Tags { \"LightMode\" = \"Forward\" }\nHLSLPROGRAM\n#pragma vertex VSMain\n#pragma fragment PSMain\n"+
        hlsl+"\nENDHLSL\n} } }",source.string());
    ASSERT_TRUE(parsed.Succeeded()) << parsed.DiagnosticsToString();
    R::ShaderLab::ShaderLabKeywordSet alternate; alternate.Enable("LAYOUT_B");
    R::ShaderLab::ShaderLabKeywordSet cubeKeywords;cubeKeywords.Enable("CUBE_MODE");
    NLS::MaterialCompiler::MaterialShaderCompileOptions compileOptions;
    compileOptions.sourcePath=source;
    compileOptions.generatedPath=root/"Generated"/"Material.hlsl";
    compileOptions.sourceIdentity="Assets/Material.shader";
    compileOptions.subAssetKey="shader:Material";
    compileOptions.targets={vulkan ? SC::ShaderTargetPlatform::SPIRV : SC::ShaderTargetPlatform::DXIL};
    compileOptions.variants={{},alternate,cubeKeywords}; compileOptions.generateGlsl=false;
    R::Resources::MaterialParameterSchema schema;
    R::Resources::MaterialParameterDesc cubeParameter;cubeParameter.id=1;cubeParameter.name="_Environment";
    cubeParameter.type=R::Resources::UniformType::UNIFORM_SAMPLER_CUBE;cubeParameter.defaultValue=std::string{};schema.parameters.push_back(cubeParameter);
    auto artifact=NLS::MaterialCompiler::CompileMaterialPass(parsed.asset.subShaders[0].passes[0],compileOptions,schema);
    for (const auto& stage : artifact.stages)
        ASSERT_EQ(stage.output.status,SC::ShaderCompilationStatus::Succeeded) << stage.output.diagnostics;
    ASSERT_EQ(artifact.variantReflections.size(),3);
    const auto alternateReflection=std::find_if(artifact.variantReflections.begin(),artifact.variantReflections.end(),[&](const auto& r){return r.keywordHash==alternate.Hash();});
    ASSERT_NE(alternateReflection,artifact.variantReflections.end());ASSERT_EQ(alternateReflection->reflection.constantBuffers[0].members[0].byteOffset,32);
    const auto shaderPath = root/"Library"/"Artifacts"/"Material"/std::string(64,'a');
    std::filesystem::create_directories(shaderPath.parent_path());
    { const auto bytes = R::Assets::SerializeShaderArtifact(artifact); std::ofstream file(shaderPath,std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
    R::Assets::MaterialAsset sourceMaterial; sourceMaterial.shader.path = artifact.sourcePath;
    sourceMaterial.parameters.push_back({0,"_Tint",R::Resources::UniformType::UNIFORM_FLOAT_VEC4,std::array<float,4>{1,0,0,1}});
    R::Resources::Loaders::MaterialLoader::LoadOptions options;
    options.loadMissingTextures = false; options.shaderPassArtifactPaths = {shaderPath.string()};
    auto* material = R::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload("Assets/Test.mat",
        R::Assets::SerializeMaterialAsset(sourceMaterial),options);
    ASSERT_NE(material,nullptr); ASSERT_TRUE(material->IsValid());
    const auto initialTint = material->Get<NLS::Maths::Vector4>("_Tint");
    EXPECT_FLOAT_EQ(initialTint.x,1.f);
    EXPECT_FLOAT_EQ(initialTint.w,1.f);
    struct MaterialOwner { R::Resources::Material*& value; ~MaterialOwner(){R::Resources::Loaders::MaterialLoader::Destroy(value);} } materialOwner{material};
    GPU::RHITextureDesc mapDesc; mapDesc.extent={1,1,1}; mapDesc.format=GPU::TextureFormat::RGBA8;
    mapDesc.usage=GPU::TextureUsageFlags::Sampled;
    const std::array<uint8_t,4> white{255,255,255,255};
    GPU::RHITextureUploadDesc upload; upload.data=white.data(); upload.dataSize=4; upload.rowPitch=4; upload.slicePitch=4;
    const auto map = device->CreateTexture(mapDesc,upload); ASSERT_NE(map,nullptr);
    auto texture = R::Resources::Texture2D::WrapExternal(map,1,1);
    material->Set("_Map",texture.get());
    GPU::SamplerDesc sampler; sampler.minFilter=GPU::TextureFilter::Nearest; sampler.magFilter=GPU::TextureFilter::Nearest;
    material->SetSamplerOverride("sampler_Map",sampler);
    const auto cache = GPU::CreateDefaultPipelineCache();
    struct FrozenDraw { std::shared_ptr<GPU::RHIGraphicsPipeline> pipeline; std::shared_ptr<GPU::RHIBindingSet> bindings; };
    std::array<FrozenDraw,2> draws;
    std::array<std::shared_ptr<const R::Resources::MaterialRuntimeResource>,2> resources;
    R::Data::PipelineState state; state.culling=false; state.depthTest=false; state.depthWriting=false;
    R::Resources::MaterialPipelineStateOverrides overrides; overrides.hasDepthAttachment=false; overrides.culling=false;
    for (size_t index=0; index<2; ++index)
    {
        if (index) { material->EnableKeyword("LAYOUT_B"); material->Set("_Tint",NLS::Maths::Vector4(0,1,0,1)); }
        // A fullscreen composite must still shade interior pixels while the scene is wireframe.
        if (index)
        {
            state.rasterizationMode = R::Settings::ERasterizationMode::LINE;
            overrides.wireframe = false;
        }
        draws[index].pipeline=material->BuildRecordedGraphicsPipeline(device,cache,R::Settings::EPrimitiveMode::TRIANGLES,state,overrides);
        draws[index].bindings=material->GetRecordedBindingSet(device);
        ASSERT_NE(draws[index].pipeline,nullptr);
        ASSERT_NE(draws[index].bindings,nullptr);
        EXPECT_FALSE(material->HasExplicitBindingErrors());
        resources[index]=material->CaptureRuntimeResource(device);
        ASSERT_NE(resources[index],nullptr);
    }
    EXPECT_NE(draws[0].bindings,draws[1].bindings);
    state.rasterizationMode = R::Settings::ERasterizationMode::FILL;
    overrides.wireframe.reset();
    R::Assets::TextureArtifactData cubeAsset;cubeAsset.width=cubeAsset.height=2;cubeAsset.dimension=GPU::TextureDimension::TextureCube;cubeAsset.arrayLayers=6;
    for(uint32_t level=0;level<2;++level)for(uint32_t face=0;face<6;++face)
    {
        R::Assets::TextureArtifactMip mip;mip.level=level;mip.width=mip.height=2u>>level;mip.rowPitch=mip.width*4;mip.slicePitch=mip.rowPitch*mip.height;
        for(uint32_t pixel=0;pixel<mip.width*mip.height;++pixel)
        {const std::array<uint8_t,4> color=level==1 && (face==0 || face==5)?std::array<uint8_t,4>{0,255,255,255}:std::array<uint8_t,4>{255,0,0,255};mip.pixels.insert(mip.pixels.end(),color.begin(),color.end());}
        cubeAsset.mips.push_back(mip);R::Assets::TextureArtifactSubresource sub;sub.level=level;sub.arrayLayer=face;
        sub.face=static_cast<R::Assets::TextureArtifactCubeFace>(face+1);sub.width=mip.width;sub.height=mip.height;sub.rowPitch=mip.rowPitch;sub.slicePitch=mip.slicePitch;
        cubeAsset.subresources.push_back(sub);
    }
    const auto cubeBytes=R::Assets::SerializeTextureArtifact(cubeAsset);ASSERT_FALSE(cubeBytes.empty());
    const auto cubePath=root/"Library"/"Artifacts"/"Material"/NLS::Core::Assets::BuildArtifactStorageFileName(cubeBytes.data(),cubeBytes.size());
    {std::ofstream file(cubePath,std::ios::binary);file.write(reinterpret_cast<const char*>(cubeBytes.data()),cubeBytes.size());}
    R::Assets::MaterialAsset cubeSource;cubeSource.shader.path=artifact.sourcePath;
    cubeSource.staticParameters["CUBE_MODE"]=true;
    cubeSource.parameters={{0,"_Tint",R::Resources::UniformType::UNIFORM_FLOAT_VEC4,std::array<float,4>{1,1,1,1}},
        {1,"_Environment",R::Resources::UniformType::UNIFORM_SAMPLER_CUBE,cubePath.string()}};
    auto cubeOptions=options;cubeOptions.loadMissingTextures=true;
    auto* cubeMaterial=R::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload("Assets/Cube.mat",R::Assets::SerializeMaterialAsset(cubeSource),cubeOptions);
    ASSERT_NE(cubeMaterial,nullptr);ASSERT_TRUE(cubeMaterial->IsValid());
    ASSERT_NE(cubeMaterial->Get<R::Resources::TextureCube*>("_Environment"),nullptr);
    std::string textureError;
    ASSERT_TRUE(R::Resources::ApplyMaterialParameter(*material,"_Map",std::string("black"),textureError))<<textureError;
    EXPECT_EQ(material->GetTextureResourcePath("_Map"),"black");
    EXPECT_NE(material->Get<R::Resources::Texture2D*>("_Map"),nullptr);
    std::unique_ptr<R::Resources::Material> cubeCopy;
    {
        R::Resources::MaterialInstance dynamicCube(*cubeMaterial);std::string error;
        ASSERT_TRUE(dynamicCube.SetParameter("_Environment",std::string("black"),error))<<error;
        ASSERT_TRUE(dynamicCube.ClearParameter("_Environment",error))<<error;
        EXPECT_EQ(dynamicCube.GetMaterial().GetTextureResourcePath("_Environment"),cubePath.string());
        cubeCopy=dynamicCube.GetMaterial().CreateDynamicCopy();
    }
    R::Resources::Loaders::MaterialLoader::Destroy(cubeMaterial);
    FrozenDraw cubeDraw{cubeCopy->BuildRecordedGraphicsPipeline(device,cache,R::Settings::EPrimitiveMode::TRIANGLES,state,overrides),cubeCopy->GetRecordedBindingSet(device)};
    ASSERT_NE(cubeDraw.pipeline,nullptr);ASSERT_NE(cubeDraw.bindings,nullptr);
    cubeCopy.reset();
    R::Resources::Loaders::MaterialLoader::Destroy(material);
    manager.UnloadResources();
    EXPECT_EQ(resources[1]->layout.reflection->constantBuffers[0].members[0].byteOffset,32);
    std::array<float,42> vertices{};
    for (size_t index=0; index<3; ++index)
    { vertices[index*14] = index==1 ? 3.f : -1.f; vertices[index*14+1] = index==2 ? 3.f : -1.f;
        vertices[index*14+3] = index==1 ? 2.f : 0.f; vertices[index*14+4] = index==2 ? 2.f : 0.f; }
    GPU::RHIBufferDesc vertexDesc; vertexDesc.size=sizeof(vertices); vertexDesc.usage=GPU::BufferUsageFlags::Vertex;
    GPU::RHIBufferUploadDesc vertexUpload; vertexUpload.data=vertices.data(); vertexUpload.dataSize=sizeof(vertices);
    const auto vertexBuffer=device->CreateBuffer(vertexDesc,vertexUpload); ASSERT_NE(vertexBuffer,nullptr);
    const auto pool=device->CreateCommandPool(GPU::QueueType::Graphics);
    for (size_t index=0; index<3; ++index)
    {
        SCOPED_TRACE(index);
        GPU::RHITextureDesc desc; desc.extent={32,32,1}; desc.format=GPU::TextureFormat::RGBA8;
        desc.usage=GPU::TextureUsageFlags::ColorAttachment|GPU::TextureUsageFlags::CopySrc;
        const auto output=device->CreateTexture(desc); ASSERT_NE(output,nullptr);
        GPU::RHITextureViewDesc viewDesc; viewDesc.format=desc.format;
        const auto view=device->CreateTextureView(output,viewDesc);
        const auto commands=pool->CreateCommandBuffer("MaterialParameterValidation"); commands->Begin();
        GPU::RHIRenderPassDesc pass;
        GPU::RHIRenderPassColorAttachmentDesc attachment; attachment.view=view; attachment.loadOp=GPU::LoadOp::Clear; attachment.storeOp=GPU::StoreOp::Store;
        pass.colorAttachments.push_back(attachment); pass.renderArea={0,0,32,32};
        commands->BeginRenderPass(pass);
        commands->SetViewport({0,0,32,32,0,1}); commands->SetScissor({0,0,32,32});
        commands->BindGraphicsPipeline(index==2?cubeDraw.pipeline:draws[index].pipeline);
        commands->BindBindingSet(GPU::BindingPointMap::kMaterialDescriptorSet,index==2?cubeDraw.bindings:draws[index].bindings);
        commands->BindVertexBuffer(0,{vertexBuffer,0,14*sizeof(float)}); commands->Draw(3,1,0,0);
        commands->EndRenderPass(); commands->End();
        const auto fence=device->CreateFence("MaterialValidationComplete"); fence->Reset();
        GPU::RHISubmitDesc submit; submit.commandBuffers={commands}; submit.signalFence=fence;
        ASSERT_TRUE(device->GetQueue(GPU::QueueType::Graphics)->SubmitChecked(submit).Succeeded());
        ASSERT_TRUE(fence->Wait(30000000000ull));
        std::array<uint8_t,32*32*4> pixels{};
        const auto read=device->BeginReadPixels(output,0,0,32,32,R::Settings::EPixelDataFormat::RGBA,
            R::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
        ASSERT_TRUE(read.Succeeded()) << read.message;
        if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        const size_t center=(16*32+16)*4;
        EXPECT_GT(pixels[center+(index==2?1:index)],220) << "RGBA=" << static_cast<int>(pixels[center]) << ',' << static_cast<int>(pixels[center+1])
            << ',' << static_cast<int>(pixels[center+2]) << ',' << static_cast<int>(pixels[center+3]);
        EXPECT_LT(pixels[center+(index==0?1:0)],20);
        if(index==2)EXPECT_GT(pixels[center+2],220);
    }
    // The same retained bindings also survive a depth/stencil prepass followed
    // by a deferred exclusion and equal-depth forward color draw.
    for(bool forwardColor:{false,true})
    {
        GPU::RHITextureDesc outputDesc;outputDesc.extent={32,32,1};outputDesc.format=GPU::TextureFormat::RGBA8;
        outputDesc.usage=GPU::TextureUsageFlags::ColorAttachment|GPU::TextureUsageFlags::CopySrc;
        const auto output=device->CreateTexture(outputDesc);ASSERT_NE(output,nullptr);
        GPU::RHITextureViewDesc outputViewDesc;outputViewDesc.format=outputDesc.format;
        const auto outputView=device->CreateTextureView(output,outputViewDesc);
        GPU::RHITextureDesc depthDesc;depthDesc.extent={32,32,1};depthDesc.format=GPU::TextureFormat::Depth24Stencil8;
        depthDesc.usage=GPU::TextureUsageFlags::DepthStencilAttachment;
        const auto depth=device->CreateTexture(depthDesc);ASSERT_NE(depth,nullptr);
        GPU::RHITextureViewDesc depthViewDesc;depthViewDesc.format=depthDesc.format;
        const auto depthView=device->CreateTextureView(depth,depthViewDesc);
        auto prepass=draws[0].pipeline->GetDesc();prepass.renderTargetLayout.hasDepth=true;prepass.renderTargetLayout.depthFormat=depthDesc.format;
        prepass.renderTargetLayout.colorFormats.clear();prepass.renderTargetLayout.colorSpaces.clear();prepass.blendState.renderTargets.clear();
        prepass.depthStencilState.depthTest=true;prepass.depthStencilState.depthWrite=true;
        prepass.depthStencilState.depthCompare=R::Settings::EComparaisonAlgorithm::LESS_EQUAL;
        prepass.depthStencilState.stencilTest=true;prepass.depthStencilState.stencilReference=0x80;
        prepass.depthStencilState.stencilCompare=R::Settings::EComparaisonAlgorithm::ALWAYS;
        prepass.depthStencilState.stencilWriteMask=0x80;prepass.depthStencilState.stencilReadMask=0x80;
        prepass.depthStencilState.stencilPassOp=R::Settings::EOperation::REPLACE;
        const auto depthPipeline=device->CreateGraphicsPipeline(prepass);ASSERT_NE(depthPipeline,nullptr);
        auto deferred=draws[0].pipeline->GetDesc();deferred.renderTargetLayout.hasDepth=true;deferred.renderTargetLayout.depthFormat=depthDesc.format;
        deferred.depthStencilState.depthTest=false;deferred.depthStencilState.depthWrite=false;
        deferred.depthStencilState.stencilTest=true;deferred.depthStencilState.stencilReference=0;
        deferred.depthStencilState.stencilWriteMask=0;deferred.depthStencilState.stencilReadMask=0x80;
        deferred.depthStencilState.stencilCompare=R::Settings::EComparaisonAlgorithm::EQUAL;
        const auto deferredPipeline=device->CreateGraphicsPipeline(deferred);ASSERT_NE(deferredPipeline,nullptr);
        auto color=draws[1].pipeline->GetDesc();color.renderTargetLayout.hasDepth=true;color.renderTargetLayout.depthFormat=depthDesc.format;
        color.depthStencilState.depthTest=true;color.depthStencilState.depthWrite=false;color.depthStencilState.depthCompare=R::Settings::EComparaisonAlgorithm::EQUAL;
        const auto colorPipeline=device->CreateGraphicsPipeline(color);ASSERT_NE(colorPipeline,nullptr);
        const auto commands=pool->CreateCommandBuffer("MaterialHybridValidation");commands->Begin();
        GPU::RHIRenderPassDesc pass;pass.renderArea={0,0,32,32};
        GPU::RHIRenderPassDepthStencilAttachmentDesc attachment;attachment.view=depthView;
        attachment.depthLoadOp=GPU::LoadOp::Clear;attachment.depthStoreOp=GPU::StoreOp::Store;
        attachment.stencilLoadOp=GPU::LoadOp::Clear;attachment.stencilStoreOp=GPU::StoreOp::Store;pass.depthStencilAttachment=attachment;
        const auto draw=[&](const auto& pipeline,const auto& bindings)
        {commands->SetViewport({0,0,32,32,0,1});commands->SetScissor({0,0,32,32});commands->BindGraphicsPipeline(pipeline);
            commands->BindBindingSet(GPU::BindingPointMap::kMaterialDescriptorSet,bindings);
            commands->BindVertexBuffer(0,{vertexBuffer,0,14*sizeof(float)});commands->Draw(3,1,0,0);};
        commands->BeginRenderPass(pass);draw(depthPipeline,draws[0].bindings);commands->EndRenderPass();
        attachment.depthLoadOp=GPU::LoadOp::Load;attachment.stencilLoadOp=GPU::LoadOp::Load;attachment.readOnlyDepthStencil=true;
        pass.depthStencilAttachment=attachment;
        GPU::RHIRenderPassColorAttachmentDesc target;target.view=outputView;target.clearValue={0,0,1,1};target.storeOp=GPU::StoreOp::Store;
        pass.colorAttachments={target};commands->BeginRenderPass(pass);draw(deferredPipeline,draws[0].bindings);commands->EndRenderPass();
        if(forwardColor){pass.colorAttachments.front().loadOp=GPU::LoadOp::Load;commands->BeginRenderPass(pass);draw(colorPipeline,draws[1].bindings);commands->EndRenderPass();}
        commands->End();const auto fence=device->CreateFence("MaterialHybridComplete");fence->Reset();
        GPU::RHISubmitDesc submit;submit.commandBuffers={commands};submit.signalFence=fence;
        ASSERT_TRUE(device->GetQueue(GPU::QueueType::Graphics)->SubmitChecked(submit).Succeeded());ASSERT_TRUE(fence->Wait(30000000000ull));
        std::array<uint8_t,32*32*4> pixels{};const auto read=device->BeginReadPixels(output,0,0,32,32,R::Settings::EPixelDataFormat::RGBA,
            R::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());ASSERT_TRUE(read.Succeeded())<<read.message;
        if(read.completion)ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        const size_t center=(16*32+16)*4;EXPECT_LT(pixels[center],20);EXPECT_GT(pixels[center+(forwardColor?1:2)],220);
    }
    R::Resources::Loaders::MaterialLoader::Destroy(material);
    manager.UnloadResources();
    R::Resources::Material::ReleaseDefaultTexturesForDevice(device->GetCacheIdentity());
}
}

TEST(MaterialGpuTests, Dx12MaterialArtifactParametersAndVariantSnapshotsReachPixels)
{
#if defined(_WIN32)
    ValidateMaterialGpu(false);
#else
    GTEST_SKIP() << "DX12 is Windows only";
#endif
}
TEST(MaterialGpuTests, VulkanMaterialArtifactParametersAndVariantSnapshotsReachPixels)
{ ValidateMaterialGpu(true); }
