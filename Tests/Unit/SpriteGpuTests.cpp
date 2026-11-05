#include <gtest/gtest.h>
#include <Engine/Rendering/SpriteSceneRenderer.h>
#include <Engine/Components/SpriteRendererComponent.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>
#include <Engine/Serialize/ObjectGraphInstantiator.h>
#include <Engine/Serialize/ObjectGraphReader.h>
#include <Engine/Serialize/ObjectGraphSerializer.h>
#include <Scripting/ProjectPhysicsTiming.h>
#include <Rendering/Context/TransparentSubmission.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/Context/DriverInternal.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/Entities/Camera.h>
#include <Rendering/Tooling/MaterialVisualEvidence.h>
#include <Guid.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

namespace
{
namespace R=NLS::Render; namespace C=R::Context; namespace E=NLS::Engine; namespace H=R::RHI;
TEST(SpriteRuntime, EditorSelectsSpriteSiblingsWithTheSameHostPlatformAsTextures)
{
    using Type=NLS::Core::Assets::ArtifactType;
    for(const auto* platform:{"editor","win64-dx12","win64","linux-vulkan","macos-metal"})
        EXPECT_EQ(E::Assets::IsRuntimeAssetPlatformCompatible(Type::Sprite,platform,"editor"),
                  E::Assets::IsRuntimeAssetPlatformCompatible(Type::Texture,platform,"editor"))<<platform;
    EXPECT_FALSE(E::Assets::IsRuntimeAssetPlatformCompatible(Type::Sprite,"win64-dx12","linux-vulkan"));
}
TEST(SpriteRuntime, GpuProjectionUsesNativeDepthWithoutChangingCpuCameraMatrices)
{
    R::Entities::Camera camera;
    for(const auto mode:{R::Settings::EProjectionMode::ORTHOGRAPHIC,R::Settings::EProjectionMode::PERSPECTIVE})
    {
        camera.SetProjectionMode(mode);camera.SetNear(0.1f);camera.SetFar(100);camera.CacheMatrices(640,360);
        const auto cpu=camera.GetProjectionMatrix();const auto gpu=camera.GetGpuProjectionMatrix();
        const auto near=gpu*NLS::Maths::Vector4(0,0,-camera.GetNear(),1);
        const auto far=gpu*NLS::Maths::Vector4(0,0,-camera.GetFar(),1);
        EXPECT_NEAR(near.z/near.w,0.f,1e-6f);EXPECT_NEAR(far.z/far.w,1.f,1e-6f);
        EXPECT_TRUE(NLS::Maths::Matrix4::AreEquals(cpu,camera.GetProjectionMatrix()));
    }
}
TEST(SpriteRuntime, ProjectTimingSelectsSixtyHzAndKeepsTheLastValidClockOnBadSettings)
{
    const auto path=std::filesystem::path(NLS_BUILD_DIR)/("sprite-timing-"+NLS::Guid::New().ToString()+".ini");
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);}} cleanup{path};
    {std::ofstream output(path);output<<"fixed_delta_time=0.016666667\nmaximum_fixed_steps=8\n";}
    NLS::Filesystem::IniFile project(path.string());NLS::Scripting::RuntimePlaySession session;std::string error;
    ASSERT_TRUE(NLS::Scripting::ConfigureProjectPhysicsTiming(session,project,error))<<error;
    EXPECT_NEAR(session.GetPhysicsClock().GetSettings().fixedDeltaTime,1.0/60,1e-7);
    EXPECT_EQ(session.GetPhysicsClock().GetSettings().maxSubsteps,8);
    project.Set<int>("maximum_fixed_steps",0);
    EXPECT_FALSE(NLS::Scripting::ConfigureProjectPhysicsTiming(session,project,error));EXPECT_FALSE(error.empty());
    EXPECT_EQ(session.GetPhysicsClock().GetSettings().maxSubsteps,8);
}
TEST(SpriteRuntime, TemplateSceneRoundTripsTheReflectedComponentAndPersistentReference)
{
    namespace S=E::Serialize;
    std::ifstream input(std::filesystem::path(NLS_ROOT_DIR)/"Templates/TwoD/content/Assets/Scenes/Main.scene");
    ASSERT_TRUE(input);const std::string text(std::istreambuf_iterator<char>(input),{});
    const auto document=S::ObjectGraphReader::Read(text);ASSERT_TRUE(document);
    S::LoadPolicy policy;policy.deferAssetReferenceResolution=true;policy.invalidReferencePolicy=S::InvalidReferencePolicy::Preserve;
    auto instantiated=S::ObjectGraphInstantiator::InstantiateScene(*document,policy);
    auto& scene=instantiated.scene;ASSERT_TRUE(scene);
    ASSERT_EQ(scene->GetFastAccessComponents().sprites.size(),1);
    auto* sprite=scene->GetFastAccessComponents().sprites[0];ASSERT_TRUE(sprite);
    S::ObjectIdentifier reference;
    ASSERT_TRUE(S::PersistentManager::Instance().InstanceIDToObjectIdentifier(sprite->GetSpriteReference().GetInstanceID(),reference));
    const auto meta=NLS::Core::Assets::AssetMeta::Load(std::filesystem::path(NLS_ROOT_DIR)/"Templates/TwoD/content/Assets/Textures/Hero.png.meta");
    ASSERT_TRUE(meta);EXPECT_EQ(reference.guid,meta->id.GetGuid());
    const auto expected=S::MakeLocalIdentifierInFile(meta->id.GetGuid(),"sprite:"+meta->settings.at("SPRITE_ID"));
    EXPECT_EQ(reference.localIdentifierInFile,expected);
    sprite->SetColor({0.5f,0.75f,1,0.5f});sprite->SetFlipX(true);sprite->SetOrderInLayer(-42);
    EXPECT_EQ(sprite->GetDrawMode(),E::Components::SpriteRendererComponent::ESpriteDrawMode::Simple);
    sprite->SetDrawMode(E::Components::SpriteRendererComponent::ESpriteDrawMode::Sliced);sprite->SetSize({3,2});
    auto restored=S::ObjectGraphInstantiator::InstantiateScene(S::ObjectGraphSerializer::SerializeScene(*scene));ASSERT_TRUE(restored);
    ASSERT_EQ(restored->GetFastAccessComponents().sprites.size(),1);auto* copy=restored->GetFastAccessComponents().sprites[0];
    EXPECT_TRUE(copy->GetFlipX());EXPECT_EQ(copy->GetOrderInLayer(),-42);EXPECT_FLOAT_EQ(copy->GetColor().w,0.5f);
    EXPECT_EQ(copy->GetDrawMode(),E::Components::SpriteRendererComponent::ESpriteDrawMode::Sliced);
    EXPECT_FLOAT_EQ(copy->GetSize().x,3);EXPECT_FLOAT_EQ(copy->GetSize().y,2);
    const auto revision=copy->GetRenderRevision();copy->SetSize({0,-1});
    EXPECT_EQ(copy->GetRenderRevision(),revision);EXPECT_FLOAT_EQ(copy->GetSize().x,3);
    copy->SetDrawMode(E::Components::SpriteRendererComponent::ESpriteDrawMode::Tiled);
    EXPECT_EQ(copy->GetDrawModeValue(),2);copy->SetDrawModeValue(99);EXPECT_EQ(copy->GetDrawModeValue(),2);
    auto tiled=S::ObjectGraphInstantiator::InstantiateScene(S::ObjectGraphSerializer::SerializeScene(*restored));ASSERT_TRUE(tiled);
    EXPECT_EQ(tiled->GetFastAccessComponents().sprites[0]->GetDrawMode(),E::Components::SpriteRendererComponent::ESpriteDrawMode::Tiled);
}
TEST(SpriteSubmission, MergesMeshSpriteVfxAndRetainsResourceEdgesAndOwners)
{
    C::RenderScenePackage package;
    C::RenderPassCommandInput opaque; opaque.kind=C::RenderPassCommandKind::Opaque;
    package.passCommandInputs.push_back(opaque);
    const auto owner=std::make_shared<int>(42);
    for(float depth : {2.f,4.f,3.f})
    {
        C::RenderPassCommandInput pass; pass.kind=C::RenderPassCommandKind::Transparent; pass.drawCount=1;
        C::RecordedDrawCommandInput draw; draw.transparentSortKey=R::Sprites::SpriteSortKey{0,0,depth,uint64_t(depth)};
        draw.resourceOwner=owner; pass.recordedDrawCommands.push_back(draw); package.passCommandInputs.push_back(pass);
    }
    C::RenderPassCommandInput ui; ui.kind=C::RenderPassCommandKind::UIOverlay; package.passCommandInputs.push_back(ui);
    for(size_t i=0;i<package.passCommandInputs.size();++i)
    {
        auto units=C::BuildRecordedDrawCommandWorkUnitsForPass(package.passCommandInputs[i],i,i);
        package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.end(),units.begin(),units.end());
        if(i) {C::WorkUnitDependencyEdge edge;edge.sourceWorkUnitIndex=i-1;edge.targetWorkUnitIndex=i;package.workUnitDependencyEdges.push_back(edge);}
    }
    C::FinalizeTransparentSubmission(package);
    ASSERT_EQ(package.passCommandInputs.size(),3);
    const auto& draws=package.passCommandInputs[1].recordedDrawCommands;
    ASSERT_EQ(draws.size(),3); EXPECT_EQ(draws[0].transparentSortKey->depth,4);
    EXPECT_EQ(draws[1].transparentSortKey->depth,3); EXPECT_EQ(draws[2].transparentSortKey->depth,2);
    EXPECT_EQ(draws[1].resourceOwner,owner);
    ASSERT_EQ(package.parallelCommandWorkUnits.size(),3);
    ASSERT_EQ(package.workUnitDependencyEdges.size(),2);
    EXPECT_EQ(package.workUnitDependencyEdges[1].sourceWorkUnitIndex,1);
    EXPECT_EQ(package.workUnitDependencyEdges[1].targetWorkUnitIndex,2);
}
TEST(SpriteSubmission, RetainsDistinctMipDependenciesWhileRemovingExactDuplicates)
{
    C::RenderScenePackage package;
    for(size_t p=0;p<3;++p)
    {
        C::RenderPassCommandInput pass;pass.kind=p?C::RenderPassCommandKind::Transparent:C::RenderPassCommandKind::Opaque;
        if(p){C::RecordedDrawCommandInput draw;draw.transparentSortKey=R::Sprites::SpriteSortKey{0,0,float(p),p};pass.recordedDrawCommands.push_back(draw);pass.drawCount=1;}
        package.passCommandInputs.push_back(pass);
        auto units=C::BuildRecordedDrawCommandWorkUnitsForPass(pass,p,p);
        package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.end(),units.begin(),units.end());
    }
    for(uint32_t mip=0;mip<2;++mip)
    {
        C::WorkUnitDependencyEdge edge;edge.sourceWorkUnitIndex=0;edge.targetWorkUnitIndex=2;edge.resourceKind=C::ThreadedDependencyResourceKind::Texture;
        C::TextureResourceAccess write;write.mode=C::ResourceAccessMode::Write;write.state=H::ResourceState::RenderTarget;write.subresourceRange.baseMipLevel=mip;
        edge.sourceTextureAccess=write;auto read=write;read.mode=C::ResourceAccessMode::Read;read.state=H::ResourceState::ShaderRead;edge.targetTextureAccess=read;
        package.workUnitDependencyEdges.push_back(edge);package.parallelCommandWorkUnits[2].incomingDependencyEdges.push_back(edge);
    }
    C::FinalizeTransparentSubmission(package);
    ASSERT_EQ(package.workUnitDependencyEdges.size(),2);
    EXPECT_EQ(package.workUnitDependencyEdges[0].sourceTextureAccess->subresourceRange.baseMipLevel,0);
    EXPECT_EQ(package.workUnitDependencyEdges[1].sourceTextureAccess->subresourceRange.baseMipLevel,1);
    EXPECT_EQ(package.workUnitDependencyEdges[1].targetWorkUnitIndex,1);
}

TEST(SpriteGpu, Dx12InstanceAndCpuBatchRenderSameGeometryAlphaPivotAndFlip)
{
#if !defined(_WIN32)
    GTEST_SKIP()<<"Windows DX12 acceptance";
#else
    auto& executor=C::RhiCommandListExecutor::Get();
    ASSERT_FALSE(executor.IsInitialized()); executor.Initialize(C::RhiThreadMode::None,{});
    struct Shutdown {C::RhiCommandListExecutor& value;~Shutdown(){value.Shutdown();}} shutdown{executor};
    R::Settings::DriverSettings settings;settings.graphicsBackend=R::Settings::EGraphicsBackend::DX12;settings.debugMode=true;
    auto device=R::Backend::CreateRhiDevice(settings);ASSERT_TRUE(device&&device->IsBackendReady());
    executor.SetTargets(C::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto root=std::filesystem::path(NLS_BUILD_DIR)/"sprite-gpu-artifacts";
    auto program=std::make_shared<E::Rendering::SpriteShaderSnapshot>(); program->identity=1;
    R::ShaderCompiler::ShaderCompiler compiler;
    for(bool pixel : {false,true})
    {
        R::ShaderCompiler::ShaderCompilationInput input;
        input.assetPath=(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders/NullusShaderLibrary/SpriteUnlit.hlsl").string();
        input.stage=pixel?R::ShaderCompiler::ShaderStage::Pixel:R::ShaderCompiler::ShaderStage::Vertex;
        input.options.targetPlatform=R::ShaderCompiler::ShaderTargetPlatform::DXIL;
        input.options.entryPoint=pixel?"SpritePS":"SpriteVS";input.options.targetProfile=pixel?"ps_6_0":"vs_6_0";
        input.options.artifactDirectory=root.string();
        const auto binary=compiler.Compile(input);ASSERT_EQ(binary.status,R::ShaderCompiler::ShaderCompilationStatus::Succeeded)<<binary.diagnostics;
        H::RHIShaderModuleDesc module;module.stage=pixel?H::ShaderStage::Fragment:H::ShaderStage::Vertex;
        module.targetBackend=H::NativeBackendType::DX12;module.entryPoint=input.options.entryPoint;module.bytecode=binary.bytecode;program->shaders.push_back(module);
    }
    program->materialLayout.entries.push_back({"MaterialProperties",H::BindingType::UniformBuffer,H::BindingPointMap::kMaterialDescriptorSet,0,1,H::ShaderStageMask::Fragment,H::BindingPointMap::kMaterialBindingSpace,0});
    E::Rendering::SpriteShaderSnapshot::Constant constant; constant.bytes.resize(16);
    const std::array<float,4> white{1,1,1,1};std::memcpy(constant.bytes.data(),white.data(),16);program->constants.push_back(constant);
    const std::array<uint8_t,16> texels{255,0,0,255,0,255,0,255,0,0,255,255,255,255,255,0};
    H::RHITextureDesc textureDesc;textureDesc.extent={2,2,1};textureDesc.format=H::TextureFormat::RGBA8;
    H::RHITextureUploadDesc upload;upload.data=texels.data();upload.dataSize=16;upload.rowPitch=8;upload.slicePitch=16;upload.extent=textureDesc.extent;
    auto texture=device->CreateTexture(textureDesc,upload);ASSERT_TRUE(texture);
    H::RHITextureViewDesc textureView;textureView.format=textureDesc.format;
    auto sprite=std::make_shared<NLS::Core::Assets::SpriteArtifact>();sprite->sourceWidth=sprite->width=sprite->sourceHeight=sprite->height=2;sprite->pixelsPerUnit=2;
    sprite->id=NLS::Guid::New().ToString();sprite->textureAssetId=NLS::Core::Assets::AssetId::New();sprite->textureSubAssetKey="texture:main";
    auto proxy=std::make_shared<E::Rendering::SpriteRenderProxy>();proxy->sprite=sprite;proxy->program=program;
    proxy->texture=device->CreateTextureView(texture,textureView);ASSERT_TRUE(proxy->texture);
    proxy->sampler.minFilter=proxy->sampler.magFilter=H::TextureFilter::Nearest;
    NLS::Maths::Transform cameraTransform;
    R::Entities::Camera camera(&cameraTransform);camera.SetProjectionMode(R::Settings::EProjectionMode::ORTHOGRAPHIC);camera.SetSize(1);
    camera.SetPosition({0,0,2});camera.SetRotation({0,1,0,0});camera.CacheMatrices(64,64);
    R::Data::FrameDescriptor frame;frame.camera=&camera;frame.renderWidth=frame.renderHeight=64;
    const auto frozen=R::FrameGraph::FreezeFrameDescriptor(frame);
    E::Rendering::SpriteSceneRenderer renderer;
    E::Rendering::SpriteSceneRenderer helperRenderer;
    std::vector<std::shared_ptr<const E::Rendering::SpriteRenderProxy>> drawSprites{proxy};
    bool retainTarget=false, includeHelper=false, clearTarget=true;
    uint32_t lastInstances=0,lastVertices=0;
    std::shared_ptr<H::RHITexture> retainedOutput;
    std::shared_ptr<H::RHITextureView> retainedView;
    const auto render=[&](bool cpu,std::optional<float> depthClear={})
    {
        std::vector<uint8_t> pixels;
        H::RHITextureDesc outputDesc;outputDesc.extent={64,64,1};outputDesc.format=H::TextureFormat::RGBA8;
        outputDesc.usage=H::TextureUsageFlags::ColorAttachment|H::TextureUsageFlags::CopySrc;
        const bool newTarget=!retainTarget || !retainedOutput;
        auto output=newTarget?device->CreateTexture(outputDesc):retainedOutput;
        auto view=newTarget?device->CreateTextureView(output,textureView):retainedView;
        if(retainTarget){retainedOutput=output;retainedView=view;}
        C::RenderScenePackage package;package.renderWidth=package.renderHeight=64;package.externalSceneOutputColorView=view;package.clearColorValue={0,0,0,1};
        package.clearColorBuffer=package.clearDepthBuffer=package.clearStencilBuffer=clearTarget;
        if(depthClear)
        {
            auto depthDesc=outputDesc;depthDesc.format=H::TextureFormat::Depth32F;depthDesc.usage=H::TextureUsageFlags::DepthStencilAttachment;
            auto depth=device->CreateTexture(depthDesc);H::RHITextureViewDesc depthView;depthView.format=depthDesc.format;
            package.externalSceneOutputDepthView=device->CreateTextureView(depth,depthView);
        }
        std::string error;
        if(includeHelper)
        {
            // A real partial draw reproduces grid/gizmo output preceding the
            // Sprite without initializing the retained scene framebuffer.
            auto marker=std::make_shared<E::Rendering::SpriteRenderProxy>(*proxy);
            marker->picking=false;marker->color={1,1,1,1};
            marker->world=NLS::Maths::Matrix4::Translation({-.85f,-.85f,0})*NLS::Maths::Matrix4::Scaling({.125f,.125f,1});
            C::RenderScenePackage helperPackage;helperPackage.renderWidth=helperPackage.renderHeight=64;
            helperPackage.externalSceneOutputColorView=view;
            helperPackage.clearColorBuffer=helperPackage.clearDepthBuffer=helperPackage.clearStencilBuffer=false;
            EXPECT_TRUE(helperRenderer.AppendPass(device,helperPackage,frozen,{marker},error))<<error;
            if(helperPackage.passCommandInputs.empty())return pixels;
            auto helper=helperPackage.passCommandInputs.back();helper.kind=C::RenderPassCommandKind::Helper;
            helper.debugName="PartialEditorHelper";helper.clearColor=helper.clearDepth=helper.clearStencil=false;
            package.passCommandInputs.push_back(std::move(helper));
        }
        const bool built=renderer.AppendPass(device,package,frozen,drawSprites,error,cpu);EXPECT_TRUE(built)<<error;if(!built||package.passCommandInputs.empty())return pixels;
        auto pool=device->CreateCommandPool(H::QueueType::Graphics,"SpriteAcceptance");auto commands=pool->CreateCommandBuffer("SpriteAcceptance");commands->Begin();
        if(newTarget)
        {
            H::RHIRenderPassDesc initial;initial.renderArea={0,0,64,64};
            H::RHIRenderPassColorAttachmentDesc color;color.view=view;color.clearValue={0,0,0,1};initial.colorAttachments={color};
            commands->BeginRenderPass(initial);commands->EndRenderPass();
        }
        for(const auto& input:package.passCommandInputs)
        {
            H::RHIRenderPassDesc pass;pass.renderArea={0,0,64,64};
            if(input.usesColorAttachment)
            {
                H::RHIRenderPassColorAttachmentDesc color;color.view=view;color.clearValue={0,0,0,1};
                color.loadOp=input.clearColor?H::LoadOp::Clear:H::LoadOp::Load;pass.colorAttachments={color};
            }
            if(input.usesDepthStencilAttachment && package.externalSceneOutputDepthView)
            {
                H::RHIRenderPassDepthStencilAttachmentDesc depth;depth.view=package.externalSceneOutputDepthView;
                depth.clearValue.depth=depthClear.value_or(1.f);depth.depthLoadOp=input.clearDepth?H::LoadOp::Clear:H::LoadOp::Load;
                pass.depthStencilAttachment=depth;
            }
            commands->BeginRenderPass(pass);commands->SetViewport({0,0,64,64,0,1});commands->SetScissor({0,0,64,64});
            for(const auto& draw:input.recordedDrawCommands)
            {EXPECT_FALSE(draw.pipeline->GetDesc().depthStencilState.depthWrite);lastInstances=draw.instanceCount;lastVertices=draw.vertexCount;}
            EXPECT_EQ(C::Detail::RecordPreparedDrawCommandsForPass(commands.get(),input),input.drawCount);
            commands->EndRenderPass();
        }
        commands->End();auto fence=device->CreateFence("SpriteAcceptance");fence->Reset();
        H::RHISubmitDesc submit;submit.commandBuffers={commands};submit.signalFence=fence;const auto result=device->GetQueue(H::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_TRUE(result.Succeeded())<<result.message;EXPECT_TRUE(fence->Wait(30000000000ull));
        pixels.resize(64*64*4);const auto read=device->BeginReadPixels(output,0,0,64,64,R::Settings::EPixelDataFormat::RGBA,R::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
        EXPECT_TRUE(read.Succeeded())<<read.message;if(read.completion)EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded());return pixels;
    };
    const auto instanced=render(false),expanded=render(true);ASSERT_EQ(instanced.size(),64*64*4);EXPECT_EQ(instanced,expanded);
    EXPECT_EQ(renderer.GetStatistics().drawCalls,1);EXPECT_TRUE(renderer.GetStatistics().cpuExpanded);
    const auto pixel=[&](const auto& pixels,int x,int y){return std::array<uint8_t,4>{pixels[(y*64+x)*4],pixels[(y*64+x)*4+1],pixels[(y*64+x)*4+2],pixels[(y*64+x)*4+3]};};
    // A one-unit Sprite occupies the central half of this two-unit ortho view.
    EXPECT_EQ(pixel(instanced,24,24),(std::array<uint8_t,4>{255,0,0,255}));
    EXPECT_EQ(pixel(instanced,40,40),(std::array<uint8_t,4>{0,0,0,255}));
    EXPECT_EQ(pixel(instanced,8,8),(std::array<uint8_t,4>{0,0,0,255}));
    proxy->color.w=0.5f;const auto translucent=render(false);
    ASSERT_EQ(translucent.size(),instanced.size());
    EXPECT_NEAR(pixel(translucent,24,24)[0],128,1);
    EXPECT_EQ(pixel(translucent,24,24)[1],0);
    EXPECT_EQ(pixel(translucent,40,40),(std::array<uint8_t,4>{0,0,0,255}));
    proxy->color.w=1;
    proxy->flipX=true;const auto flipped=render(false);ASSERT_EQ(flipped.size(),instanced.size());
    EXPECT_EQ(pixel(flipped,24,24),pixel(instanced,40,24));
    proxy->world=NLS::Maths::Matrix4::Scaling({-1,1,1});const auto negative=render(false);EXPECT_EQ(negative,instanced);
    proxy->world=NLS::Maths::Matrix4::Identity;proxy->flipX=false;
    proxy->flipY=true;const auto flippedY=render(false);ASSERT_EQ(flippedY.size(),instanced.size());
    EXPECT_EQ(pixel(flippedY,24,24),pixel(instanced,24,40));
    EXPECT_EQ(pixel(flippedY,40,40),pixel(instanced,40,24));
    proxy->flipY=false;
    const auto occluded=render(false,0.001f),visible=render(false,1.f);
    ASSERT_EQ(occluded.size(),instanced.size());EXPECT_EQ(visible,instanced);
    EXPECT_EQ(pixel(occluded,24,24),(std::array<uint8_t,4>{0,0,0,255}));
    proxy->picking=true;proxy->color={0.25f,0.5f,0.75f,1};const auto picking=render(false);
    ASSERT_EQ(picking.size(),instanced.size());const auto id=pixel(picking,24,24);
    EXPECT_NEAR(id[0],64,1);EXPECT_NEAR(id[1],128,1);EXPECT_NEAR(id[2],191,1);
    EXPECT_EQ(pixel(picking,40,40),(std::array<uint8_t,4>{0,0,0,255}));
    proxy->color.w=0;const auto invisiblePicking=render(false);
    ASSERT_EQ(invisiblePicking.size(),instanced.size());
    EXPECT_EQ(pixel(invisiblePicking,24,24),(std::array<uint8_t,4>{0,0,0,255}));
    auto left=std::make_shared<E::Rendering::SpriteRenderProxy>(*proxy),right=std::make_shared<E::Rendering::SpriteRenderProxy>(*proxy);
    left->world=NLS::Maths::Matrix4::Translation({-.5f,0,0});left->ownerId=101;left->color={.25f,.5f,.75f,1};
    right->world=NLS::Maths::Matrix4::Translation({.5f,0,0});right->ownerId=102;right->color={.75f,.25f,.5f,1};
    drawSprites={left,right};
    const auto batchedIds=render(false),cpuIds=render(true);
    ASSERT_EQ(batchedIds.size(),instanced.size());EXPECT_EQ(batchedIds,cpuIds);
    EXPECT_EQ(renderer.GetStatistics().batches,1);EXPECT_EQ(renderer.GetStatistics().drawCalls,1);
    for (const std::array<uint8_t,3> expected : {std::array<uint8_t,3>{64,128,191},std::array<uint8_t,3>{191,64,128}})
    {
        size_t matches=0;
        for(size_t index=0;index<batchedIds.size();index+=4)
            if(std::abs(int(batchedIds[index])-int(expected[0]))<=1 &&
               std::abs(int(batchedIds[index+1])-int(expected[1]))<=1 &&
               std::abs(int(batchedIds[index+2])-int(expected[2]))<=1) ++matches;
        EXPECT_GT(matches,200)<<"Each object ID must survive picking instancing.";
    }
    right->sampler.minFilter=right->sampler.magFilter=H::TextureFilter::Linear;
    const auto splitIds=render(false),splitCpuIds=render(true);
    if (splitIds!=splitCpuIds)
    {
        std::string diagnostic;
        R::Tooling::WriteMaterialVisualEvidencePng((root/"split-instanced.png").string(),splitIds.data(),64,64,4,256,&diagnostic);
        R::Tooling::WriteMaterialVisualEvidencePng((root/"split-cpu.png").string(),splitCpuIds.data(),64,64,4,256,&diagnostic);
    }
    ASSERT_EQ(splitIds.size(),instanced.size());EXPECT_EQ(splitIds,splitCpuIds);
    EXPECT_EQ(renderer.GetStatistics().batches,2);EXPECT_EQ(renderer.GetStatistics().stateBreaks,1);
    // Nine source regions reveal stretching errors that a CPU/GPU equality
    // check alone would miss. Corners stay eight pixels wide at this PPU.
    const auto originalView=proxy->texture;
    auto bordered=std::make_shared<NLS::Core::Assets::SpriteArtifact>(*sprite);
    bordered->sourceWidth=bordered->width=bordered->sourceHeight=bordered->height=4;
    bordered->pixelsPerUnit=4;bordered->border={1,1,1,1};
    const std::array<std::array<uint8_t,4>,9> colors{{{255,0,0,255},{0,255,0,255},{0,0,255,255},
        {255,255,0,255},{255,255,255,255},{0,255,255,255},{255,0,255,255},{128,128,128,255},{255,128,0,255}}};
    std::array<uint8_t,64> borderPixels{};
    for(int y=0;y<4;++y)for(int x=0;x<4;++x)
    {const auto& color=colors[(y==0?0:y==3?2:1)*3+(x==0?0:x==3?2:1)];std::copy(color.begin(),color.end(),borderPixels.begin()+(y*4+x)*4);}
    auto borderDesc=textureDesc;borderDesc.extent={4,4,1};
    H::RHITextureUploadDesc borderUpload;borderUpload.data=borderPixels.data();borderUpload.dataSize=64;
    borderUpload.rowPitch=16;borderUpload.slicePitch=64;borderUpload.extent=borderDesc.extent;
    auto borderTexture=device->CreateTexture(borderDesc,borderUpload);ASSERT_TRUE(borderTexture);
    proxy->texture=device->CreateTextureView(borderTexture,textureView);ASSERT_TRUE(proxy->texture);
    proxy->sprite=bordered;proxy->sliced=true;proxy->size={1.5f,1.5f};proxy->picking=false;proxy->color={1,1,1,1};
    drawSprites={proxy};const auto nine=render(false);EXPECT_EQ(lastInstances,9);
    const auto nineCpu=render(true);EXPECT_EQ(lastInstances,1);EXPECT_EQ(lastVertices,54);EXPECT_EQ(nine,nineCpu);
    EXPECT_EQ(renderer.GetStatistics().submitted,1);EXPECT_EQ(renderer.GetStatistics().visible,1);EXPECT_EQ(renderer.GetStatistics().drawCalls,1);
    ASSERT_EQ(nine.size(),instanced.size());
    EXPECT_EQ(pixel(nine,10,10),colors[0]);EXPECT_EQ(pixel(nine,24,10),colors[1]);EXPECT_EQ(pixel(nine,52,10),colors[2]);
    EXPECT_EQ(pixel(nine,10,24),colors[3]);EXPECT_EQ(pixel(nine,24,24),colors[4]);EXPECT_EQ(pixel(nine,52,24),colors[5]);
    EXPECT_EQ(pixel(nine,10,52),colors[6]);EXPECT_EQ(pixel(nine,24,52),colors[7]);EXPECT_EQ(pixel(nine,52,52),colors[8]);
    const auto borderView=proxy->texture;
    auto tilePixels=borderPixels;
    const std::array<std::array<uint8_t,4>,4> centerColors{{{255,64,64,255},{64,255,64,255},{64,64,255,255},{255,255,64,0}}};
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)
        std::copy(centerColors[y*2+x].begin(),centerColors[y*2+x].end(),tilePixels.begin()+((y+1)*4+x+1)*4);
    borderUpload.data=tilePixels.data();auto tileTexture=device->CreateTexture(borderDesc,borderUpload);ASSERT_TRUE(tileTexture);
    proxy->texture=device->CreateTextureView(tileTexture,textureView);ASSERT_TRUE(proxy->texture);
    proxy->sliced=false;proxy->tiled=true;
    const auto tiledPixels=render(false);EXPECT_EQ(lastInstances,16);EXPECT_EQ(tiledPixels,render(true));EXPECT_EQ(lastVertices,96);
    EXPECT_EQ(pixel(tiledPixels,20,20),centerColors[0]);EXPECT_EQ(pixel(tiledPixels,36,20),centerColors[0]);
    EXPECT_EQ(pixel(tiledPixels,28,20),centerColors[1]);EXPECT_EQ(pixel(tiledPixels,20,28),centerColors[2]);
    EXPECT_EQ(pixel(tiledPixels,20,36),centerColors[0]);EXPECT_EQ(pixel(tiledPixels,28,28),(std::array<uint8_t,4>{0,0,0,255}));
    proxy->picking=true;proxy->color={.25f,.5f,.75f,1};
    const auto tilePicking=render(false);EXPECT_EQ(tilePicking,render(true));
    EXPECT_NEAR(pixel(tilePicking,20,20)[0],64,1);EXPECT_NEAR(pixel(tilePicking,20,20)[1],128,1);
    EXPECT_EQ(pixel(tilePicking,28,28),(std::array<uint8_t,4>{0,0,0,255}));
    proxy->texture=borderView;proxy->tiled=false;proxy->sliced=true;proxy->picking=false;proxy->color={1,1,1,1};
    proxy->flipX=true;const auto nineFlipped=render(false);EXPECT_EQ(pixel(nineFlipped,10,10),colors[2]);
    proxy->world=NLS::Maths::Matrix4::Scaling({-1,1,1});EXPECT_EQ(render(false),nine);
    proxy->flipX=false;proxy->world=NLS::Maths::Matrix4::Identity;proxy->size={.25f,.25f};
    const auto compressed=render(false);EXPECT_EQ(lastInstances,4);EXPECT_EQ(compressed,render(true));
    proxy->size={4,1.5f};proxy->world=NLS::Maths::Matrix4::Translation({1.5f,0,0});
    EXPECT_FALSE(render(false).empty());EXPECT_EQ(renderer.GetStatistics().visible,1);
    proxy->world=NLS::Maths::Matrix4::Identity;proxy->sliced=false;proxy->sprite=sprite;proxy->texture=originalView;
    auto outside=std::make_shared<E::Rendering::SpriteRenderProxy>(*proxy);
    outside->world=NLS::Maths::Matrix4::Translation({100,0,0});outside->picking=false;outside->color={1,1,1,1};
    for (bool culling : {true,false})
    {
        camera.SetFrustumGeometryCulling(culling);
        C::RenderScenePackage package;package.renderWidth=package.renderHeight=64;
        std::string error;
        ASSERT_TRUE(renderer.AppendPass(device,package,R::FrameGraph::FreezeFrameDescriptor(frame),{outside},error))<<error;
        EXPECT_EQ(renderer.GetStatistics().submitted,1);
        EXPECT_EQ(renderer.GetStatistics().culled,culling?1:0);
        EXPECT_EQ(renderer.GetStatistics().visible,culling?0:1);
    }
    camera.SetFrustumGeometryCulling(true);
    includeHelper=true;retainTarget=true;proxy->picking=false;proxy->color={1,1,1,1};drawSprites={proxy};
    for(bool cpu:{false,true})
    {
        retainedOutput.reset();retainedView.reset();
        proxy->world=NLS::Maths::Matrix4::Translation({-.5f,0,0});const auto firstFrame=render(cpu);
        proxy->world=NLS::Maths::Matrix4::Translation({.5f,0,0});const auto movedFrame=render(cpu);
        retainTarget=false;const auto freshRight=render(cpu);retainTarget=true;
        ASSERT_EQ(firstFrame.size(),instanced.size());EXPECT_NE(firstFrame,movedFrame);
        EXPECT_EQ(movedFrame,freshRight)<<"A retained viewport must not keep the Sprite's old position.";
        proxy->world=NLS::Maths::Matrix4::Translation({100,0,0});const auto culledFrame=render(cpu);
        drawSprites.clear();const auto removedFrame=render(cpu);
        EXPECT_EQ(culledFrame,removedFrame);
        EXPECT_NE(removedFrame,movedFrame)<<"Culling/removing the last Sprite must clear its old pixels.";
        EXPECT_EQ(renderer.GetStatistics().visible,0);
        drawSprites={proxy};
    }
    retainedOutput.reset();retainedView.reset();
    proxy->world=NLS::Maths::Matrix4::Translation({-.5f,0,0});render(false);
    clearTarget=false;proxy->world=NLS::Maths::Matrix4::Translation({.5f,0,0});const auto intentionalHistory=render(false);
    clearTarget=true;retainTarget=false;const auto clearedRight=render(false);
    EXPECT_NE(intentionalHistory,clearedRight)<<"An explicit camera no-clear request must retain previous contents.";
#endif
}
}
