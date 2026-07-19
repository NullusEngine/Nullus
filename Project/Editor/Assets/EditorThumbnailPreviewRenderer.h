#pragma once

#include "Assets/AssetThumbnail.h"
#include "Assets/AssetThumbnailCache.h"
#include "Serialize/ObjectGraphInstantiator.h"

#include <cstddef>
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
    ThumbnailRenderStatus status = ThumbnailRenderStatus::NotReady;
    std::vector<uint8_t> rgbaPixels;
    AssetThumbnailGpuTexture gpuTexture;
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

struct EditorThumbnailPreviewReuseStats
{
    uint64_t previewSceneUseCount = 0u;
    uint64_t renderTargetAllocationCount = 0u;
    uint64_t renderTargetReuseCount = 0u;
    size_t renderTargetPoolSize = 0u;
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
    AssetThumbnailGpuTexture gpuTexture;
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
EditorThumbnailPreviewCameraDebugInfo BuildMeshPreviewCameraDebugInfoForTesting(
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
size_t GetThumbnailPreviewPrefabMeshRequestStartBudgetForTesting();
size_t GetThumbnailPreviewPrefabMeshPumpBudgetForTesting();
size_t GetThumbnailPreviewMaterialPumpBudgetForTesting();
size_t GetThumbnailPreviewTexturePumpBudgetForTesting();
size_t GetThumbnailPreviewPrefabTexturePumpBudgetForTesting();
size_t GetThumbnailPreviewPrefabResourceInspectionBudgetForTesting();
uint64_t GetThumbnailPreviewPrefabResourcePumpTimeBudgetMicrosForTesting();
size_t GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting();
size_t GetThumbnailPreviewPrefabDrawItemCapacityForTesting();
size_t GetThumbnailPreviewPrefabProxyDrawItemCapacityForTesting();
size_t GetThumbnailPreviewPrefabProxyCandidateDrawItemCapacityForTesting();
std::filesystem::path BuildThumbnailPreviewPrefabProxyArtifactPathForTesting(
    const AssetThumbnailRequest& request);
std::optional<std::filesystem::path> BuildThumbnailPreviewPrefabProxyForTesting(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot);
struct ThumbnailPreviewPrefabProxyDetailsForTesting
{
    std::vector<std::filesystem::path> meshPaths;
    std::vector<std::string> materialPaths;
};
std::optional<ThumbnailPreviewPrefabProxyDetailsForTesting>
BuildThumbnailPreviewPrefabProxyDetailsForTesting(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot);
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
bool ShouldDeferPrefabPreviewForResourceReadinessForTesting(
    size_t pendingMeshResourceCount,
    size_t pendingMaterialResourceCount,
    size_t pendingMaterialTextureCount,
    bool resourcePlanTruncated);
bool ShouldDeferPrefabPreviewAfterDrawPrewarmForTesting(
    bool prewarmSupported,
    bool prewarmComplete);
bool ShouldWaitForPersistentPrefabPreviewProxyForTesting(
    bool usesProvisionalPlan,
    bool persistentProxyReady);
bool ShouldUseFullSourceBoundsForPrefabCameraForTesting(bool usesProvisionalPlan);
bool ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
    const std::string& diagnostic);
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
	size_t dependencyDrawItemInspectionCount = 0u;
	bool truncatedForPendingResources = false;
	std::vector<size_t> selectedDrawItemIndices;
	NLS::Maths::Vector3 fullWorldBoundsMin {};
	NLS::Maths::Vector3 fullWorldBoundsMax {};
	bool hasFullWorldBounds = false;
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

class IEditorThumbnailPreviewRenderer
{
public:
    virtual ~IEditorThumbnailPreviewRenderer() = default;

    virtual bool Supports(const AssetThumbnailRequest& request) const = 0;
    virtual EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request);
    virtual EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) = 0;
};

class EditorThumbnailPreviewRenderer final : public IEditorThumbnailPreviewRenderer
{
public:
    explicit EditorThumbnailPreviewRenderer(NLS::Render::Context::Driver& driver);
    ~EditorThumbnailPreviewRenderer();

    EditorThumbnailPreviewRenderer(const EditorThumbnailPreviewRenderer&) = delete;
    EditorThumbnailPreviewRenderer& operator=(const EditorThumbnailPreviewRenderer&) = delete;

    bool Supports(const AssetThumbnailRequest& request) const override;
    bool PrewarmMaterialPreviewRenderPath(uint32_t requestedSize);
    EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) override;
    /// Returns lifetime reuse counters for the persistent preview scene and render-target pool.
    [[nodiscard]] EditorThumbnailPreviewReuseStats GetReuseStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
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
