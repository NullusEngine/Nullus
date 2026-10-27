#include <gtest/gtest.h>
#include "Rendering/SceneVisibilityPipeline.h"
#include "Rendering/SceneSpatialIndex.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/Data/Frustum.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Resources/Mesh.h"

namespace
{
namespace R=NLS::Render;namespace G=R::RHI;
class Texture final:public G::RHITexture
{
public:
    G::RHITextureDesc desc;
    std::string_view GetDebugName()const override{return "MaterialHybridTexture";}
    const G::RHITextureDesc& GetDesc()const override{return desc;}
    G::ResourceState GetState()const override{return G::ResourceState::Unknown;}
};
class View final:public G::RHITextureView
{
public:
    G::RHITextureViewDesc desc;std::shared_ptr<G::RHITexture> texture;
    std::string_view GetDebugName()const override{return "MaterialHybridView";}
    const G::RHITextureViewDesc& GetDesc()const override{return desc;}
    const std::shared_ptr<G::RHITexture>& GetTexture()const override{return texture;}
};
std::shared_ptr<View> MakeView(bool depth=false)
{
    auto texture=std::make_shared<Texture>();texture->desc.extent={320,180,1};
    texture->desc.format=depth?G::TextureFormat::Depth24Stencil8:G::TextureFormat::RGBA8;
    texture->desc.usage=depth?R::FrameGraph::kDeferredGBufferDepthUsage:R::FrameGraph::kDeferredGBufferColorUsage;
    auto view=std::make_shared<View>();view->texture=texture;view->desc.format=texture->desc.format;return view;
}
}

TEST(MaterialVisibilityTests, HybridPassesKeepThreeGbuffersAndSharedDepthBeforeTransparent)
{
    namespace F=R::FrameGraph;using Kind=R::Context::RenderPassCommandKind;
    R::Context::RenderScenePackage package;package.renderWidth=320;package.renderHeight=180;package.targetsSwapchain=true;
    package.opaqueDrawCount=1;package.transparentDrawCount=1;package.recordedDrawCommands.resize(3);
    for(const auto kind:{Kind::ForwardOpaqueDepth,Kind::ForwardOpaqueColor})
    {R::Context::RenderPassCommandInput input;input.kind=kind;input.drawCount=1;input.recordedDrawCommands.resize(1);package.passCommandInputs.push_back(input);}
    F::DeferredPreparedSceneResources resources;
    for(int i=0;i<3;++i){auto view=MakeView();resources.gbufferColorViews.push_back(view);resources.gbufferTextures.push_back(view->texture);}
    resources.gbufferDepthView=MakeView(true);resources.gbufferTextures.push_back(resources.gbufferDepthView->GetTexture());
    F::LightGridCompileContext lightGrid;lightGrid.frameDescriptor.renderWidth=320;lightGrid.frameDescriptor.renderHeight=180;
    F::DeferredPreparedQueuedDrawCounts counts;counts.lightingDrawCount=1;counts.decalDrawCount=0;counts.transparentDrawCount=1;
    ASSERT_NO_THROW(F::CompileAndApplyPreparedDeferredLightGridSceneExecution(package,lightGrid,resources,{}, {},counts));
    ASSERT_EQ(package.passCommandInputs.size(),5);
    const auto& passes=package.passCommandInputs;
    EXPECT_EQ(passes[0].kind,Kind::GBuffer);EXPECT_EQ(passes[0].colorAttachmentViews.size(),3);
    EXPECT_EQ(passes[1].kind,Kind::ForwardOpaqueDepth);EXPECT_FALSE(passes[1].usesColorAttachment);
    EXPECT_TRUE(passes[1].writesDepthStencilAttachment);EXPECT_FALSE(passes[1].clearDepth);EXPECT_FALSE(passes[1].clearStencil);
    EXPECT_EQ(passes[2].kind,Kind::Lighting);EXPECT_EQ(passes[3].kind,Kind::ForwardOpaqueColor);EXPECT_EQ(passes[4].kind,Kind::Transparent);
    for(size_t i=1;i<5;++i)EXPECT_EQ(passes[i].depthStencilAttachmentView,resources.gbufferDepthView);
    EXPECT_FALSE(passes[3].writesDepthStencilAttachment);EXPECT_FALSE(passes[3].clearColor);
}

TEST(MaterialVisibilityTests, WorldDisplacementExpandsSpatialFrustumAndHzbBoundsWithoutDisablingCulling)
{
    using namespace NLS::Engine::Rendering;
    NLS::Render::Data::Frustum frustum;
    const auto view=NLS::Maths::Matrix4::CreateView(0,0,0,0,0,-1,0,1,0);
    const auto projection=NLS::Maths::Matrix4::CreatePerspective(90,1,0.1f,100);
    frustum.CalculateFrustum(projection*view);
    ScenePrimitiveSnapshot snapshot;snapshot.sceneId=7;snapshot.snapshotSerial=1;
    ScenePrimitiveSnapshotRecord record;record.handle={7,0,1};record.modelBounds.size={1,1,1};
    // Visibility consumes the immutable bounds record; no mesh methods run.
    record.mesh=reinterpret_cast<NLS::Render::Resources::Mesh*>(uintptr_t{1});record.commandOffsetEnd=1;
    record.worldMatrix=NLS::Maths::Matrix4::Translation({7,0,-5});
    record.frustumBehaviour=NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MESHES;
    record.occupied=record.ownerAlive=record.ownerActive=record.hasMeshBinding=record.hasValidMaterial=true;
    record.depthWriteEligibleForOcclusion=true;
    snapshot.primitiveRecords={record};snapshot.denseIndexToHandle={record.handle};snapshot.handleToDenseIndex={{record.handle,0}};
    SceneSpatialIndex index;std::vector<SceneSpatialIndexPrimitiveMetadata> metadata={{record.handle,SceneSpatialIndexPrimitiveClass::Static,true}};
    index.Update(snapshot,metadata);
    SceneVisibilityPipelineOptions options;options.frustum=&frustum;options.enableSpatialIndex=true;
    EXPECT_EQ(SceneVisibilityPipeline::Evaluate(options,snapshot,index).visiblePrimitiveCount,0);
    const auto oldHzb=SceneOcclusionSystem::BuildHZBPrimitivePacketSources(snapshot,{record.handle});ASSERT_EQ(oldHzb.sources.size(),1);
    snapshot.snapshotSerial++;snapshot.primitiveRecords[0].maximumWorldDisplacement=3;
    snapshot.dirtySyncHandles={record.handle};index.Update(snapshot,metadata);
    EXPECT_EQ(SceneVisibilityPipeline::Evaluate(options,snapshot,index).visiblePrimitiveCount,1);
    EXPECT_EQ(snapshot.primitiveRecords[0].frustumBehaviour,NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MESHES);
    const auto newHzb=SceneOcclusionSystem::BuildHZBPrimitivePacketSources(snapshot,{record.handle});ASSERT_EQ(newHzb.sources.size(),1);
    EXPECT_NE(oldHzb.sources[0].primitive.boundsGeneration,newHzb.sources[0].primitive.boundsGeneration);
    EXPECT_FLOAT_EQ(newHzb.sources[0].maximumWorldDisplacement,3);
    snapshot.snapshotSerial++;snapshot.primitiveRecords[0].worldMatrix=NLS::Maths::Matrix4::Translation({50,0,-5});index.Update(snapshot,metadata);
    EXPECT_EQ(SceneVisibilityPipeline::Evaluate(options,snapshot,index).visiblePrimitiveCount,0);
}
