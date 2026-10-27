#include "SkinnedMeshRenderProxy.h"
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Geometry/BoundingSphereUtils.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Shader.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <algorithm>
#include <cstring>

namespace NLS::Render::AnimationRendering
{
namespace
{
class GeometryMesh final:public RHI::RHIMesh
{
public:
    std::shared_ptr<RHI::RHIBuffer> vertices,indices;
    uint32_t vertexCount=0,indexCount=0;
    std::shared_ptr<RHI::RHIBuffer> GetVertexBuffer()const override{return vertices;}
    std::shared_ptr<RHI::RHIBuffer> GetIndexBuffer()const override{return indices;}
    uint32_t GetVertexCount()const override{return vertexCount;}
    uint32_t GetIndexCount()const override{return indexCount;}
    Settings::EPrimitiveMode GetPrimitiveMode()const override{return Settings::EPrimitiveMode::TRIANGLES;}
    uint32_t GetVertexStride()const override{return sizeof(Animation::Vertex);}
    RHI::IndexType GetIndexType()const override{return RHI::IndexType::UInt32;}
};
class FrameGeometry final:public RHI::RHIMesh
{
public:
    std::shared_ptr<RHI::RHIMesh> geometry;
    std::shared_ptr<const SkinningFrame> frame;
    std::shared_ptr<RHI::RHIBuffer> GetVertexBuffer()const override{return geometry->GetVertexBuffer();}
    std::shared_ptr<RHI::RHIBuffer> GetIndexBuffer()const override{return geometry->GetIndexBuffer();}
    uint32_t GetVertexCount()const override{return geometry->GetVertexCount();}
    uint32_t GetIndexCount()const override{return geometry->GetIndexCount();}
    Settings::EPrimitiveMode GetPrimitiveMode()const override{return geometry->GetPrimitiveMode();}
    uint32_t GetVertexStride()const override{return geometry->GetVertexStride();}
    RHI::IndexType GetIndexType()const override{return geometry->GetIndexType();}
    void NotifySubmitted(uint64_t frameId)const override{frame->NotifySubmitted(frameId);}
};
std::shared_ptr<RHI::RHIBuffer> Upload(const std::shared_ptr<RHI::RHIDevice>& device,const void* data,size_t size,RHI::BufferUsageFlags usage,const char* name)
{
    RHI::RHIBufferDesc desc;desc.size=size;desc.usage=usage;desc.memoryUsage=RHI::MemoryUsage::CPUToGPU;desc.debugName=name;
    RHI::RHIBufferUploadDesc upload;upload.data=data;upload.dataSize=size;upload.debugName=name;
    return device->CreateBuffer(desc,upload);
}
}
void SkinningFrame::NotifySubmitted(uint64_t frameId)const
{
    if(!submittedHistory)return;std::lock_guard lock(submittedHistory->mutex);
    if(frameId<=submittedHistory->frameId)return;
    const bool hasPrevious=submittedHistory->generation==historyGeneration&&submittedHistory->pose;
    previousPose=hasPrevious?submittedHistory->pose:pose;
    previousPalette=hasPrevious?submittedHistory->palette:palette;
    submittedHistory->frameId=frameId;submittedHistory->generation=historyGeneration;submittedHistory->pose=pose;submittedHistory->palette=palette;
}
std::shared_ptr<Resources::Material> SkinningFrame::ResolveMaterial(const Resources::Material& source)const
{
    const auto key=std::to_string(source.GetInstanceId())+":"+std::to_string(source.GetParameterRevision())+":"+std::to_string(source.GetRenderStateRevision())+":"+std::to_string(source.GetBindingRevision());
    std::lock_guard lock(mutex);if(auto found=materials.find(key);found!=materials.end())return found->second;
    std::shared_ptr<Resources::Material> copy=source.CreateDynamicCopy();copy->EnableKeyword("NLS_SKINNED");
    if(const auto* shader=source.GetShader();shader&&std::filesystem::path(shader->path).extension()==".hlsl")
    {
        static std::mutex compilerMutex;std::lock_guard compilerLock(compilerMutex);
        const auto target=device&&device->GetNativeDeviceInfo().backend==RHI::NativeBackendType::Vulkan?ShaderCompiler::ShaderTargetPlatform::SPIRV:ShaderCompiler::ShaderTargetPlatform::DXIL;
        Resources::Loaders::ShaderLoader::EnsureBuiltInHlslVariant(*const_cast<Resources::Shader*>(shader),shader->path,
            NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot(),target,copy->GetShaderLabKeywords().Hash(),ShaderLab::BuildShaderLabKeywordMacros(copy->GetShaderLabKeywords()));
    }
    copy->SetExternalBuffer("u_SkinningPalette",palette);materials.emplace(key,copy);return copy;
}
bool SkinnedMeshRenderProxy::Initialize(std::shared_ptr<RHI::RHIDevice> device,std::shared_ptr<const Animation::Model> model,size_t mesh,std::string& error)
{
    if(!device||!model||mesh>=model->meshes.size()||!model->Validate(error)){if(error.empty())error="Skinned render proxy requires a valid device, model and mesh";return false;}
    const auto& source=model->meshes[mesh];auto binding=std::find_if(model->skins.begin(),model->skins.end(),[&](const auto& b){return b.id==source.skin;});
    if(binding==model->skins.end()||source.vertices.empty()||source.indices.empty()){error="Skinned geometry or binding is empty";return false;}
    auto geometry=std::make_shared<GeometryMesh>();geometry->vertexCount=uint32_t(source.vertices.size());geometry->indexCount=uint32_t(source.indices.size());
    geometry->vertices=Upload(device,source.vertices.data(),source.vertices.size()*sizeof(Animation::Vertex),RHI::BufferUsageFlags::Vertex,"AnimationVertices");
    geometry->indices=Upload(device,source.indices.data(),source.indices.size()*sizeof(uint32_t),RHI::BufferUsageFlags::Index,"AnimationIndices");
    if(!geometry->vertices||!geometry->indices){error="Skinned geometry upload failed";return false;}
    m_skin=size_t(binding-model->skins.begin());m_mesh=mesh;m_device=std::move(device);m_model=std::move(model);m_geometry=std::move(geometry);return true;
}
std::shared_ptr<Resources::Mesh> SkinnedMeshRenderProxy::Publish(std::shared_ptr<const Animation::AnimationFrameResult> pose,std::string& error)
{
    if(!m_geometry||!pose){error="Skinned geometry and pose must be initialized";return {};}
    if(!pose->skeletonSignature.empty()&&pose->skeletonSignature!=m_model->skeleton.Signature())
    {error="Animation pose is incompatible with the mesh skeleton";return {};}
    Animation::SkinningPalette palette;if(!Animation::BuildPalette(pose->model,m_model->skins[m_skin],palette,error))return {};
    std::vector<Maths::Matrix4> packed;packed.reserve(palette.position.size()*2);
    for(size_t i=0;i<palette.position.size();++i){packed.push_back(palette.position[i]);packed.push_back(palette.normal[i]);}
    auto frame=std::make_shared<SkinningFrame>();frame->palette=Upload(m_device,packed.data(),packed.size()*sizeof(Maths::Matrix4),RHI::BufferUsageFlags::ShaderRead,"AnimationPalette");
    if(!frame->palette){error="Skinning palette upload failed";return {};}
    frame->revision=pose->revision;frame->historyGeneration=pose->historyGeneration;frame->pose=std::move(pose);frame->asset=m_model;
    frame->device=m_device;
    frame->submittedHistory=m_history;
    // Reference deformation also supplies exact dynamic bounds; a later budget
    // implementation can replace this with conservative per-bone bounds.
    std::vector<Animation::Vertex> skinned;Animation::SkinVertices(m_model->meshes[m_mesh].vertices,palette,skinned);
    std::vector<Geometry::Vertex> positions(skinned.size());for(size_t i=0;i<skinned.size();++i)std::copy_n(skinned[i].position,3,positions[i].position);
    const auto bounds=Geometry::ComputeBounds(positions);const auto sphere=Geometry::ComputeBoundingSphere(positions);
    auto geometry=std::make_shared<FrameGeometry>();geometry->geometry=m_geometry;geometry->frame=frame;
    return std::make_shared<Resources::Mesh>(std::move(geometry),m_model->meshes[m_mesh].material,bounds,sphere,std::move(frame));
}
}
