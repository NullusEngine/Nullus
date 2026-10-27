#pragma once
#include <Animation/Graph.h>
#include <Rendering/RHI/Core/RHIMesh.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Geometry/Bounds.h>
#include <mutex>
namespace NLS::Render::Resources {class Mesh;}

namespace NLS::Render::AnimationRendering
{
struct SkinningSubmissionHistory
{
    std::mutex mutex;
    uint64_t frameId=0,generation=0;
    std::shared_ptr<const Animation::AnimationFrameResult> pose;
    std::shared_ptr<RHI::RHIBuffer> palette;
};
struct NLS_RENDER_API SkinningFrame
{
    uint64_t revision=0,historyGeneration=0;
    std::shared_ptr<RHI::RHIBuffer> palette;
    std::shared_ptr<RHI::RHIDevice> device;
    std::shared_ptr<const Animation::AnimationFrameResult> pose;
    std::shared_ptr<const Animation::Model> asset;
    std::shared_ptr<SkinningSubmissionHistory> submittedHistory;
    mutable std::shared_ptr<const Animation::AnimationFrameResult> previousPose;
    mutable std::shared_ptr<RHI::RHIBuffer> previousPalette;
    void NotifySubmitted(uint64_t frameId)const;
    std::shared_ptr<Resources::Material> ResolveMaterial(const Resources::Material& source)const;
private:
    mutable std::mutex mutex;
    mutable std::map<std::string,std::shared_ptr<Resources::Material>> materials;
};
class NLS_RENDER_API SkinnedMeshRenderProxy
{
public:
    bool Initialize(std::shared_ptr<RHI::RHIDevice> device,std::shared_ptr<const Animation::Model> model,size_t mesh,std::string& error);
    std::shared_ptr<Resources::Mesh> Publish(std::shared_ptr<const Animation::AnimationFrameResult> frame,std::string& error);
private:
    std::shared_ptr<RHI::RHIDevice> m_device;
    std::shared_ptr<const Animation::Model> m_model;
    std::shared_ptr<RHI::RHIMesh> m_geometry;
    size_t m_mesh=0,m_skin=0;
    std::shared_ptr<SkinningSubmissionHistory> m_history=std::make_shared<SkinningSubmissionHistory>();
};
}
