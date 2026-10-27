#include <gtest/gtest.h>
#include <GLFW/glfw3.h>
#include <Animation/Animation.h>
#include <Rendering/Animation/SkinnedMeshRenderProxy.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Resources/Loaders/MaterialLoader.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Assets/MaterialAsset.h>
#include <Rendering/Assets/ShaderArtifact.h>
#include <Rendering/Resources/MaterialRuntimeResource.h>
#include <Rendering/Resources/IndexedObjectDataShaderSupport.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/RHI/Utils/PipelineCache/PipelineCache.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <MaterialCompiler/MaterialShaderCompiler.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Rendering/Settings/DriverSettings.h>
#include <fstream>

namespace
{
void ValidateSkinning(bool vulkan,bool useStockAttributes=false)
{
    namespace R=NLS::Render;namespace G=R::RHI;namespace A=NLS::Animation;
    struct Window{GLFWwindow* handle=nullptr;~Window(){if(handle){glfwDestroyWindow(handle);glfwTerminate();}}}window;
    if(vulkan){ASSERT_TRUE(glfwInit());glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);window.handle=glfwCreateWindow(32,32,"Animation GPU validation",nullptr,nullptr);ASSERT_NE(window.handle,nullptr);}
    R::Settings::DriverSettings settings;settings.graphicsBackend=vulkan?R::Settings::EGraphicsBackend::VULKAN:R::Settings::EGraphicsBackend::DX12;settings.platformWindow=window.handle;settings.debugMode=false;
    R::Context::Driver driver(settings);R::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device=R::Context::DriverRendererAccess::GetExplicitDevice(driver);ASSERT_TRUE(device&&device->IsBackendReady());
    NLS::Core::ServiceLocator::Provide(driver);NLS::Core::ResourceManagement::ShaderManager manager;NLS::Core::ServiceLocator::Provide(manager);
    struct Services{~Services(){NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();NLS::Core::ServiceLocator::Remove<R::Context::Driver>();}}services;
    const auto root=std::filesystem::temp_directory_path()/("NullusAnimationGpu-"+NLS::Guid::New().ToString());std::filesystem::create_directories(root);
    struct Cleanup{std::filesystem::path root;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}}cleanup{root};
    R::ShaderLab::ShaderLabPassDesc pass;pass.vertexEntry="VSMain";pass.fragmentEntry="PSMain";pass.tags.values["LightMode"]="Forward";
    pass.hlslSource=R"(
#include "NullusShaderLibrary/Skinning.hlsl"
struct Attributes {float3 positionOS:POSITION;float2 uv:TEXCOORD0;float3 normalOS:NORMAL;float3 tangentOS:TEXCOORD1;float3 bitangentOS:TEXCOORD2;NLS_SKIN_ATTRIBUTES};
struct Varyings {float4 position:SV_POSITION;float3 normal:TEXCOORD0;};
Varyings VSMain(Attributes input){Varyings output;output.position=float4(NLS_SKIN_POSITION(input),1);output.normal=NLS_SKIN_NORMAL(input);return output;}
float4 PSMain(Varyings input):SV_Target0{return float4(abs(normalize(input.normal)),1);}
)";
    if(useStockAttributes)
    {
        std::ifstream stockFile(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader");
        const std::string stock{std::istreambuf_iterator<char>(stockFile),std::istreambuf_iterator<char>()};
        const auto stockBegin=stock.find("struct Attributes");ASSERT_NE(stockBegin,std::string::npos);
        const auto stockEnd=stock.find("};",stockBegin);ASSERT_NE(stockEnd,std::string::npos);
        const auto begin=pass.hlslSource.find("struct Attributes");const auto end=pass.hlslSource.find("};",begin);
        pass.hlslSource.replace(begin,end+2-begin,stock.substr(stockBegin,stockEnd+2-stockBegin));
    }
    NLS::MaterialCompiler::MaterialShaderCompileOptions options;options.sourcePath=root/"Skin.shader";options.generatedPath=root/"Skin.hlsl";options.sourceIdentity="Assets/Skin.shader";options.subAssetKey="shader:Skin";
    options.targets={vulkan?R::ShaderCompiler::ShaderTargetPlatform::SPIRV:R::ShaderCompiler::ShaderTargetPlatform::DXIL};options.generateGlsl=false;
    options.includeDirectories={(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders").string()};
    auto artifact=NLS::MaterialCompiler::CompileMaterialPass(pass,options);for(auto& stage:artifact.stages)ASSERT_EQ(stage.output.status,R::ShaderCompiler::ShaderCompilationStatus::Succeeded)<<stage.output.diagnostics;
    ASSERT_EQ(artifact.variantReflections.size(),2u);const auto shaderPath=root/"Library/Artifacts"/std::string(64,'a');std::filesystem::create_directories(shaderPath.parent_path());
    {const auto bytes=R::Assets::SerializeShaderArtifact(artifact);std::ofstream file(shaderPath,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    R::Assets::MaterialAsset source;source.shader.path=artifact.sourcePath;R::Resources::Loaders::MaterialLoader::LoadOptions load;load.loadMissingTextures=false;load.shaderPassArtifactPaths={shaderPath.string()};
    std::unique_ptr<R::Resources::Material> material(R::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload("Assets/Skin.mat",R::Assets::SerializeMaterialAsset(source),load));ASSERT_TRUE(material);
    // GBuffer skinning also uses indexed object matrices and the shared
    // push-constant ABI. Exercise the compiled artifact through Material's
    // actual pipeline-layout validation on both backends.
    auto indexedPass=pass;indexedPass.hlslSource=R"(
#if defined(NLS_SPIRV)
struct NLSObjectDrawConstants {uint objectIndex;uint objectFlags;uint objectPadding0;uint objectPadding1;};
[[vk::push_constant]] ConstantBuffer<NLSObjectDrawConstants> g_NLSObjectDrawConstants;
#define u_ObjectIndex g_NLSObjectDrawConstants.objectIndex
#else
cbuffer ObjectIndexConstants:register(b1,space3){uint u_ObjectIndex;uint u_ObjectFlags;uint u_ObjectPadding0;uint u_ObjectPadding1;};
#endif
StructuredBuffer<float4x4> ObjectData:register(t0,space3);
StructuredBuffer<uint> InstanceObjectIndices:register(t1,space3);
)"+indexedPass.hlslSource;
    const std::string original="output.position=float4(NLS_SKIN_POSITION(input),1)";
    const auto indexedPosition=indexedPass.hlslSource.find(original);ASSERT_NE(indexedPosition,std::string::npos);
    indexedPass.hlslSource.replace(indexedPosition,original.size(),"output.position=mul(ObjectData[InstanceObjectIndices[u_ObjectIndex]],float4(NLS_SKIN_POSITION(input),1))");
    auto indexedOptions=options;indexedOptions.generatedPath=root/"IndexedSkin.hlsl";indexedOptions.subAssetKey="shader:IndexedSkin";
    auto indexedArtifact=NLS::MaterialCompiler::CompileMaterialPass(indexedPass,indexedOptions);
    for(const auto& stage:indexedArtifact.stages)ASSERT_EQ(stage.output.status,R::ShaderCompiler::ShaderCompilationStatus::Succeeded)<<stage.output.diagnostics;
    const auto indexedPath=root/"Library/Artifacts"/std::string(64,'b');
    {const auto bytes=R::Assets::SerializeShaderArtifact(indexedArtifact);std::ofstream file(indexedPath,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    auto indexedLoad=load;indexedLoad.shaderPassArtifactPaths={indexedPath.string()};
    std::unique_ptr<R::Resources::Material> indexedMaterial(R::Resources::Loaders::MaterialLoader::CreateFromSerializedPayload("Assets/IndexedSkin.mat",R::Assets::SerializeMaterialAsset(source),indexedLoad));ASSERT_TRUE(indexedMaterial);ASSERT_NE(indexedMaterial->GetShader(),nullptr);
    EXPECT_TRUE(R::Resources::ValidateIndexedObjectDataShader(*indexedMaterial->GetShader()).IsCompatible());
    EXPECT_TRUE(indexedMaterial->GetExplicitPipelineLayout(device));
    if(useStockAttributes)
    {
        const auto engineAssets=std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine";
        R::Resources::Loaders::ShaderLoader::SetTrustedBuiltInShaderEngineAssetsPath(engineAssets.string());
        auto* lighting=R::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl((engineAssets/"Shaders/DeferredLighting.hlsl").string(),(root/"Assets").string());
        ASSERT_NE(lighting,nullptr);
        {R::Resources::Material lightingMaterial;lightingMaterial.SetShader(lighting);EXPECT_FALSE(lighting->GetReflectionSnapshot()->constantBuffers.empty());EXPECT_TRUE(lightingMaterial.GetExplicitPipelineLayout(device));}
        R::Resources::Loaders::ShaderLoader::Destroy(lighting);
    }
    auto model=std::make_shared<A::Model>();model->skeleton.id="test";model->skeleton.bones={{"root","root",A::NoBone,{}},{"child","child",0,{}}};
    A::SkinBinding skin;skin.id="skin";skin.skeleton="test";skin.joints={0,1};skin.inverseBind={NLS::Maths::Matrix4::Identity,NLS::Maths::Matrix4::Identity};model->skins={skin};
    A::SkeletalMesh triangle;triangle.id="triangle";triangle.skin="skin";triangle.vertices.resize(3);triangle.indices={0,1,2};
    for(size_t i=0;i<3;++i){auto& v=triangle.vertices[i];v.position[0]=i==1?3.f:-1.f;v.position[1]=i==2?3.f:-1.f;v.position[2]=.5f;v.normals[0]=v.normals[1]=.70710678f;v.influence.joints[0]=1;v.influence.weights[0]=65535;}
    model->meshes={triangle};std::string error;auto proxy=std::make_unique<R::AnimationRendering::SkinnedMeshRenderProxy>();ASSERT_TRUE(proxy->Initialize(device,model,0,error))<<error;
    struct Draw{std::shared_ptr<R::Resources::Mesh> mesh;std::shared_ptr<G::RHIGraphicsPipeline> pipeline;std::shared_ptr<G::RHIBindingSet> binding,objects;};std::array<Draw,2> draws;
    const auto cache=G::CreateDefaultPipelineCache();R::Data::PipelineState state;state.culling=false;state.depthTest=false;state.depthWriting=false;
    R::Resources::MaterialPipelineStateOverrides overrides;overrides.hasDepthAttachment=false;overrides.culling=false;
    for(size_t i=0;i<2;++i)
    {
        auto pose=std::make_shared<A::AnimationFrameResult>();pose->revision=i+1;pose->model={NLS::Maths::Matrix4::Translation({1000,0,0}),i?NLS::Maths::Matrix4::Translation({1.5f,0,0})*NLS::Maths::Matrix4::Scaling({2,1,1}):NLS::Maths::Matrix4::Identity};
        draws[i].mesh=proxy->Publish(pose,error);ASSERT_TRUE(draws[i].mesh)<<error;auto dynamic=draws[i].mesh->GetSkinningFrame()->ResolveMaterial(useStockAttributes?*indexedMaterial:*material);
        draws[i].pipeline=dynamic->BuildRecordedGraphicsPipeline(device,cache,R::Settings::EPrimitiveMode::TRIANGLES,state,overrides);draws[i].binding=dynamic->GetRecordedBindingSet(device);
        ASSERT_TRUE(draws[i].pipeline);ASSERT_TRUE(draws[i].binding);EXPECT_FALSE(dynamic->HasExplicitBindingErrors());
        if(useStockAttributes)
        {
            G::RHIBufferDesc bufferDesc;bufferDesc.size=64;bufferDesc.usage=G::BufferUsageFlags::ShaderRead;bufferDesc.memoryUsage=G::MemoryUsage::CPUToGPU;
            G::RHIBufferUploadDesc upload;upload.data=NLS::Maths::Matrix4::Identity.data;upload.dataSize=64;auto matrix=device->CreateBuffer(bufferDesc,upload);ASSERT_TRUE(matrix);
            std::shared_ptr<G::RHIBindingLayout> objectLayout;
            for(const auto& layout:dynamic->GetExplicitPipelineLayout(device)->GetDesc().bindingLayouts)if(layout&&std::any_of(layout->GetDesc().entries.begin(),layout->GetDesc().entries.end(),[](const auto& e){return e.name=="ObjectData";}))objectLayout=layout;
            ASSERT_TRUE(objectLayout);G::RHIBindingSetDesc objects;objects.layout=objectLayout;G::RHIBindingSetEntry entry;entry.type=G::BindingType::StructuredBuffer;entry.binding=0;entry.buffer=matrix;entry.bufferRange=64;entry.elementStride=64;objects.entries={entry};
            const uint32_t objectIndex=0;bufferDesc.size=4;upload.data=&objectIndex;upload.dataSize=4;entry.binding=1;entry.buffer=device->CreateBuffer(bufferDesc,upload);entry.bufferRange=4;entry.elementStride=4;objects.entries.push_back(entry);
            draws[i].objects=device->CreateBindingSet(objects);ASSERT_TRUE(draws[i].objects);
        }
    }
    proxy.reset();material.reset();model.reset(); // Recorded draws must retain every resource they use.
    const auto pool=device->CreateCommandPool(G::QueueType::Graphics);
    for(size_t i=0;i<2;++i)
    {
        G::RHITextureDesc desc;desc.extent={32,32,1};desc.format=G::TextureFormat::RGBA8;desc.usage=G::TextureUsageFlags::ColorAttachment|G::TextureUsageFlags::CopySrc;
        auto output=device->CreateTexture(desc);ASSERT_TRUE(output);G::RHITextureViewDesc viewDesc;viewDesc.format=desc.format;auto view=device->CreateTextureView(output,viewDesc);
        auto commands=pool->CreateCommandBuffer("AnimationVertexSkinning");commands->Begin();G::RHIRenderPassDesc render;render.renderArea={0,0,32,32};G::RHIRenderPassColorAttachmentDesc color;color.view=view;color.loadOp=G::LoadOp::Clear;color.storeOp=G::StoreOp::Store;render.colorAttachments.push_back(color);
        commands->BeginRenderPass(render);commands->SetViewport({0,0,32,32,0,1});commands->SetScissor({0,0,32,32});commands->BindGraphicsPipeline(draws[i].pipeline);commands->BindBindingSet(G::BindingPointMap::kMaterialDescriptorSet,draws[i].binding);
        if(draws[i].objects){const uint32_t constants[4]={0,0,0,0};commands->BindBindingSet(G::BindingPointMap::kObjectDescriptorSet,draws[i].objects);commands->PushConstants(G::ShaderStageMask::Vertex|G::ShaderStageMask::Fragment,0,16,constants);}
        const auto mesh=draws[i].mesh->GetRHIMesh();commands->BindVertexBuffer(0,{mesh->GetVertexBuffer(),0,mesh->GetVertexStride()});commands->BindIndexBuffer({mesh->GetIndexBuffer(),0,G::IndexType::UInt32});commands->DrawIndexed(3,1,0,0,0);commands->EndRenderPass();commands->End();
        auto fence=device->CreateFence("AnimationGpuComplete");fence->Reset();G::RHISubmitDesc submit;submit.commandBuffers={commands};submit.signalFence=fence;ASSERT_TRUE(device->GetQueue(G::QueueType::Graphics)->SubmitChecked(submit).Succeeded());ASSERT_TRUE(fence->Wait(30000000000ull));
        std::array<uint8_t,4096> pixels{};auto read=device->BeginReadPixels(output,0,0,32,32,R::Settings::EPixelDataFormat::RGBA,R::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());ASSERT_TRUE(read.Succeeded())<<read.message;if(read.completion)ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        const size_t center=(16*32+16)*4,left=(16*32+2)*4;
        EXPECT_NEAR(pixels[center],i?114:180,5);EXPECT_NEAR(pixels[center+1],i?228:180,5);if(i)EXPECT_LT(pixels[left],5);else EXPECT_GT(pixels[left],170);
    }
}
TEST(AnimationGpu,DX12VertexWeightsPaletteAndNormalReachPixels){ValidateSkinning(false);}
TEST(AnimationGpu,VulkanVertexWeightsPaletteAndNormalReachPixels){ValidateSkinning(true);}
TEST(AnimationGpu,DX12StockShaderAttributeOrderReachesPixels){ValidateSkinning(false,true);}
TEST(AnimationGpu,VulkanStockShaderAttributeOrderReachesPixels){ValidateSkinning(true,true);}
}
