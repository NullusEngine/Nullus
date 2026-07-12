#pragma once

#include "Assets/AssetThumbnailCache.h"
#include "Serialize/ObjectGraphInstantiator.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Math/Vector3.h>

namespace NLS::Render::Context
{
class Driver;
struct PostSubmitTextureReadbackState;
}

namespace NLS::Render::RHI
{
class RHICompletionToken;
}

namespace NLS::Core::ResourceManagement
{
class MaterialManager;
class MeshManager;
class ShaderManager;
}

namespace NLS::Render::Resources
{
class Material;
}

namespace NLS::Editor::Assets
{
struct PreviewRenderableSnapshot;

struct EditorThumbnailPreviewResult
{
    std::vector<uint8_t> rgbaPixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
    std::string diagnostic;
    bool completedPendingReadback = false;
    uint64_t rawVisibleDrawCount = 0u;
    uint64_t submittedSceneDrawCount = 0u;
};

struct EditorThumbnailPreviewResourcePumpResult
{
    bool supported = false;
    bool resourcesPending = false;
    std::string diagnostic;
};

enum class EditorThumbnailPreviewReadbackPollStatus
{
    Missing,
    Pending,
    Ready,
    Superseded,
    Failed,
    DeviceLost
};

struct EditorThumbnailPreviewReadbackState
{
    bool active = false;
    std::string requestKey;
    std::shared_ptr<std::vector<uint8_t>> rgbaPixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t rawVisibleDrawCount = 0u;
    uint64_t submittedSceneDrawCount = 0u;
    std::shared_ptr<NLS::Render::RHI::RHICompletionToken> completion;
    std::shared_ptr<NLS::Render::Context::PostSubmitTextureReadbackState> postSubmitTextureReadbackState;
    std::shared_ptr<void> renderInputsKeepAlive;
};

struct EditorThumbnailPreviewReadbackPollResult
{
    EditorThumbnailPreviewReadbackPollStatus status = EditorThumbnailPreviewReadbackPollStatus::Missing;
    EditorThumbnailPreviewResult preview;
};

EditorThumbnailPreviewReadbackPollResult PollEditorThumbnailPreviewReadback(
    EditorThumbnailPreviewReadbackState& state,
    const std::string& requestKey,
    const NLS::Render::Context::Driver* driver = nullptr);

NLS::Engine::Serialize::LoadPolicy BuildEditorThumbnailPreviewLoadPolicy();

#if defined(NLS_ENABLE_TEST_HOOKS)
struct EditorThumbnailPreviewCameraDebugInfo
{
    NLS::Maths::Vector3 cameraPosition;
    NLS::Maths::Vector3 lookDirection;
    float distance = 0.0f;
};

EditorThumbnailPreviewCameraDebugInfo BuildPrefabPreviewCameraDebugInfoForTesting(
    const NLS::Maths::Vector3& boundsMin,
    const NLS::Maths::Vector3& boundsMax,
    uint32_t width,
    uint32_t height);

NLS::Maths::Vector3 GetThumbnailPreviewKeyLightDirectionForTesting();
float GetThumbnailPreviewKeyLightIntensityForTesting();
size_t GetThumbnailPreviewKeyLightSampleCountForTesting();
float GetThumbnailPreviewKeyLightAngularRadiusDegreesForTesting();
float GetThumbnailPreviewKeyLightSampleIntensitySumForTesting();
float GetThumbnailPreviewAmbientIntensityForTesting();
size_t GetThumbnailPreviewMeshPumpBudgetForTesting();
size_t GetThumbnailPreviewMaterialPumpBudgetForTesting();
size_t GetThumbnailPreviewTexturePumpBudgetForTesting();
size_t GetThumbnailPreviewPrefabResourceInspectionBudgetForTesting();
size_t GetThumbnailPreviewPrefabDrawItemCapacityForTesting();
std::string BuildThumbnailPreviewReadbackRequestKeyForTesting(const AssetThumbnailRequest& request);
bool ThumbnailPreviewMeshPathUsesArtifactLoaderForTesting(const std::string& meshPath);
std::string ResolveThumbnailPreviewMeshLoadPathForTesting(
    const AssetThumbnailRequest& request,
    const std::string& meshPath,
    NLS::Core::Assets::AssetId meshAssetId);
struct ThumbnailPreviewDefaultShaderSelectionForTesting
{
    std::string resourcePath;
    std::string sourcePath;
    std::string subAssetKey;
    std::string lightMode;
    bool usesShaderLabStandardPbrForward = false;
    bool usesLegacyBuiltInStandardHlsl = false;
};
ThumbnailPreviewDefaultShaderSelectionForTesting SelectThumbnailPreviewDefaultShaderForTesting(
    NLS::Core::ResourceManagement::ShaderManager& shaderManager);
bool ThumbnailPreviewSnapshotIsCompleteForGpuPrefabPreviewForTesting(
    const PreviewRenderableSnapshot& snapshot);
bool ShouldDeferPrefabPreviewForMeshReadinessForTesting(
    size_t pendingMeshResourceCount,
    bool resourcePlanTruncated);
bool BindReadyMaterialPreviewTexturesForTesting(NLS::Render::Resources::Material& material);
std::unique_ptr<NLS::Render::Resources::Material> CreateStablePreviewMaterialForTesting(
    NLS::Render::Resources::Material& source);
struct ThumbnailPreviewRenderStatsForTesting
{
    uint64_t rawVisibleDrawCount = 0u;
    uint64_t submittedSceneDrawCount = 0u;
};
ThumbnailPreviewRenderStatsForTesting GetLastThumbnailPreviewRenderStatsForTesting();
struct ThumbnailPreviewPrefabResourcePlanForTesting
{
    size_t drawItemCount = 0u;
    size_t uniqueMeshLoadPathCount = 0u;
    size_t uniqueMaterialLoadPathCount = 0u;
    bool truncatedForPendingResources = false;
};
	ThumbnailPreviewPrefabResourcePlanForTesting BuildThumbnailPreviewPrefabResourcePlanForTesting(
	    const AssetThumbnailRequest& request,
	    const PreviewRenderableSnapshot& snapshot,
	    size_t maxUnreadyDependencyAttempts = SIZE_MAX);
	ThumbnailPreviewPrefabResourcePlanForTesting BuildThumbnailPreviewPrefabResourcePlanWithManagersForTesting(
	    const AssetThumbnailRequest& request,
	    const PreviewRenderableSnapshot& snapshot,
	    NLS::Core::ResourceManagement::MeshManager& meshManager,
	    NLS::Core::ResourceManagement::MaterialManager& materialManager,
	    size_t maxUnreadyDependencyAttempts = SIZE_MAX);
	#endif

class EditorThumbnailPreviewRenderer
{
public:
    explicit EditorThumbnailPreviewRenderer(NLS::Render::Context::Driver& driver);
    ~EditorThumbnailPreviewRenderer();

    EditorThumbnailPreviewRenderer(const EditorThumbnailPreviewRenderer&) = delete;
    EditorThumbnailPreviewRenderer& operator=(const EditorThumbnailPreviewRenderer&) = delete;

    bool Supports(const AssetThumbnailRequest& request) const;
    bool PrewarmMaterialPreviewRenderPath(uint32_t requestedSize);
    EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request);
    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

class IEditorThumbnailPreviewRenderer
{
public:
    virtual ~IEditorThumbnailPreviewRenderer() = default;

    virtual bool Supports(const AssetThumbnailRequest& request) const = 0;
    virtual EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request);
    virtual EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) = 0;
};

class EditorThumbnailPreviewRendererAdapter final : public IEditorThumbnailPreviewRenderer
{
public:
    explicit EditorThumbnailPreviewRendererAdapter(EditorThumbnailPreviewRenderer& renderer);

    bool Supports(const AssetThumbnailRequest& request) const override;
    EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) override;

private:
    EditorThumbnailPreviewRenderer& m_renderer;
};
}
