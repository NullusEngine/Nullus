#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include "Rendering/Assets/DistanceFieldBuilder.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/DistanceField/DistanceFieldSceneData.h"
#include "Rendering/RHI/Utils/UploadContext/TextureUploadRegion.h"
#include "Rendering/DistanceField/DistanceFieldSystem.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/RHI/Backends/RHIDeviceFactory.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/ShaderBindingLayoutUtils.h"
#include "Rendering/Resources/SpirvShaderReflection.h"
#if defined(_WIN32)
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#endif

namespace
{
    NLS::Render::Assets::MeshArtifactData Box()
    {
        NLS::Render::Assets::MeshArtifactData mesh;
        for(int i=0;i<8;++i)
        {
            NLS::Render::Geometry::Vertex v{};
            v.position[0]=(i&1)?1.f:-1.f; v.position[1]=(i&2)?1.f:-1.f; v.position[2]=(i&4)?1.f:-1.f;
            mesh.vertices.push_back(v);
        }
        mesh.indices={0,2,3,0,3,1,4,5,7,4,7,6,0,4,6,0,6,2,1,3,7,1,7,5,0,1,5,0,5,4,2,6,7,2,7,3};
        return mesh;
    }
    NLS::Render::DistanceField::DistanceFieldBuildSettings Settings()
    {
        NLS::Render::DistanceField::DistanceFieldBuildSettings settings;
        settings.mode=NLS::Render::DistanceField::GenerationMode::Enabled;
        settings.maxResolution=21; return settings;
    }
    const NLS::Render::DistanceField::MeshDistanceFieldArtifact& BoxField()
    {
        static auto field=[] { const auto box=Box(); return NLS::Render::DistanceField::BuildMeshDistanceField({&box,1},Settings()).asset; }();
        return *field;
    }
    NLS::Render::Assets::MeshArtifactData Sphere()
    {
        using V=NLS::Maths::Vector3;
        std::vector<V> positions{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        std::vector<uint32_t> triangles{0,2,4,0,4,3,0,3,5,0,5,2,1,4,2,1,3,4,1,5,3,1,2,5};
        for(int level=0;level<2;++level)
        {
            std::map<std::pair<uint32_t,uint32_t>,uint32_t> edges;
            const auto middle=[&](uint32_t a,uint32_t b)
            {
                const auto key=std::minmax(a,b);
                if(const auto found=edges.find(key);found!=edges.end()) return found->second;
                auto p=positions[a]+positions[b]; p.Normalise();
                const auto index=static_cast<uint32_t>(positions.size()); positions.push_back(p); edges[key]=index; return index;
            };
            std::vector<uint32_t> next;
            for(size_t i=0;i<triangles.size();i+=3)
            {
                const auto a=triangles[i],b=triangles[i+1],c=triangles[i+2];
                const auto ab=middle(a,b),bc=middle(b,c),ca=middle(c,a);
                next.insert(next.end(),{a,ab,ca,ab,b,bc,ca,bc,c,ab,bc,ca});
            }
            triangles=std::move(next);
        }
        NLS::Render::Assets::MeshArtifactData mesh; mesh.indices=std::move(triangles);
        for(const auto& p:positions)
        { NLS::Render::Geometry::Vertex vertex{}; std::copy_n(p.array,3,vertex.position); mesh.vertices.push_back(vertex); }
        return mesh;
    }
}

using namespace NLS::Render::DistanceField;

TEST(DistanceFieldBuildTests, BuildsSignedBoxAndIndependentSparseMips)
{
    const auto& asset=BoxField();
    for(uint32_t level=0;level<3;++level)
    {
        EXPECT_TRUE(ValidateDistanceFieldMip(asset.mips[level]));
        EXPECT_LT(SampleMeshDistanceField(asset,{0,0,0},level).distance,0);
        const auto sample=SampleMeshDistanceField(asset,{1.08f,0,0},level);
        ASSERT_TRUE(sample.valid);
        EXPECT_NEAR(sample.distance,.08f,sample.voxelSize*std::sqrt(3.f)+asset.mips[level].maxDistance/127.f);
        for(int z=-6;z<=6;++z) for(int y=-6;y<=6;++y) for(int x=-6;x<=6;++x)
        {
            const NLS::Maths::Vector3 p{x*.2f,y*.2f,z*.2f};
            const NLS::Maths::Vector3 q{std::abs(p.x)-1,std::abs(p.y)-1,std::abs(p.z)-1};
            const NLS::Maths::Vector3 outside{(std::max)(q.x,0.f),(std::max)(q.y,0.f),(std::max)(q.z,0.f)};
            const float reference=outside.Length()+(std::min)((std::max)({q.x,q.y,q.z}),0.f);
            if(std::abs(reference)>=asset.mips[level].maxDistance) continue;
            const auto actual=SampleMeshDistanceField(asset,p,level);
            ASSERT_TRUE(actual.valid);
            EXPECT_NEAR(actual.distance,reference,actual.voxelSize*std::sqrt(3.f)+asset.mips[level].maxDistance/255.f);
        }
    }
    EXPECT_GT(asset.mips[0].grid[0],asset.mips[2].grid[0]);
}
TEST(DistanceFieldBuildTests, SphereApproximationPreservesSignAndMeetsAnalyticalErrorBound)
{
    const auto mesh=Sphere();
    const auto built=BuildMeshDistanceField({&mesh,1},Settings()); ASSERT_TRUE(built.success); ASSERT_TRUE(built.asset);
    EXPECT_TRUE(built.diagnostics.empty());
    // Every triangle lies between the inscribed and unit spheres. This gives
    // an independent signed-distance bound without reusing the builder's BVH.
    float inradius=1;
    for(size_t i=0;i<mesh.indices.size();i+=3)
    {
        const auto point=[&](uint32_t index){const auto* p=mesh.vertices[index].position;return NLS::Maths::Vector3{p[0],p[1],p[2]};};
        const auto a=point(mesh.indices[i]),b=point(mesh.indices[i+1]),c=point(mesh.indices[i+2]);
        auto normal=NLS::Maths::Vector3::Cross(b-a,c-a); normal.Normalise();
        inradius=(std::min)(inradius,std::abs(NLS::Maths::Vector3::Dot(normal,a)));
    }
    for(uint32_t mip=0;mip<3;++mip)
        for(int i=0;i<32;++i) for(float radius:{.1f,.6f,.9f,1.02f,1.15f})
        {
            const float z=1-2*(i+.5f)/32,r=std::sqrt(1-z*z),phi=i*2.39996323f;
            const auto sample=SampleMeshDistanceField(*built.asset,{radius*r*std::cos(phi),radius*r*std::sin(phi),radius*z},mip);
            ASSERT_TRUE(sample.valid);
            const float tolerance=sample.voxelSize*std::sqrt(3.f)+built.asset->mips[mip].maxDistance/255.f;
            if(std::abs(radius-1)>=built.asset->mips[mip].maxDistance-tolerance) continue;
            EXPECT_GE(sample.distance,radius-1-tolerance);
            EXPECT_LE(sample.distance,radius-inradius+tolerance);
            if(radius<.6f) EXPECT_LT(sample.distance,0);
        }
}
TEST(DistanceFieldBuildTests, DegenerateTrianglesAreDiagnosedAndSettingsInvalidateIdentity)
{
    auto mesh=Box(); mesh.indices.insert(mesh.indices.end(),{0,0,0});
    const auto built=BuildMeshDistanceField({&mesh,1},Settings());
    ASSERT_TRUE(built.success); ASSERT_TRUE(built.asset); EXPECT_FALSE(built.diagnostics.empty());
    EXPECT_EQ(built.asset->mips[0].bricks,BoxField().mips[0].bricks);
    auto changed=Settings(); changed.twoSided=true;
    const auto surface=BuildMeshDistanceField({&mesh,1},changed); ASSERT_TRUE(surface.asset);
    EXPECT_NE(surface.asset->buildIdentity,built.asset->buildIdentity);
    changed=Settings(); changed.resolutionScale=.5f;
    const auto lower=BuildMeshDistanceField({&mesh,1},changed); ASSERT_TRUE(lower.asset);
    EXPECT_NE(lower.asset->buildIdentity,built.asset->buildIdentity);
}
TEST(DistanceFieldBuildTests, RetainsInteriorAndExteriorConstantBricks)
{
    const auto& mip=BoxField().mips[0];
    EXPECT_NE(mip.indirection.size(),0u);
    EXPECT_LT(SampleMeshDistanceField(BoxField(),{0,0,0}).distance,0);
    EXPECT_GT(SampleMeshDistanceField(BoxField(),{4,0,0}).distance,0);
}
TEST(DistanceFieldBuildTests, RejectsInvalidIndicesAndNonFiniteGeometry)
{
    auto box=Box(); box.indices[0]=999;
    EXPECT_FALSE(BuildMeshDistanceField({&box,1},Settings()).success);
    box=Box(); box.vertices[0].position[0]=NAN;
    EXPECT_FALSE(BuildMeshDistanceField({&box,1},Settings()).success);
}
TEST(DistanceFieldBuildTests, SupportsCancellationAndDisabledGeneration)
{
    const auto box=Box(); std::atomic_bool cancelled{true};
    EXPECT_FALSE(BuildMeshDistanceField({&box,1},Settings(),&cancelled).success);
    auto settings=Settings(); settings.mode=GenerationMode::Disabled;
    const auto build=BuildMeshDistanceField({&box,1},settings);
    EXPECT_TRUE(build.success); EXPECT_FALSE(build.asset);
}
TEST(DistanceFieldBuildTests, TwoSidedOpenSurfaceHasNoNegativeInterior)
{
    auto box=Box(); box.indices.resize(6);
    auto settings=Settings(); settings.twoSided=true;
    const auto build=BuildMeshDistanceField({&box,1},settings);
    ASSERT_TRUE(build.success); ASSERT_TRUE(build.asset);
    EXPECT_FALSE(build.diagnostics.empty());
    EXPECT_GE(SampleMeshDistanceField(*build.asset,{0,0,-.8f}).distance,0);
}
TEST(DistanceFieldBuildTests, CombinesMaterialSectionsAndHasDeterministicIdentity)
{
    auto first=Box(),second=Box(); first.indices.resize(18);
    second.indices.erase(second.indices.begin(),second.indices.begin()+18);
    std::array sections{first,second};
    const auto combined=BuildMeshDistanceField(sections,Settings());
    ASSERT_TRUE(combined.success);
    EXPECT_EQ(combined.asset->buildIdentity,BoxField().buildIdentity);
    EXPECT_EQ(combined.asset->mips[0].bricks,BoxField().mips[0].bricks);
}
TEST(DistanceFieldArtifactTests, MainContainsOnlyMandatoryMipAndDetailsLoadIndependently)
{
    auto asset=BoxField(); asset.mipPaths[0]="distancefield:mesh:box:mip0";
    const auto bytes=SerializeDistanceFieldArtifact(asset);
    ASSERT_FALSE(bytes.empty());
    auto loaded=DeserializeDistanceFieldArtifact(bytes);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->mips[0].bricks.empty()); EXPECT_TRUE(loaded->mips[0].indirection.empty());
    EXPECT_EQ(loaded->mipPaths[0],asset.mipPaths[0]);
    EXPECT_EQ(loaded->mips[2].bricks,asset.mips[2].bricks);
    auto detail=DeserializeDistanceFieldMip(SerializeDistanceFieldMip(asset.mips[0]));
    ASSERT_TRUE(detail.has_value()); loaded->mips[0]=std::move(*detail);
    EXPECT_FLOAT_EQ(SampleMeshDistanceField(*loaded,{0,0,0}).distance,SampleMeshDistanceField(asset,{0,0,0}).distance);
}
TEST(DistanceFieldArtifactTests, RejectsCorruptionAndInvalidBrickIndices)
{
    auto bytes=SerializeDistanceFieldArtifact(BoxField()); bytes.back()^=1;
    EXPECT_FALSE(DeserializeDistanceFieldArtifact(bytes).has_value());
    auto mip=BoxField().mips[0]; mip.indirection[0]=mip.BrickCount()+10;
    EXPECT_FALSE(ValidateDistanceFieldMip(mip)); EXPECT_TRUE(SerializeDistanceFieldMip(mip).empty());
}
TEST(DistanceFieldTextureUploadTests, ValidatesVolumeRegionAndPaddedSlices)
{
    using namespace NLS::Render::RHI;
    RHITextureDesc desc; desc.dimension=TextureDimension::Texture3D; desc.format=TextureFormat::R8;
    desc.extent={16,16,16}; desc.mipLevels=3;
    TextureUploadRegionLayout layout; std::string error;
    EXPECT_TRUE(ValidateTextureUploadRegion(desc,1,0,{1,2,3},{3,2,4},8,24,83,layout,error));
    EXPECT_EQ(layout.requiredBytes,83u);
    EXPECT_FALSE(ValidateTextureUploadRegion(desc,1,0,{1,2,3},{3,2,4},8,24,82,layout,error));
    EXPECT_FALSE(ValidateTextureUploadRegion(desc,1,0,{1,2,7},{3,2,4},8,24,1024,layout,error));
    EXPECT_FALSE(ValidateTextureUploadRegion(desc,1,0,{0,0,0},{3,2,4},2,24,1024,layout,error));
}

TEST(DistanceFieldSceneTests, IncrementalUpdatesClearOldBoundsAndKeepSceneIdentitiesSeparate)
{
    DistanceFieldSceneData scene;
    DistanceFieldObjectInput a,b;
    a.sceneId=1; a.index=3; a.generation=1; a.asset=std::make_shared<MeshDistanceFieldArtifact>(BoxField());
    b=a; b.sceneId=2; b.localToWorld=NLS::Maths::Matrix4::Translation({8,0,0});
    std::array objects{a,b}; scene.Synchronize(objects);
    ASSERT_EQ(scene.Objects().size(),2u);
    const auto before=scene.Revision(); scene.Synchronize(objects);
    EXPECT_EQ(scene.Revision(),before);
    EXPECT_LT(scene.SampleReference({0,0,0},0).distance,0);
    EXPECT_LT(scene.SampleReference({8,0,0},0).distance,0);
    objects[0].localToWorld=NLS::Maths::Matrix4::Translation({4,0,0}); scene.Synchronize(objects);
    std::vector<DistanceFieldDirtyBounds> changed;
    ASSERT_TRUE(scene.ChangesSince(before,changed)); EXPECT_EQ(changed.size(),2u);
    EXPECT_GT(scene.SampleReference({0,0,0},0).distance,0);
    scene.Synchronize({}); EXPECT_TRUE(scene.Objects().empty()); EXPECT_FALSE(scene.SampleReference({0,0,0}).valid);
}
TEST(DistanceFieldSceneTests, MirroredNonUniformScaleIsConservativeAndSingularTransformIsExcluded)
{
    DistanceFieldObjectInput object; object.sceneId=1; object.generation=1;
    object.asset=std::make_shared<MeshDistanceFieldArtifact>(BoxField());
    object.localToWorld.data[0]=-2; object.localToWorld.data[5]=3; object.localToWorld.data[10]=4;
    DistanceFieldSceneData scene(1); scene.Synchronize({&object,1});
    ASSERT_EQ(scene.Objects().size(),1u); EXPECT_NEAR(scene.Objects()[0].distanceScale,2,1e-5);
    EXPECT_LT(scene.SampleReference({0,0,0},0).distance,0);
    object.localToWorld.data[0]=0; scene.Synchronize({&object,1}); EXPECT_TRUE(scene.Objects().empty());
}
TEST(DistanceFieldSceneTests, StationaryViewDoesNoPageWorkAndCameraScrollOnlyExposesSlabs)
{
    DistanceFieldSceneData scene;
    DistanceFieldViewState view;
    EXPECT_EQ(view.Update(scene,{0,0,0}).size(),DistanceFieldViewState::PageTableEntries);
    EXPECT_TRUE(view.Update(scene,{0,0,0}).empty());
    const auto scrolled=view.Update(scene,{1.61f,0,0});
    EXPECT_GT(scrolled.size(),0u); EXPECT_LT(scrolled.size(),DistanceFieldViewState::PageTableEntries);
    EXPECT_EQ(view.Update(scene,{10000,10000,10000}).size(),DistanceFieldViewState::PageTableEntries);
}

TEST(DistanceFieldSceneTests, MaterialSectionsShareAClosedFieldAndSplitWhenTheirTransformsDiverge)
{
    namespace assets=NLS::Render::Assets;
    const std::string logical="mesh/sections";
    const auto firstKey="mesh:"+assets::BuildPrimitiveMeshSourceKey(logical,0);
    const auto secondKey="mesh:"+assets::BuildPrimitiveMeshSourceKey(logical,1);
    auto first=Box(),second=Box(); first.indices.resize(18);
    second.indices.erase(second.indices.begin(),second.indices.begin()+18); second.materialIndex=1;
    std::map<std::string,assets::MeshArtifactData> parts{{firstKey,first},{secondKey,second}};
    assets::ImportedScene scene; scene.materials.resize(2);
    std::vector<NLS::Core::Assets::ArtifactPayload> payloads; std::vector<std::string> diagnostics;
    ASSERT_TRUE(AppendDistanceFieldModelPayloads(scene,parts,Settings(),payloads,diagnostics));
    EXPECT_EQ(payloads.size(),9u);
    NLS::Core::Assets::ArtifactManifest manifest;
    const auto root=std::filesystem::current_path()/"build/tmp/distance-field-sections";
    for(const auto& [key,part]:parts)
    {
        NLS::Core::Assets::ImportedArtifact mesh;
        mesh.subAssetKey=key; mesh.artifactType=NLS::Core::Assets::ArtifactType::Mesh;
        mesh.artifactPath=(root/(key==firstKey?"first.nmesh":"second.nmesh")).generic_string();
        manifest.subAssets.push_back(mesh);
    }
    for(const auto& payload:payloads)
    {
        NLS::Core::Assets::ImportedArtifact artifact;
        artifact.subAssetKey=payload.subAssetKey; artifact.artifactType=payload.artifactType;
        artifact.loaderId=payload.loaderId; artifact.artifactPath=(root/payload.relativePath).generic_string();
        manifest.subAssets.push_back(artifact);
    }
    RegisterDistanceFieldManifest(manifest);
    std::vector<DistanceFieldObjectInput> objects(2);
    for(uint32_t i=0;i<2;++i)
    { objects[i].sceneId=1; objects[i].index=i; objects[i].generation=1; objects[i].meshArtifactPath=manifest.subAssets[i].artifactPath; }
    auto grouped=GroupDistanceFieldObjects(objects);
    ASSERT_EQ(grouped.size(),1u);
    EXPECT_EQ(grouped[0].meshArtifactPath,manifest.FindSubAsset(DistanceFieldSubAssetKey("mesh:"+logical))->artifactPath);
    objects[1].localToWorld=NLS::Maths::Matrix4::Translation({3,0,0});
    EXPECT_EQ(GroupDistanceFieldObjects(objects).size(),2u);
    objects.resize(1);
    auto incomplete=GroupDistanceFieldObjects(objects); ASSERT_EQ(incomplete.size(),1u);
    EXPECT_EQ(incomplete[0].meshArtifactPath,manifest.subAssets[0].artifactPath);
}

TEST(DistanceFieldGpuTests, Dx12SparsePagesMatchCpuAndDeletionLeavesNoSurface)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 distance field validation runs on Windows";
#else
    namespace rhi=NLS::Render::RHI;
    namespace context=NLS::Render::Context;
    auto& executor=context::RhiCommandListExecutor::Get();
    if(executor.IsInitialized()) GTEST_SKIP() << "Another suite owns the RHI executor";
    executor.ResetForTesting();
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown(){executor.Shutdown();executor.ResetForTesting();} } shutdown{executor};
    NLS::Render::Settings::DriverSettings settings;
    settings.debugMode=true; settings.graphicsBackend=NLS::Render::Settings::EGraphicsBackend::DX12;
    auto device=NLS::Render::Backend::CreateRhiDevice(settings);
    ASSERT_TRUE(device && device->IsBackendReady());
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
    auto* native=static_cast<ID3D12Device*>(device->GetNativeDeviceInfo().device);
    if(native) native->QueryInterface(IID_PPV_ARGS(&messages));
    if(messages) messages->ClearStoredMessages();
    auto system=DistanceFieldSystem::Get(device);
    DistanceFieldFrameInput input;
    input.viewId=DistanceFieldSystem::AllocateViewId(); input.viewLifetime=std::make_shared<int>(1);
    input.debug.visualization=DistanceFieldVisualization::Global;
    DistanceFieldObjectInput object;
    object.sceneId=1; object.index=3; object.generation=1;
    object.asset=std::make_shared<MeshDistanceFieldArtifact>(BoxField());
    input.objects={object}; input.probePositions={{0,0,0,1},{1.5f,0,0,1},{4,0,0,1},{10000,0,0,1}};
    const auto execute=[&](DistanceFieldFrameInput frame,std::vector<NLS::Maths::Vector4>& values)
    {
        auto built=system->BuildFrame(std::move(frame));
        EXPECT_TRUE(built.stats.diagnostic.empty()) << built.stats.diagnostic;
        if(!built.probeResults) { ADD_FAILURE() << "Probe result buffer was not produced"; return built.stats; }
        auto pool=device->CreateCommandPool(rhi::QueueType::Graphics,"DistanceFieldValidation");
        auto commands=pool->CreateCommandBuffer("DistanceFieldValidation");
        commands->Begin();
        for(const auto& pass:built.passes) context::RecordComputeDispatchInputs(*commands,pass.computeDispatchInputs);
        commands->End();
        auto fence=device->CreateFence("DistanceFieldValidationComplete"); fence->Reset();
        rhi::RHISubmitDesc submit; submit.commandBuffers={commands}; submit.signalFence=fence;
        const auto submitted=device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_TRUE(submitted.Succeeded()) << submitted.message;
        if(!submitted.Succeeded()) return built.stats;
        EXPECT_TRUE(fence->Wait(30000000000ull));
        if(built.timingReadback)
        {
            const auto timing=device->BeginReadBuffer(built.timingReadback->desc);
            std::lock_guard lock(built.timingReadback->state->mutex);
            built.timingReadback->state->beginAttempted=true;
            built.timingReadback->state->beginSucceeded=timing.Succeeded();
            built.timingReadback->state->completion=timing.completion;
            if(timing.completion) EXPECT_TRUE(timing.completion->Wait(30000000000ull).Succeeded());
        }
        values.resize(4);
        rhi::RHIBufferReadbackDesc read;
        read.source=built.probeResults; read.sourceState=built.probeResults->GetState();
        read.data=values.data(); read.size=values.size()*sizeof(values[0]);
        const auto readback=device->BeginReadBuffer(read);
        EXPECT_TRUE(readback.Succeeded()) << readback.message;
        if(readback.completion) EXPECT_TRUE(readback.completion->Wait(30000000000ull).Succeeded());
        return built.stats;
    };
    std::vector<NLS::Maths::Vector4> values;
    input.frameSerial=1;
    const auto first=execute(input,values);
    ASSERT_EQ(values.size(),4u);
    DistanceFieldSceneData reference; reference.Synchronize(input.objects);
    for(uint32_t i=0;i<3;++i)
    {
        EXPECT_EQ(values[i].z,1) << i;
        const auto expected=reference.SampleReference({input.probePositions[i].x,input.probePositions[i].y,input.probePositions[i].z},0);
        EXPECT_NEAR(values[i].x,expected.distance,values[i].y*std::sqrt(3.f)+.03f) << i;
    }
    EXPECT_LT(values[0].x,0); EXPECT_GT(values[1].x,0); EXPECT_EQ(values[3].z,0);
    input.frameSerial=2;
    const auto stationary=execute(input,values);
    EXPECT_EQ(stationary.dirtyPages,0u);
    EXPECT_EQ(stationary.uploadedBytes,0u);
    EXPECT_TRUE(stationary.gpuUpdateMs.has_value());
    if(stationary.gpuUpdateMs) EXPECT_GT(*stationary.gpuUpdateMs,0);
    NLS::Render::Entities::Camera camera; camera.CacheProjectionMatrix(64,64);
    input.inverseViewProjection=NLS::Maths::Matrix4::Inverse(camera.GetProjectionMatrix()*NLS::Maths::Matrix4::Translation({0,0,-5}));
    input.cameraPosition={0,0,5}; input.width=input.height=64; input.traceDistance=64;
    rhi::RHITextureDesc viewTarget; viewTarget.extent={64,64,1}; viewTarget.format=rhi::TextureFormat::RGBA8;
    viewTarget.usage=rhi::TextureUsageFlags::Sampled|rhi::TextureUsageFlags::CopyDst|rhi::TextureUsageFlags::CopySrc;
    input.outputTexture=device->CreateTexture(viewTarget); ASSERT_TRUE(input.outputTexture);
    execute(input,values);
    std::vector<uint8_t> image(64*64*4);
    const auto picture=device->ReadPixelsChecked(input.outputTexture,0,0,64,64,
        NLS::Render::Settings::EPixelDataFormat::RGBA,NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,image.data());
    ASSERT_TRUE(picture.Succeeded()) << picture.message;
    EXPECT_GT(image[(32*64+32)*4],30);
    EXPECT_LT(image[0],30);
    const auto picturePath=std::filesystem::current_path()/"build/distance-field-tests/global-distance-field.ppm";
    { std::ofstream file(picturePath,std::ios::binary); file << "P6\n64 64\n255\n";
      for(size_t i=0;i<image.size();i+=4) file.write(reinterpret_cast<const char*>(image.data()+i),3); }
    input.cameraPosition={0,0,0};
    input.debug.channel=DistanceFieldDebugChannel::Residency;
    for(const auto mode:{DistanceFieldVisualization::Global,DistanceFieldVisualization::Mesh})
    {
        input.debug.visualization=mode; execute(input,values);
        ASSERT_TRUE(device->ReadPixelsChecked(input.outputTexture,0,0,64,64,
            NLS::Render::Settings::EPixelDataFormat::RGBA,NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,image.data()).Succeeded());
        EXPECT_GT(image[(32*64+32)*4+1],image[(32*64+32)*4]);
    }
    input.debug.visualization=DistanceFieldVisualization::Global; input.debug.channel=DistanceFieldDebugChannel::Surface;
    input.outputTexture.reset(); input.width=input.height=0; input.cameraPosition={0,0,0};
    input.frameSerial=3;
    input.objects[0].localToWorld=NLS::Maths::Matrix4::Translation({4,0,0});
    input.objects[0].localToWorld.data[0]=-2; input.objects[0].localToWorld.data[5]=3; input.objects[0].localToWorld.data[10]=.5f;
    input.objects[0].mostlyStatic=false;
    execute(input,values);
    EXPECT_GT(values[0].x,0); EXPECT_LT(values[2].x,0);
    auto second=input; second.viewId=DistanceFieldSystem::AllocateViewId(); second.viewLifetime=std::make_shared<int>(2);
    second.objects={object}; second.frameSerial=4;
    const auto duplicate=execute(second,values);
    EXPECT_LT(values[0].x,0); EXPECT_EQ(duplicate.residentBricks,first.residentBricks);
    second.viewLifetime.reset();
    input.frameSerial=5; input.objects.clear();
    execute(input,values);
    for(const auto& value:values) EXPECT_EQ(value.z,0);
    // Recycled primitive slots and replacement meshes invalidate both caches.
    auto replacement=object; replacement.sceneId=2; replacement.generation=2;
    input.objects={replacement}; input.frameSerial=6; execute(input,values);
    EXPECT_LT(values[0].x,0); EXPECT_TRUE(values[2].z==0 || values[2].x>0);
    auto smallBox=Box(); for(auto& vertex:smallBox.vertices) for(float& coordinate:vertex.position) coordinate*=.5f;
    const auto rebuilt=BuildMeshDistanceField({&smallBox,1},Settings()); ASSERT_TRUE(rebuilt.asset);
    input.objects[0].asset=rebuilt.asset; input.frameSerial=7; execute(input,values);
    EXPECT_LT(values[0].x,0); EXPECT_TRUE(values[1].z==0 || values[1].x>0);
    input.objects[0].enabled=false; input.frameSerial=8; execute(input,values);
    for(const auto& value:values) EXPECT_EQ(value.z,0);
    input.objects[0].enabled=true; input.cameraPosition={2,0,0}; input.frameSerial=9; execute(input,values);
    EXPECT_EQ(values[0].z,1); EXPECT_LT(values[0].x,0);
    input.cameraPosition={10000,10000,10000}; input.frameSerial=10; execute(input,values);
    for(const auto& value:values) EXPECT_EQ(value.z,0);
    input.cameraPosition={0,0,0}; input.frameSerial=11; execute(input,values);
    EXPECT_EQ(values[0].z,1); EXPECT_LT(values[0].x,0);
    input.objects.clear(); input.frameSerial=12; execute(input,values);
    for(const auto& value:values) EXPECT_EQ(value.z,0);
    // Identical artifacts can be mounted under different project/install roots.
    // Their content identity, rather than their host path, owns the atlas data.
    namespace coreAssets=NLS::Core::Assets;
    const auto sharedRoot=std::filesystem::current_path()/"build/tmp"/("distance-field-shared-"+NLS::Guid::New().ToString());
    const auto mainBytes=SerializeDistanceFieldArtifact(BoxField());
    const auto contentHash=coreAssets::ToHexString(coreAssets::ComputeSha256(mainBytes.data(),mainBytes.size()));
    std::array<std::string,2> meshPaths;
    for(uint32_t copy=0;copy<2;++copy)
    {
        const auto directory=sharedRoot/std::to_string(copy); std::filesystem::create_directories(directory);
        coreAssets::ArtifactManifest manifest; manifest.sourceAssetId=coreAssets::AssetId::New(); manifest.targetPlatform="win64-dx12";
        coreAssets::ImportedArtifact mesh; mesh.subAssetKey="mesh:shared"; mesh.artifactType=coreAssets::ArtifactType::Mesh;
        mesh.artifactPath=(directory/"shared.nmesh").generic_string(); meshPaths[copy]=mesh.artifactPath; manifest.subAssets.push_back(mesh);
        for(uint32_t mip=0;mip<3;++mip)
        {
            const auto bytes=mip==2?mainBytes:SerializeDistanceFieldMip(BoxField().mips[mip]);
            const auto path=directory/("field-"+std::to_string(mip)+".nldf");
            { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
            coreAssets::ImportedArtifact field; field.subAssetKey=DistanceFieldSubAssetKey(mesh.subAssetKey,mip==2?MipCount:mip);
            field.artifactType=coreAssets::ArtifactType::MeshDistanceField; field.artifactPath=path.generic_string();
            field.loaderId=mip==2?"mesh-distance-field":"mesh-distance-field-mip";
            if(mip==2) field.contentHash=contentHash;
            manifest.subAssets.push_back(std::move(field));
        }
        RegisterDistanceFieldManifest(manifest);
    }
    uint64_t nextDiskFrame=13;
    const auto settle=[&]()
    {
        DistanceFieldStats stats;
        for(int attempt=0;attempt<64;++attempt)
        {
            input.frameSerial=nextDiskFrame++; stats=execute(input,values);
            if(values[0].z!=0 && !DistanceFieldSystem::HasPendingWork()) return stats;
        }
        ADD_FAILURE() << "Artifact-backed field did not finish loading";
        return stats;
    };
    auto diskObject=object; diskObject.asset.reset(); diskObject.meshArtifactPath=meshPaths[0];
    input.objects={diskObject}; const auto oneArtifact=settle();
    diskObject.index++; diskObject.meshArtifactPath=meshPaths[1]; input.objects.push_back(diskObject);
    const auto sharedArtifacts=settle();
    EXPECT_EQ(sharedArtifacts.objects,2u); EXPECT_EQ(sharedArtifacts.residentBricks,oneArtifact.residentBricks);
    input.objects.clear(); input.frameSerial=nextDiskFrame++; execute(input,values);
    // An empty second view must not hide another view's unfinished uploads
    // from the frame-cache invalidation signal.
    auto largeField=std::make_shared<MeshDistanceFieldArtifact>(BoxField());
    largeField->buildIdentity+="-upload-budget";
    auto& largeMip=largeField->mips[0]; largeMip.grid={32,32,32};
    largeMip.indirection.resize(32*32*32); largeMip.bricks.assign(32*32*32*512,128);
    for(uint32_t brick=0;brick<largeMip.indirection.size();++brick) largeMip.indirection[brick]=brick;
    auto largeObject=object; largeObject.asset=largeField;
    input.objects={largeObject}; input.frameSerial=nextDiskFrame++;
    const auto partialUpload=execute(input,values);
    EXPECT_LE(partialUpload.uploadedBytes,8u*1024u*1024u);
    ASSERT_TRUE(DistanceFieldSystem::HasPendingWork());
    auto emptyView=input; emptyView.objects.clear(); emptyView.viewId=DistanceFieldSystem::AllocateViewId();
    emptyView.viewLifetime=DistanceFieldSystem::CreateViewLifetime(); execute(emptyView,values);
    EXPECT_TRUE(DistanceFieldSystem::HasPendingWork());
    emptyView.viewLifetime.reset();
    for(int attempt=0;attempt<4 && DistanceFieldSystem::HasPendingWork();++attempt)
    { input.frameSerial=nextDiskFrame++; execute(input,values); }
    EXPECT_FALSE(DistanceFieldSystem::HasPendingWork());
    input.objects.clear(); input.frameSerial=nextDiskFrame++; execute(input,values);
    // Three views of a very large object exceed the shared page pool. Invalid
    // pages must remain invalid, and releasing views must make retries succeed.
    std::array<DistanceFieldFrameInput,3> pressure;
    uint64_t frameSerial=nextDiskFrame;
    for(auto& view:pressure)
    {
        view=input; view.viewId=DistanceFieldSystem::AllocateViewId(); view.viewLifetime=DistanceFieldSystem::CreateViewLifetime();
        view.objects={object}; view.objects[0].localToWorld.data[0]=4096;
        view.objects[0].localToWorld.data[5]=4096; view.objects[0].localToWorld.data[10]=4096;
        view.frameSerial=frameSerial++; execute(view,values);
        for(uint32_t i=0;i<3;++i) if(values[i].z!=0) { EXPECT_TRUE(std::isfinite(values[i].x)); EXPECT_LT(values[i].x,0); }
    }
    DistanceFieldStats exhausted;
    for(int i=0;i<10;++i) { pressure[2].frameSerial=frameSerial++; exhausted=execute(pressure[2],values); }
    EXPECT_GT(exhausted.allocationFailures,0u);
    EXPECT_GT(exhausted.allocationRetries,0u);
    EXPECT_LE(exhausted.residentPages,131072u);
    pressure[0].viewLifetime.reset(); pressure[1].viewLifetime.reset();
    pressure[2].debug.forceRebuild=true; pressure[2].frameSerial=frameSerial++;
    execute(pressure[2],values);
    for(uint32_t i=0;i<3;++i) { EXPECT_EQ(values[i].z,1); EXPECT_LT(values[i].x,0); }
    if(messages)
        for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
        {
            SIZE_T size=0; if(FAILED(messages->GetMessage(i,nullptr,&size))) continue;
            std::vector<uint8_t> storage(size); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(messages->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)
                ADD_FAILURE() << message->pDescription;
        }
#endif
}

TEST(DistanceFieldGpuTests, VolumeMipRegionAndStorageImageReflectionRoundTrip)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 volume validation runs on Windows";
#else
    namespace rhi=NLS::Render::RHI;
    namespace shader=NLS::Render::ShaderCompiler;
    namespace resources=NLS::Render::Resources;
    namespace context=NLS::Render::Context;
    auto& executor=context::RhiCommandListExecutor::Get();
    if(executor.IsInitialized()) GTEST_SKIP() << "Another suite owns the RHI executor";
    executor.ResetForTesting();
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown(){executor.Shutdown();executor.ResetForTesting();} } shutdown{executor};
    NLS::Render::Settings::DriverSettings settings; settings.debugMode=true;
    settings.graphicsBackend=NLS::Render::Settings::EGraphicsBackend::DX12;
    auto device=NLS::Render::Backend::CreateRhiDevice(settings);
    ASSERT_TRUE(device && device->IsBackendReady());
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
    auto* native=static_cast<ID3D12Device*>(device->GetNativeDeviceInfo().device);
    if(native) native->QueryInterface(IID_PPV_ARGS(&messages));
    if(messages) messages->ClearStoredMessages();
    rhi::RHITextureDesc desc; desc.dimension=rhi::TextureDimension::Texture3D;
    desc.extent={16,16,16}; desc.mipLevels=3; desc.format=rhi::TextureFormat::R8;
    desc.usage=rhi::TextureUsageFlags::Sampled|rhi::TextureUsageFlags::CopyDst;
    auto volume=device->CreateTexture(desc); ASSERT_TRUE(volume);
    std::vector<uint8_t> data(20);
    for(uint32_t z=0;z<2;++z) for(uint32_t y=0;y<2;++y) for(uint32_t x=0;x<2;++x)
        data[z*13+y*5+x]=uint8_t(50+40*z+10*y+x);
    rhi::RHITextureUpdateDesc update; update.texture=volume; update.data=data.data(); update.dataSize=data.size();
    update.mipLevel=2; update.x=update.y=update.z=1; update.extent={2,2,2}; update.rowPitch=5; update.slicePitch=13;
    ASSERT_TRUE(device->UpdateTexture(update).Succeeded());
    update.z=3; EXPECT_FALSE(device->UpdateTexture(update).Succeeded());
    const auto root=std::filesystem::current_path()/"build/tmp/distance-field-volume";
    std::filesystem::create_directories(root);
    const auto source=root/"volume.hlsl";
    { std::ofstream file(source); file << R"(
Texture3D<float> Input : register(t0);
RWStructuredBuffer<uint> Output : register(u0);
RWTexture3D<float> OutputVolume : register(u1);
[numthreads(2,2,2)] void Main(uint3 p:SV_DispatchThreadID)
{
    float value=Input.Load(int4(p+1,2));
    Output[(p.z*2+p.y)*2+p.x]=(uint)round(value*255);
    OutputVolume[p]=value;
}
)"; }
    shader::ShaderCompiler compiler;
    shader::ShaderCompilationInput input; input.assetPath=source.string(); input.stage=shader::ShaderStage::Compute;
    input.options.targetPlatform=shader::ShaderTargetPlatform::DXIL; input.options.targetProfile="cs_6_0";
    input.options.entryPoint="Main"; input.options.artifactDirectory=(root/"cache").string();
    const auto compiled=compiler.Compile(input);
    ASSERT_EQ(compiled.status,shader::ShaderCompilationStatus::Succeeded) << compiled.diagnostics;
    const auto reflection=compiler.Reflect(input,compiled);
    const auto checkReflection=[](const resources::ShaderReflection& reflection)
    {
        for(const auto& name:{"Input","OutputVolume"})
        {
            const auto property=std::find_if(reflection.properties.begin(),reflection.properties.end(),[&](const auto& p){return p.name==name;});
            ASSERT_NE(property,reflection.properties.end()) << name;
            EXPECT_EQ(property->textureDimension,rhi::TextureDimension::Texture3D);
            EXPECT_EQ(property->kind,std::string_view(name)=="Input"?resources::ShaderResourceKind::SampledTexture:resources::ShaderResourceKind::StorageTexture);
        }
    };
    checkReflection(reflection);
    NLS::Render::Assets::ShaderArtifact artifact; artifact.sourcePath=source.string(); artifact.subAssetKey="shader:volume";
    artifact.reflection=reflection; artifact.stages.push_back({shader::ShaderStage::Compute,shader::ShaderTargetPlatform::DXIL,"Main","cs_6_0",compiled,0});
    const auto loaded=NLS::Render::Assets::DeserializeShaderArtifact(NLS::Render::Assets::SerializeShaderArtifact(artifact));
    ASSERT_TRUE(loaded); checkReflection(loaded->reflection);
    auto spirvInput=input; spirvInput.options.targetPlatform=shader::ShaderTargetPlatform::SPIRV;
    const auto spirv=compiler.Compile(spirvInput);
    ASSERT_EQ(spirv.status,shader::ShaderCompilationStatus::Succeeded) << spirv.diagnostics;
    resources::ShaderReflection spirvReflection; std::string spirvError;
    ASSERT_TRUE(resources::ReflectSpirvShader(spirv.bytecode,shader::ShaderStage::Compute,spirvReflection,spirvError)) << spirvError;
    checkReflection(spirvReflection);
    const auto samplingSource=root/"sampling.hlsl";
    { std::ofstream file(samplingSource); file << R"(
#include "DistanceFieldSampling.hlsli"
[numthreads(1,1,1)] void Main(uint3 id:SV_DispatchThreadID)
{
    DistanceFieldSample result=SampleGlobalDistanceField(float3(0,0,0));
    ProbeResults[0]=float4(result.Distance,result.Gradient);
}
)"; }
    auto samplingInput=input; samplingInput.assetPath=samplingSource.string();
    samplingInput.options.includeDirectories.push_back((std::filesystem::current_path()/"App/Assets/Engine/Shaders/HLSL").string());
    const auto samplingCompiled=compiler.Compile(samplingInput);
    EXPECT_EQ(samplingCompiled.status,shader::ShaderCompilationStatus::Succeeded) << samplingCompiled.diagnostics;
    auto layoutDescs=resources::BuildExplicitBindingLayoutDescsBySet(reflection,"Volume");
    ASSERT_FALSE(layoutDescs.empty());
    auto layout=device->CreateBindingLayout(layoutDescs[0]); ASSERT_TRUE(layout);
    rhi::RHIPipelineLayoutDesc pipelineLayoutDesc; pipelineLayoutDesc.bindingLayouts={layout};
    auto pipelineLayout=device->CreatePipelineLayout(pipelineLayoutDesc);
    rhi::RHIShaderModuleDesc module; module.stage=rhi::ShaderStage::Compute; module.targetBackend=rhi::NativeBackendType::DX12;
    module.entryPoint="Main"; module.bytecode=compiled.bytecode;
    rhi::RHIComputePipelineDesc pipelineDesc; pipelineDesc.pipelineLayout=pipelineLayout; pipelineDesc.computeShader=device->CreateShaderModule(module);
    auto pipeline=device->CreateComputePipeline(pipelineDesc); ASSERT_TRUE(pipeline);
    rhi::RHIBufferDesc outputDesc; outputDesc.size=32; outputDesc.usage=rhi::BufferUsageFlags::Storage|rhi::BufferUsageFlags::CopySrc;
    auto output=device->CreateBuffer(outputDesc); ASSERT_TRUE(output);
    desc.mipLevels=1; desc.extent={2,2,2}; desc.format=rhi::TextureFormat::R32F;
    desc.usage=rhi::TextureUsageFlags::Storage|rhi::TextureUsageFlags::Sampled;
    auto resultVolume=device->CreateTexture(desc); ASSERT_TRUE(resultVolume);
    rhi::RHITextureViewDesc viewDesc; viewDesc.viewType=rhi::TextureViewType::Texture3D;
    rhi::RHIBindingSetDesc bindings; bindings.layout=layout;
    rhi::RHIBindingSetEntry sampled; sampled.type=rhi::BindingType::Texture; sampled.binding=0;
    viewDesc.subresourceRange.mipLevelCount=3;
    sampled.textureView=device->CreateTextureView(volume,viewDesc); bindings.entries.push_back(sampled);
    rhi::RHIBindingSetEntry stored; stored.type=rhi::BindingType::StorageBuffer; stored.binding=0; stored.buffer=output; stored.elementStride=4;
    bindings.entries.push_back(stored);
    rhi::RHIBindingSetEntry image; image.type=rhi::BindingType::RWTexture; image.binding=1;
    viewDesc.subresourceRange.mipLevelCount=1;
    image.textureView=device->CreateTextureView(resultVolume,viewDesc); bindings.entries.push_back(image);
    auto bindingSet=device->CreateBindingSet(bindings); ASSERT_TRUE(bindingSet);
    auto pool=device->CreateCommandPool(rhi::QueueType::Graphics,"VolumeValidation");
    auto commands=pool->CreateCommandBuffer("VolumeValidation"); commands->Begin();
    rhi::RHIBarrierDesc barrier;
    barrier.bufferBarriers.push_back({output,rhi::ResourceState::Unknown,rhi::ResourceState::ShaderWrite});
    barrier.textureBarriers.push_back({resultVolume,rhi::ResourceState::Unknown,rhi::ResourceState::ShaderWrite});
    commands->Barrier(barrier); commands->BindComputePipeline(pipeline); commands->BindBindingSet(0,bindingSet); commands->Dispatch(1,1,1); commands->End();
    auto fence=device->CreateFence("VolumeValidationComplete"); fence->Reset();
    rhi::RHISubmitDesc submit; submit.commandBuffers={commands}; submit.signalFence=fence;
    ASSERT_TRUE(device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit).Succeeded()); ASSERT_TRUE(fence->Wait(30000000000ull));
    std::array<uint32_t,8> values{}; rhi::RHIBufferReadbackDesc read;
    read.source=output; read.sourceState=rhi::ResourceState::ShaderWrite; read.size=sizeof(values); read.data=values.data();
    const auto readback=device->BeginReadBuffer(read); ASSERT_TRUE(readback.Succeeded());
    if(readback.completion) ASSERT_TRUE(readback.completion->Wait(30000000000ull).Succeeded());
    for(uint32_t z=0;z<2;++z) for(uint32_t y=0;y<2;++y) for(uint32_t x=0;x<2;++x)
        EXPECT_EQ(values[(z*2+y)*2+x],50+40*z+10*y+x);
    if(messages) for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i)
    {
        SIZE_T size=0; if(FAILED(messages->GetMessage(i,nullptr,&size))) continue;
        std::vector<uint8_t> storage(size); auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(SUCCEEDED(messages->GetMessage(i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) ADD_FAILURE()<<message->pDescription;
    }
#endif
}
