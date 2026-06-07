#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NLS::Render::Resources
{
    class Mesh;
}

namespace NLS::Render::Assets
{
    struct MeshArtifactData;
}

namespace NLS::Editor::Core
{
    struct PrefabInstanceMeshArtifactLoadState
    {
        std::mutex mutex;
        bool completed = false;
        bool accepted = true;
        bool failed = false;
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<const NLS::Render::Assets::MeshArtifactData> data;
        std::shared_ptr<NLS::Render::Resources::Mesh> transientMesh;
    };

    struct RendererResourcePrewarmRequest
    {
        std::string ownerToken;
        std::unordered_set<std::string> prewarmedResources;
        std::unordered_map<std::string, std::shared_ptr<PrefabInstanceMeshArtifactLoadState>> meshLoadsByPath;
        std::unordered_set<std::string> materialLoadsByPath;
        std::unordered_set<std::string> textureLoadsByPath;
    };

    struct PrefabInstancePreviewResourceHandoff
    {
        RendererResourcePrewarmRequest prewarm;
    };

    inline PrefabInstancePreviewResourceHandoff CollectPrefabInstancePreviewResourceHandoff(
        RendererResourcePrewarmRequest& request)
    {
        PrefabInstancePreviewResourceHandoff handoff;
        handoff.prewarm = std::move(request);
        request = {};
        return handoff;
    }

    inline PrefabInstancePreviewResourceHandoff CollectPrefabInstancePreviewResourceHandoff(
        RendererResourcePrewarmRequest&& request)
    {
        return CollectPrefabInstancePreviewResourceHandoff(request);
    }
}
