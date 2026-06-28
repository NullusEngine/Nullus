#include <Rendering/Data/LightingDescriptor.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Math/Vector2.h"
#include "Math/Vector4.h"
#include "Debug/Logger.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/LightGridPrepass.h"
#include "Rendering/SceneLightingProvider.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/SceneVisibilityPipeline.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/Settings/EngineDiagnosticsSettings.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Profiling/Profiler.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine::Rendering
{
namespace
{
	template <typename Compare>
	void SortSceneDrawables(BaseSceneRenderer::SceneDrawables& drawables, Compare compare)
	{
		std::stable_sort(
			drawables.begin(),
			drawables.end(),
			[compare](const auto& lhs, const auto& rhs)
			{
				return compare(lhs.first, rhs.first);
			});
	}

	struct LoadedSceneFallbackShader
	{
		NLS::Render::Resources::Shader* shader = nullptr;
		std::string resourcePath;
	};

	std::string ToLowerGenericPath(std::string path)
	{
		path = std::filesystem::path(path).generic_string();
		std::transform(path.begin(), path.end(), path.begin(), [](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
		return path;
	}

	std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
		return value;
	}

	bool IsStandardPbrSourcePath(const std::string& path)
	{
		const auto sourcePath = ToLowerGenericPath(path);
		return sourcePath == "app/assets/engine/shaders/shaderlab/standardpbr.shader" ||
			sourcePath.ends_with("/app/assets/engine/shaders/shaderlab/standardpbr.shader") ||
			sourcePath == "assets/engine/shaders/shaderlab/standardpbr.shader" ||
			sourcePath.ends_with("/assets/engine/shaders/shaderlab/standardpbr.shader");
	}

	bool IsStandardPbrForwardSubAssetKey(const std::string& subAssetKey)
	{
		const auto key = ToLowerAscii(subAssetKey);
		return key == "shader:standardpbr" ||
			key == "shader:standardpbr/forward" ||
			key.rfind("shader:standardpbr/forward#", 0u) == 0u;
	}

	bool IsDefaultSceneFallbackShader(const NLS::Render::Resources::Shader& shader)
	{
		if (!shader.GetShaderLabPassState().has_value())
			return false;
		if (!IsStandardPbrSourcePath(shader.GetImportedArtifactSourcePath()))
			return false;
		if (!IsStandardPbrForwardSubAssetKey(shader.GetImportedArtifactSubAssetKey()))
			return false;
		return shader.GetShaderLabLightMode() == "Forward";
	}

	LoadedSceneFallbackShader ResolveLoadedSceneFallbackShader()
	{
		if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
			return {};

		auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
		for (const auto& [resourcePath, shader] : shaderManager.GetResources())
		{
			if (shader != nullptr && IsDefaultSceneFallbackShader(*shader))
				return { shader, resourcePath };
		}

		return {};
	}

		bool IsStandardPbrForwardArtifactRecord(const NLS::Core::Assets::ArtifactDatabaseRecord& record)
		{
			if (record.status != NLS::Core::Assets::ArtifactRecordStatus::UpToDate ||
				record.artifactType != NLS::Core::Assets::ArtifactType::Shader ||
				record.targetPlatform != "editor" ||
				record.artifactPath.empty())
			{
				return false;
		}

		if (!IsStandardPbrSourcePath(record.sourcePath))
			return false;

		return IsStandardPbrForwardSubAssetKey(record.subAssetKey);
	}

	std::filesystem::path ResolveProjectRootFromShaderManager(
		const NLS::Core::ResourceManagement::ShaderManager& shaderManager)
	{
		const auto& projectAssetsPath = shaderManager.ProjectAssetsRoot();
		if (projectAssetsPath.empty())
			return {};

		auto projectAssetsRoot = std::filesystem::path(projectAssetsPath).lexically_normal();
		if (projectAssetsRoot.filename().generic_string().empty())
			projectAssetsRoot = projectAssetsRoot.parent_path();
		if (ToLowerGenericPath(projectAssetsRoot.filename().generic_string()) == "assets")
			return projectAssetsRoot.parent_path();
		return {};
	}

	bool TryLoadDefaultSceneFallbackShaderFromArtifactDatabase(
		NLS::Core::ResourceManagement::ShaderManager& shaderManager)
	{
		const auto projectRoot = ResolveProjectRootFromShaderManager(shaderManager);
		if (projectRoot.empty())
			return false;

		NLS::Core::Assets::ArtifactDatabase database;
		if (!database.Load(projectRoot / "Library" / "ArtifactDB"))
			return false;

		std::vector<std::string> candidateArtifactPaths;
		database.VisitRecords([&](const NLS::Core::Assets::ArtifactDatabaseRecord& record)
		{
			if (IsStandardPbrForwardArtifactRecord(record))
				candidateArtifactPaths.push_back(std::filesystem::path(record.artifactPath).lexically_normal().generic_string());
		});

		for (const auto& artifactPath : candidateArtifactPaths)
		{
			if (auto* shader = shaderManager.GetResource(artifactPath, true);
				shader != nullptr && IsDefaultSceneFallbackShader(*shader))
			{
				return true;
			}
		}

		return false;
	}

	uint64_t ElapsedNanoseconds(const std::chrono::steady_clock::time_point start)
	{
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - start).count();
		return static_cast<uint64_t>(std::max<int64_t>(elapsed, 1));
	}

	Render::Context::LargeSceneCullReasonDebugSnapshot ToFrameCullReasonDebugSnapshot(
		const SceneCullReasonDebugSnapshot& source)
	{
		Render::Context::LargeSceneCullReasonDebugSnapshot target;
		target.frameSerial = source.frameSerial;
		target.sceneId = source.sceneId;
		target.primitiveCount = source.primitiveCount;
		target.visiblePrimitiveCount = source.visiblePrimitiveCount;
		target.reasonCounts = source.reasonCounts;
		target.entries.reserve(source.entries.size());
		for (const auto& sourceEntry : source.entries)
		{
			target.entries.push_back({
				sourceEntry.handle.sceneId,
				sourceEntry.handle.index,
				sourceEntry.handle.generation,
				static_cast<uint8_t>(sourceEntry.reason),
				sourceEntry.selectedLOD,
				sourceEntry.commandOffsetBegin,
				sourceEntry.commandOffsetEnd,
				sourceEntry.visible
			});
		}
		return target;
	}

    class PreparedPassBindingPlaceholder final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        std::string_view GetDebugName() const override { return "PreparedPassBindingPlaceholder"; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc{};
    };

    const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetPreparedPassBindingPlaceholderInstance()
    {
        static const std::shared_ptr<NLS::Render::RHI::RHIBindingSet> kPlaceholder =
            std::make_shared<PreparedPassBindingPlaceholder>();
        return kPlaceholder;
    }

    bool IsPreparedPassBindingPlaceholder(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
    {
        return bindingSet == GetPreparedPassBindingPlaceholderInstance();
    }

    void ResolvePreparedPassBindingPlaceholders(
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands,
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet)
    {
        for (auto& drawCommand : drawCommands)
        {
            if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
                drawCommand.passBindingSet = resolvedBindingSet;
        }
    }

	void ResolvePreparedPassBindingPlaceholders(
		std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands,
		const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet,
		const size_t firstDrawIndex,
        const size_t lastDrawIndex)
    {
        const auto resolvedLastDrawIndex = std::min(lastDrawIndex, drawCommands.size());
        if (firstDrawIndex >= resolvedLastDrawIndex)
            return;

        for (size_t drawIndex = firstDrawIndex; drawIndex < resolvedLastDrawIndex; ++drawIndex)
        {
            auto& drawCommand = drawCommands[drawIndex];
            if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
                drawCommand.passBindingSet = resolvedBindingSet;
        }
    }

    void ClearPreparedPassBindingPlaceholders(
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands)
    {
        for (auto& drawCommand : drawCommands)
        {
			if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
				drawCommand.passBindingSet.reset();
		}
	}

	bool TryLoadOneMissingMaterialTexture(NLS::Render::Resources::Material& material)
	{
		if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
			return false;

		for (const auto& [uniformName, texturePath] : material.GetTextureResourcePaths())
		{
			if (texturePath.empty())
				continue;

			const auto* parameter = material.GetParameterBlock().TryGet(uniformName);
			if (parameter != nullptr &&
				parameter->type() == typeid(NLS::Render::Resources::Texture2D*) &&
				std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter) != nullptr)
			{
				continue;
			}

			auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
			if (auto* texture = textureManager.GetResource(texturePath, false))
			{
				material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
				return true;
			}

			if (auto* texture = textureManager.RequestAsyncArtifact(texturePath))
			{
				material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
				return true;
			}

			return false;
		}

		return false;
	}

	bool PumpOneVisibleMaterialTexture(BaseSceneRenderer::SceneDrawables& drawables)
	{
		for (auto& [_, drawable] : drawables)
		{
			if (drawable.material != nullptr && TryLoadOneMissingMaterialTexture(*drawable.material))
				return true;
		}
		return false;
	}

	void HashCombine(size_t& seed, const size_t value)
	{
		seed ^= value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
	}

	void HashFloat(size_t& seed, const float value)
	{
		uint32_t bits = 0u;
		std::memcpy(&bits, &value, sizeof(bits));
		HashCombine(seed, static_cast<size_t>(bits));
	}

	uint64_t HashMatrix(const Maths::Matrix4& matrix)
	{
		size_t seed = 0u;
		for (const float value : matrix.data)
			HashFloat(seed, value);
		const auto hash = static_cast<uint64_t>(seed);
		return hash != 0u ? hash : 1u;
	}

	uint64_t HashCameraViewCompatibility(const NLS::Render::Entities::Camera& camera)
	{
		size_t seed = 0u;
		HashFloat(seed, camera.GetNear());
		HashFloat(seed, camera.GetFar());
		HashCombine(seed, static_cast<size_t>(camera.GetProjectionMode()));
		const auto hash = static_cast<uint64_t>(seed);
		return hash != 0u ? hash : 1u;
	}

	uint64_t ResolveDepthFormatKey(const NLS::Render::Data::FrameDescriptor& frameDescriptor)
	{
		if (frameDescriptor.outputDepthStencilTexture != nullptr)
			return static_cast<uint64_t>(frameDescriptor.outputDepthStencilTexture->GetDesc().format);
		return static_cast<uint64_t>(NLS::Render::FrameGraph::kDeferredGBufferDepthFormat);
	}

	uint64_t BuildHZBViewKey(const NLS::Render::Data::FrameDescriptor& frameDescriptor)
	{
		size_t seed = 0u;
		HashCombine(seed, static_cast<size_t>(frameDescriptor.renderWidth));
		HashCombine(seed, static_cast<size_t>(frameDescriptor.renderHeight));
		HashCombine(seed, static_cast<size_t>(ResolveDepthFormatKey(frameDescriptor)));
		const auto hash = static_cast<uint64_t>(seed);
		return hash != 0u ? hash : 1u;
	}


	uint64_t BuildRuntimeStreamingDependencyId(const ScenePrimitiveHandle handle)
	{
		size_t seed = 0x4e554c4c55535f53ull;
		HashCombine(seed, static_cast<size_t>(handle.sceneId));
		HashCombine(seed, static_cast<size_t>(handle.index));
		HashCombine(seed, static_cast<size_t>(handle.generation));
		const auto dependencyId = static_cast<uint64_t>(seed);
		return dependencyId != 0u ? dependencyId : 1u;
	}

	void AddUniqueRuntimeStreamingDependencyPin(
		std::vector<uint64_t>& pins,
		std::unordered_set<uint64_t>& seenDependencyIds,
		const ScenePrimitiveHandle handle)
	{
		if (!handle.IsValid())
			return;

		const auto dependencyId = BuildRuntimeStreamingDependencyId(handle);
		if (seenDependencyIds.insert(dependencyId).second)
			pins.push_back(dependencyId);
	}

	std::vector<uint64_t> BuildRuntimeStreamingDependencyPins(
		const std::vector<ScenePrimitiveHandle>& visiblePrimitiveHandles,
		const std::vector<ScenePrimitiveHandle>& representationStreamingInterest)
	{
		std::vector<uint64_t> pins;
		std::unordered_set<uint64_t> seenDependencyIds;
		pins.reserve(visiblePrimitiveHandles.size() + representationStreamingInterest.size());
		seenDependencyIds.reserve(pins.capacity());
		for (const auto handle : representationStreamingInterest)
			AddUniqueRuntimeStreamingDependencyPin(pins, seenDependencyIds, handle);
		for (const auto handle : visiblePrimitiveHandles)
			AddUniqueRuntimeStreamingDependencyPin(pins, seenDependencyIds, handle);
		return pins;
	}

	void AppendUniqueRuntimeStreamingDependencyPins(
		std::vector<uint64_t>& target,
		std::unordered_set<uint64_t>& seenDependencyIds,
		const std::vector<uint64_t>& source)
	{
		for (const auto dependencyId : source)
		{
			if (dependencyId != 0u && seenDependencyIds.insert(dependencyId).second)
				target.push_back(dependencyId);
		}
	}

	void RegisterRuntimeStreamingDependencies(
		SceneStreamingResidency& residency,
		const std::vector<ScenePrimitiveHandle>& visiblePrimitiveHandles,
		const std::vector<ScenePrimitiveHandle>& representationStreamingInterest)
	{
		auto registerPrimitive = [&residency](const ScenePrimitiveHandle handle, const StreamingDependencySource source)
		{
			if (!handle.IsValid())
				return;

			StreamingResourceDependency dependency;
			dependency.dependencyId = BuildRuntimeStreamingDependencyId(handle);
			dependency.source = source;
			dependency.resourceType = StreamingResourceType::Placeholder;
			dependency.artifactId =
				"runtime-primitive:" +
				std::to_string(handle.sceneId) + ":" +
				std::to_string(handle.index) + ":" +
				std::to_string(handle.generation);
			dependency.cpuBytes = 64u;
			dependency.gpuBytes = 128u;
			dependency.ioBytes = 64u;
			dependency.gpuUploadBytes = 128u;
			dependency.cpuCommitUs = 1u;
			dependency.priority = source == StreamingDependencySource::Visibility ? 100u : 50u;
			dependency.requiredForVisibleRepresentation = source == StreamingDependencySource::Visibility;
			residency.RegisterDependency(dependency);
			residency.RegisterPrimitiveDependency(handle, dependency.dependencyId);
		};

		for (const auto handle : representationStreamingInterest)
			registerPrimitive(handle, StreamingDependencySource::HLOD);
		for (const auto handle : visiblePrimitiveHandles)
			registerPrimitive(handle, StreamingDependencySource::Visibility);
	}

	void MergeStreamingTelemetry(
		NLS::Render::Data::LargeSceneTelemetry& target,
		const StreamingResidencyPlan& plan,
		const StreamingCommitResult& commit)
	{
		target.streamingDependencyCount += plan.telemetry.streamingDependencyCount;
		target.streamingRequestCount += plan.telemetry.streamingRequestCount;
		target.streamingCommitCount += commit.telemetry.streamingCommitCount;
		target.streamingEvictCount += commit.telemetry.streamingEvictCount;
		target.residencyTicketCount = commit.telemetry.residencyTicketCount;
		target.requestedCpuBytes = commit.telemetry.requestedCpuBytes;
		target.requestedGpuBytes = commit.telemetry.requestedGpuBytes;
		target.residentCpuBytes = commit.telemetry.residentCpuBytes;
		target.residentGpuBytes = commit.telemetry.residentGpuBytes;
	}

	RepresentationResidencySnapshot BuildRepresentationResidencySnapshotFromStreamingCommit(
		const StreamingResidencyPlan& plan,
		const StreamingCommitResult& commit)
	{
		RepresentationResidencySnapshot snapshot;
		std::unordered_map<uint64_t, const StreamingResourceDependency*> dependencyById;
		dependencyById.reserve(plan.dependencyClosure.size());
		for (const auto& dependency : plan.dependencyClosure)
			dependencyById[dependency.dependencyId] = &dependency;

		std::unordered_map<uint64_t, ResidencyTicketState> stateByDependency;
		stateByDependency.reserve(commit.tickets.size());
		for (const auto& ticket : commit.tickets)
			stateByDependency[ticket.dependencyId] = ticket.state;

		for (const auto& binding : plan.primitiveDependencyBindings)
		{
			const auto stateIt = stateByDependency.find(binding.dependencyId);
			const auto dependencyIt = dependencyById.find(binding.dependencyId);
			if (stateIt == stateByDependency.end() || dependencyIt == dependencyById.end())
				continue;

			const bool resident =
				stateIt->second == ResidencyTicketState::Resident ||
				stateIt->second == ResidencyTicketState::VisibleResident;
			const bool hlodProxy =
				dependencyIt->second->source == StreamingDependencySource::HLOD ||
				dependencyIt->second->resourceType == StreamingResourceType::HLODProxy;
			if (!resident)
			{
				snapshot.MarkNotResident(binding.primitive);
				continue;
			}

			if (hlodProxy)
				snapshot.MarkHLODProxyReady(binding.primitive);
			else
				snapshot.MarkReady(binding.primitive);
		}

		return snapshot;
	}

	void MergeRepresentationResidencySnapshot(
		RepresentationResidencySnapshot& target,
		const RepresentationResidencySnapshot& source)
	{
		for (const auto handle : source.fallbackPrimitiveResources)
			target.MarkFallback(handle);
		for (const auto handle : source.readyPrimitiveResources)
			target.MarkReady(handle);
		for (const auto handle : source.readyHLODProxyResources)
			target.MarkHLODProxyReady(handle);
		for (const auto handle : source.notResidentResources)
			target.MarkNotResident(handle);
	}

}

using namespace Components;
using RenderMaterial = Render::Resources::Material;
using RenderMesh = Render::Resources::Mesh;
using LightingDescriptor = Render::Data::LightingDescriptor;

BaseSceneRenderer::BaseSceneRenderer(Render::Context::Driver& p_driver)
	: Render::Core::CompositeRenderer(p_driver)
{
	SetFrameObjectBindingProvider(std::make_unique<EngineFrameObjectBindingProvider>(*this));
	m_lightGridPrepass = std::make_shared<LightGridPrepass>(p_driver);
	m_sceneLightingProvider = std::make_unique<SceneLightingProvider>();
}

BaseSceneRenderer::~BaseSceneRenderer() = default;

void BaseSceneRenderer::PreloadSceneFallbackShader(NLS::Core::ResourceManagement::ShaderManager& shaderManager)
{
	for (const auto& [resourcePath, shader] : shaderManager.GetResources())
	{
		if (shader != nullptr && IsDefaultSceneFallbackShader(*shader))
			return;
	}

	if (TryLoadDefaultSceneFallbackShaderFromArtifactDatabase(shaderManager))
		return;

	NLS_LOG_WARNING("BaseSceneRenderer has no loaded StandardPBR Forward ShaderLab artifact fallback shader; scene objects without explicit materials may be skipped until a default material or imported shader artifact is loaded.");
}

void BaseSceneRenderer::BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_PROFILE_SCOPE();
	NLS_ASSERT(HasDescriptor<SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");
	InvalidateLightGridCompileContextCache();
	m_hasLastVisiblePickablePrimitiveDrawSources = false;
	m_lastVisiblePickablePrimitiveDrawSources.clear();

	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	RefreshSceneLightingDescriptor(sceneDescriptor.scene);

	Render::Core::CompositeRenderer::BeginFrame(p_frameDescriptor);

	m_lastCullReasonDebugSnapshot = {};
	if (const auto snapshot = BuildFrameSnapshot(p_frameDescriptor); snapshot.has_value())
		SetPendingFrameSnapshot(snapshot.value());
}

std::optional<Render::Context::FrameSnapshot> BaseSceneRenderer::BuildFrameSnapshot(
	const Render::Data::FrameDescriptor& frameDescriptor) const
{
	NLS_PROFILE_SCOPE();
	auto snapshot = Render::Core::ABaseRenderer::BuildFrameSnapshot(frameDescriptor);
	if (!snapshot.has_value())
		return snapshot;

	if (!HasDescriptor<SceneDescriptor>())
		return snapshot;

	const auto& scene = GetDescriptor<SceneDescriptor>().scene;
	const auto& fastAccess = scene.GetFastAccessComponents();
	snapshot->hasSceneInput = true;
	snapshot->sceneGameObjectCount = static_cast<uint64_t>(scene.GetGameObjects().size());
	snapshot->sceneModelRendererCount = static_cast<uint64_t>(fastAccess.modelRenderers.size());
	snapshot->sceneLightCount = static_cast<uint64_t>(fastAccess.lights.size());
	snapshot->sceneSkyboxCount = static_cast<uint64_t>(fastAccess.skyboxs.size());
	snapshot->visibleOpaqueDrawCount = 0u;
	snapshot->visibleDecalDrawCount = 0u;
	snapshot->visibleTransparentDrawCount = 0u;
	snapshot->visibleSkyboxDrawCount = 0u;
	snapshot->visibleHelperDrawCount = 0u;
	snapshot->largeSceneCullReasonSnapshot = m_lastCullReasonDebugSnapshot;
	snapshot->streamingDependencyPins = m_lastStreamingDependencyPins;
	return snapshot;
}

void BaseSceneRenderer::RefreshFrameSnapshotVisibility(
	Render::Context::FrameSnapshot& snapshot,
	const AllDrawables& drawables)
{
	snapshot.visibleOpaqueDrawCount = static_cast<uint64_t>(drawables.opaques.size());
	snapshot.visibleDecalDrawCount = static_cast<uint64_t>(drawables.decals.size());
	snapshot.visibleTransparentDrawCount = static_cast<uint64_t>(drawables.transparents.size());
	snapshot.visibleSkyboxDrawCount = static_cast<uint64_t>(drawables.skyboxes.size());
}

void BaseSceneRenderer::SetLastCullReasonDebugSnapshot(
	const std::shared_ptr<const SceneCullReasonDebugSnapshot>& snapshot) const
{
	if (snapshot == nullptr)
		return;

	auto converted = ToFrameCullReasonDebugSnapshot(*snapshot);
	if (m_lastCullReasonDebugSnapshot.entries.empty())
	{
		m_lastCullReasonDebugSnapshot = std::move(converted);
		return;
	}

	m_lastCullReasonDebugSnapshot.sceneId = 0u;
	m_lastCullReasonDebugSnapshot.primitiveCount += converted.primitiveCount;
	m_lastCullReasonDebugSnapshot.visiblePrimitiveCount += converted.visiblePrimitiveCount;
	for (size_t reasonIndex = 0u; reasonIndex < m_lastCullReasonDebugSnapshot.reasonCounts.size(); ++reasonIndex)
		m_lastCullReasonDebugSnapshot.reasonCounts[reasonIndex] += converted.reasonCounts[reasonIndex];
	m_lastCullReasonDebugSnapshot.entries.insert(
		m_lastCullReasonDebugSnapshot.entries.end(),
		std::make_move_iterator(converted.entries.begin()),
		std::make_move_iterator(converted.entries.end()));
}

bool BaseSceneRenderer::ShouldPublishCullReasonDebugSnapshots() const
{
	return false;
}

uint64_t BaseSceneRenderer::GetCullReasonDebugSnapshotMaxEntries() const
{
	return 0u;
}

const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& BaseSceneRenderer::GetLightGridGraphicsPassBindingSet() const
{
	static const std::shared_ptr<NLS::Render::RHI::RHIBindingSet> kNullBindingSet{};
	return m_lightGridPrepass != nullptr
		? m_lightGridPrepass->GetGraphicsPassBindingSet()
		: kNullBindingSet;
}

NLS::Render::FrameGraph::LightGridCompileContext BaseSceneRenderer::BuildLightGridCompileContext(
	const bool hasSkyboxTexture) const
{
	NLS_PROFILE_SCOPE();
	const auto frameSnapshot =
		NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(GetFrameDescriptor());

	if (!NLS::Render::Context::DriverRendererAccess::IsLightGridEnabled(m_driver))
	{
		if (m_lightGridPrepass != nullptr)
			m_lightGridPrepass->EnsureFallbackGraphicsPassBindingSet(frameSnapshot, hasSkyboxTexture);
		return NLS::Render::FrameGraph::BuildLightGridCompileContext(
			frameSnapshot,
			{},
			GetLightGridGraphicsPassBindingSet());
	}

	std::lock_guard lock(m_lightGridCompileContextCacheMutex);
	if (IsLightGridCompileContextCacheHit(frameSnapshot, hasSkyboxTexture))
		return m_lightGridCompileContextCache.context;

	const auto preparedComputeRequest = LightGridPrepass::BuildPreparedComputeRequest(
		frameSnapshot,
		GetLightGridPrepass(),
		BuildLightGridFrameInputs(hasSkyboxTexture));
	auto preparedComputeSource = LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest);
	if (GetLightGridGraphicsPassBindingSet() == nullptr && m_lightGridPrepass != nullptr)
		m_lightGridPrepass->EnsureFallbackGraphicsPassBindingSet(frameSnapshot, hasSkyboxTexture);
	auto graphicsPassBindingSet = GetLightGridGraphicsPassBindingSet();
	auto context = NLS::Render::FrameGraph::BuildLightGridCompileContext(
		frameSnapshot,
		std::move(preparedComputeSource),
		std::move(graphicsPassBindingSet));

	m_lightGridCompileContextCache.valid = true;
	m_lightGridCompileContextCache.hasSkyboxTexture = hasSkyboxTexture;
	m_lightGridCompileContextCache.frameDescriptor = frameSnapshot;
	if (frameSnapshot.camera != nullptr)
	{
		m_lightGridCompileContextCache.cameraPosition = frameSnapshot.camera->GetPosition();
		m_lightGridCompileContextCache.cameraRotation = frameSnapshot.camera->GetRotation();
	}
	else
	{
		m_lightGridCompileContextCache.cameraPosition = {};
		m_lightGridCompileContextCache.cameraRotation = {};
	}
	m_lightGridCompileContextCache.context = context;
	return context;
}

std::optional<LightGridPrepass::PreparedFrameInputs> BaseSceneRenderer::BuildLightGridFrameInputs(
	const bool hasSkyboxTexture) const
{
	NLS_PROFILE_SCOPE();
	if (m_lightGridPrepass == nullptr || !HasDescriptor<LightingDescriptor>())
		return std::nullopt;

	return LightGridPrepass::CaptureFrameInputs(
		GetDescriptor<LightingDescriptor>(),
		hasSkyboxTexture);
}

const std::shared_ptr<LightGridPrepass>& BaseSceneRenderer::GetLightGridPrepass() const
{
	return m_lightGridPrepass;
}

BaseSceneRenderer::Material* BaseSceneRenderer::ResolveDefaultSceneMaterial()
{
	const auto fallbackShader = ResolveLoadedSceneFallbackShader();
	if (fallbackShader.shader == nullptr)
	{
		m_sceneFallbackMaterial.reset();
		m_sceneFallbackShader = nullptr;
		m_sceneFallbackShaderInstanceId = 0u;
		m_sceneFallbackShaderGeneration = 0u;
		m_sceneFallbackShaderResourcePath.clear();
		return nullptr;
	}

	const auto shaderInstanceId = fallbackShader.shader->GetInstanceId();
	const auto shaderGeneration = fallbackShader.shader->GetGeneration();
	if (!m_sceneFallbackMaterial ||
		m_sceneFallbackShader != fallbackShader.shader ||
		m_sceneFallbackShaderInstanceId != shaderInstanceId ||
		m_sceneFallbackShaderGeneration != shaderGeneration ||
		m_sceneFallbackShaderResourcePath != fallbackShader.resourcePath)
	{
		m_sceneFallbackMaterial = std::make_unique<Render::Resources::Material>();
		m_sceneFallbackMaterial->SetShader(fallbackShader.shader);
		const_cast<std::string&>(m_sceneFallbackMaterial->path) = ":Generated/SceneFallbackMaterial";
		m_sceneFallbackMaterial->SetRawParameter("_BaseColor", Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f));
		m_sceneFallbackMaterial->SetRawParameter("_Metallic", 0.0f);
		m_sceneFallbackMaterial->SetRawParameter("_Roughness", 0.72f);
		m_sceneFallbackMaterial->SetBlendable(false);
		m_sceneFallbackMaterial->SetBackfaceCulling(false);
		m_sceneFallbackMaterial->SetFrontfaceCulling(false);
		m_sceneFallbackMaterial->SetDepthTest(true);
		m_sceneFallbackMaterial->SetDepthWriting(true);
		m_sceneFallbackMaterial->SetColorWriting(true);
		m_sceneFallbackShader = fallbackShader.shader;
		m_sceneFallbackShaderInstanceId = shaderInstanceId;
		m_sceneFallbackShaderGeneration = shaderGeneration;
		m_sceneFallbackShaderResourcePath = fallbackShader.resourcePath;
	}

	return m_sceneFallbackMaterial->IsValid() ? m_sceneFallbackMaterial.get() : nullptr;
}

void BaseSceneRenderer::InvalidateLightGridCompileContextCache() const
{
	std::lock_guard lock(m_lightGridCompileContextCacheMutex);
	m_lightGridCompileContextCache.valid = false;
	m_lightGridCompileContextCache.context = {};
}

bool BaseSceneRenderer::IsLightGridCompileContextCacheHit(
	const NLS::Render::Data::FrameDescriptor& frameDescriptor,
	const bool hasSkyboxTexture) const
{
	return m_lightGridCompileContextCache.valid &&
		m_lightGridCompileContextCache.hasSkyboxTexture == hasSkyboxTexture &&
		AreSameLightGridFrameInputs(m_lightGridCompileContextCache, frameDescriptor);
}

bool BaseSceneRenderer::AreSameLightGridFrameInputs(
	const LightGridCompileContextCache& cached,
	const NLS::Render::Data::FrameDescriptor& current) const
{
	const auto& cachedFrame = cached.frameDescriptor;
	const bool sameCameraTransform =
		current.camera == nullptr ||
		(Maths::Vector3::Distance(cached.cameraPosition, current.camera->GetPosition()) <= 1e-5f &&
			std::fabs(cached.cameraRotation.x - current.camera->GetRotation().x) <= 1e-5f &&
			std::fabs(cached.cameraRotation.y - current.camera->GetRotation().y) <= 1e-5f &&
			std::fabs(cached.cameraRotation.z - current.camera->GetRotation().z) <= 1e-5f &&
			std::fabs(cached.cameraRotation.w - current.camera->GetRotation().w) <= 1e-5f);

	return cachedFrame.renderWidth == current.renderWidth &&
		cachedFrame.renderHeight == current.renderHeight &&
		cachedFrame.camera == current.camera &&
		sameCameraTransform &&
		cachedFrame.outputBuffer == current.outputBuffer &&
		cachedFrame.outputColorTexture == current.outputColorTexture &&
		cachedFrame.outputDepthStencilTexture == current.outputDepthStencilTexture &&
		cachedFrame.outputColorView == current.outputColorView &&
		cachedFrame.outputDepthStencilView == current.outputDepthStencilView;
}

const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder()
{
	return GetPreparedPassBindingPlaceholderInstance();
}

void BaseSceneRenderer::ResolvePreparedPassBindingSetPlaceholders(
	Render::Context::RenderScenePackage& package,
	const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet)
{
	ResolvePreparedPassBindingPlaceholders(package.recordedDrawCommands, resolvedBindingSet);
	for (auto& passInput : package.passCommandInputs)
		ResolvePreparedPassBindingPlaceholders(passInput.recordedDrawCommands, resolvedBindingSet);
}

void BaseSceneRenderer::ResolvePreparedScenePassBindingSetPlaceholders(
	Render::Context::RenderScenePackage& package,
	const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet,
	const uint64_t sceneDrawCount)
{
	const auto sceneDrawEnd = static_cast<size_t>(
		std::min<uint64_t>(
			sceneDrawCount,
			static_cast<uint64_t>(package.recordedDrawCommands.size())));
	ResolvePreparedPassBindingPlaceholders(package.recordedDrawCommands, resolvedBindingSet, 0u, sceneDrawEnd);
	for (size_t drawIndex = sceneDrawEnd; drawIndex < package.recordedDrawCommands.size(); ++drawIndex)
	{
		auto& drawCommand = package.recordedDrawCommands[drawIndex];
		if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
			drawCommand.passBindingSet.reset();
	}

	for (auto& passInput : package.passCommandInputs)
	{
		switch (passInput.kind)
		{
		case NLS::Render::Context::RenderPassCommandKind::Opaque:
		case NLS::Render::Context::RenderPassCommandKind::Decal:
		case NLS::Render::Context::RenderPassCommandKind::Skybox:
		case NLS::Render::Context::RenderPassCommandKind::Transparent:
		case NLS::Render::Context::RenderPassCommandKind::GBuffer:
		case NLS::Render::Context::RenderPassCommandKind::Lighting:
			ResolvePreparedPassBindingPlaceholders(passInput.recordedDrawCommands, resolvedBindingSet);
			break;
		default:
			ClearPreparedPassBindingPlaceholders(passInput.recordedDrawCommands);
			break;
		}
	}
}

bool BaseSceneRenderer::CaptureThreadedPreparedDraw(
	PipelineState pso,
	const Drawable& drawable,
	PreparedRecordedDraw& outDraw)
{
	NLS_PROFILE_SCOPE();
	auto* bindingProvider = GetFrameObjectBindingProvider();
	if (bindingProvider != nullptr && !bindingProvider->PrepareDraw(pso, drawable))
		return false;

	if (!PrepareRecordedDraw(pso, drawable, outDraw))
		return false;

	if (outDraw.commandBuffer == nullptr && bindingProvider != nullptr)
	{
		Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
		if (bindingProvider->CapturePreparedBindingSets(pso, drawable, bindingSets))
		{
			outDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
			outDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
			outDraw.objectIndex = bindingSets.objectIndex;
			outDraw.usesObjectIndex = bindingSets.usesObjectIndex;
		}
	}

	outDraw.commandBuffer.reset();
	return outDraw.pipeline != nullptr &&
		outDraw.materialBindingSet != nullptr &&
		outDraw.mesh != nullptr &&
		outDraw.instanceCount > 0u;
}

bool BaseSceneRenderer::CaptureThreadedPreparedDraw(
	const Drawable& drawable,
	Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
	Render::Settings::EComparaisonAlgorithm depthCompareOverride,
	std::string_view lightMode,
	PreparedRecordedDraw& outDraw)
{
	NLS_PROFILE_SCOPE();
	auto effectivePso = CreatePipelineState();
	auto* bindingProvider = GetFrameObjectBindingProvider();
	if (bindingProvider != nullptr && !bindingProvider->PrepareDraw(effectivePso, drawable))
		return false;

	if (!PrepareRecordedDraw(drawable, pipelineOverrides, depthCompareOverride, lightMode, outDraw))
		return false;

	if (outDraw.commandBuffer == nullptr && bindingProvider != nullptr)
	{
		Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
		if (bindingProvider->CapturePreparedBindingSets(effectivePso, drawable, bindingSets))
		{
			outDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
			outDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
			outDraw.objectIndex = bindingSets.objectIndex;
			outDraw.usesObjectIndex = bindingSets.usesObjectIndex;
		}
	}

	outDraw.commandBuffer.reset();
	return outDraw.pipeline != nullptr &&
		outDraw.materialBindingSet != nullptr &&
		outDraw.mesh != nullptr &&
		outDraw.instanceCount > 0u;
}

Render::Context::RenderScenePackage BaseSceneRenderer::BuildSnapshotOwnedRenderScenePackage(
	const Render::Context::FrameSnapshot& snapshot,
	const SnapshotRenderScenePackageBuildMode buildMode)
{
	return Render::Context::BuildSnapshotOwnedRenderScenePackage(snapshot, buildMode);
}

Render::Context::RenderScenePackage BaseSceneRenderer::BuildRenderScenePackage(
	const Render::Context::FrameSnapshot& snapshot) const
{
	NLS_PROFILE_SCOPE();
	return BuildSnapshotOwnedRenderScenePackage(snapshot);
}

SceneLightingProvider& BaseSceneRenderer::GetSceneLightingProvider()
{
	return *m_sceneLightingProvider;
}

const SceneLightingProvider& BaseSceneRenderer::GetSceneLightingProvider() const
{
	return *m_sceneLightingProvider;
}

const SceneOcclusionPrimitivePacketBuildResult& BaseSceneRenderer::GetLastHZBOcclusionPrimitivePacketBuildResult() const
{
	return m_lastHZBOcclusionPrimitivePacketBuildResult;
}

const SceneOcclusionHistory& BaseSceneRenderer::GetHZBOcclusionHistoryForTesting() const
{
	return m_hzbOcclusionHistory;
}

bool BaseSceneRenderer::HasLastVisiblePickablePrimitiveDrawSources() const
{
	return m_hasLastVisiblePickablePrimitiveDrawSources;
}

const std::vector<ScenePickablePrimitiveDrawSource>&
BaseSceneRenderer::GetLastVisiblePickablePrimitiveDrawSources() const
{
	return m_lastVisiblePickablePrimitiveDrawSources;
}

const SceneOcclusionFrameInput& BaseSceneRenderer::GetLastHZBOcclusionFrameInput() const
{
	return m_lastHZBOcclusionFrameInput;
}

bool BaseSceneRenderer::HasPendingHZBOcclusionObservationFrame() const
{
	return !m_hzbPendingOcclusionObservationBatch.primitiveInputs.empty();
}

void BaseSceneRenderer::DiscardPendingHZBOcclusionObservationFrame()
{
	m_hzbPendingOcclusionObservationBatch = {};
}

void BaseSceneRenderer::BeginHZBOcclusionObservationFrame(
	const SceneOcclusionFrameInput& frame,
	std::span<const SceneOcclusionPrimitiveInput> primitiveInputs)
{
	std::vector<SceneOcclusionPrimitiveInput> inputs(
		primitiveInputs.begin(),
		primitiveInputs.end());
	m_hzbPendingOcclusionObservationBatch =
		SceneOcclusionSystem::CreatePendingObservationBatch(frame, inputs);
}

SceneOcclusionObservationStats BaseSceneRenderer::CompleteHZBOcclusionObservationFrame(
	std::span<const uint32_t> primitiveResultFlags)
{
	if (m_hzbPendingOcclusionObservationBatch.primitiveInputs.empty())
		return {};

	std::vector<uint32_t> flags(primitiveResultFlags.begin(), primitiveResultFlags.end());
	auto readyBatch = SceneOcclusionSystem::CompleteObservationBatchWithPrimitiveResultFlags(
		m_hzbPendingOcclusionObservationBatch,
		flags);
	const auto stats = SceneOcclusionSystem::ApplyReadyObservationBatch(
		m_hzbOcclusionHistory,
		readyBatch.frame,
		readyBatch);
	if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
	{
		const auto flaggedOccludedCount = static_cast<uint64_t>(std::count_if(
			flags.begin(),
			flags.end(),
			[](const uint32_t flag)
			{
				return flag != 0u;
			}));
		NLS_LOG_INFO(
			"[BaseSceneRenderer][HZBObservation] readbackFlags=" +
			std::to_string(flags.size()) +
			" gpuOccludedFlags=" +
			std::to_string(flaggedOccludedCount) +
			" observed=" +
			std::to_string(stats.observedPrimitiveCount) +
			" appliedOccluded=" +
			std::to_string(stats.occludedPrimitiveCount) +
			" appliedVisible=" +
			std::to_string(stats.visiblePrimitiveCount) +
			" discarded=" +
			std::to_string(stats.discardedPrimitiveCount) +
			" stale=" +
			std::to_string(stats.staleFrameCount) +
			" incompatibleView=" +
			std::to_string(stats.incompatibleViewCount));
	}
	m_hzbPendingOcclusionObservationBatch = {};
	return stats;
}

void BaseSceneRenderer::RefreshSceneLightingDescriptor(SceneSystem::Scene& scene)
{
	NLS_PROFILE_SCOPE();
	m_sceneLightingProvider->Collect(scene);
	AddDescriptor<LightingDescriptor>(LightingDescriptor{ GetSceneLightingProvider().GetLightingDescriptor().lights });
}

void BaseSceneRenderer::DrawModelWithSingleMaterial(
	Render::Data::PipelineState p_pso,
	RenderMesh& p_mesh,
	RenderMaterial& p_material,
	const Maths::Matrix4& p_modelMatrix
)
{
	auto stateMask = p_material.GenerateStateMask();
	auto userMatrix = Maths::Matrix4::Identity;

	auto engineDrawableDescriptor = EngineDrawableDescriptor{
		p_modelMatrix,
		userMatrix
	};

	Render::Entities::Drawable element;
	element.mesh = &p_mesh;
	element.material = &p_material;
	element.stateMask = stateMask;
	element.AddDescriptor(engineDrawableDescriptor);

	DrawEntity(element);
}

BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()
{
	NLS_PROFILE_SCOPE();
	if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
		NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).PumpAsyncLoads(1u);

	m_lastCullReasonDebugSnapshot = {};
	auto previousHZBOcclusionPrimitiveInputs =
		std::move(m_lastHZBOcclusionPrimitivePacketBuildResult.primitiveInputs);
	m_lastHZBOcclusionPrimitivePacketBuildResult = {};

	OpaqueDrawables opaques;
	DecalDrawables decals;
	TransparentDrawables transparents;
	SkyboxDrawables skyboxes;
	m_lastVisiblePickablePrimitiveDrawSources.clear();
	m_hasLastVisiblePickablePrimitiveDrawSources = true;

	auto& camera = *m_frameDescriptor.camera;
	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	auto overrideMaterial = sceneDescriptor.overrideMaterial;
	std::optional<Render::Data::Frustum> frustum = std::nullopt;

	if (camera.HasFrustumGeometryCulling())
	{
		auto& frustumOverride = sceneDescriptor.frustumOverride;
		frustum = frustumOverride ? frustumOverride : camera.GetFrustum();
	}

	const auto occlusionSettings = [&]()
	{
		auto settings = LargeSceneSettings::Defaults();
		SceneOcclusionCapabilityRequest capabilityRequest;
		capabilityRequest.opaqueDepthFormat =
			static_cast<NLS::Render::RHI::TextureFormat>(ResolveDepthFormatKey(m_frameDescriptor));
		const auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		const auto support = device != nullptr
			? SceneOcclusionSystem::ResolveCapabilities(*device, capabilityRequest)
			: SceneOcclusionCapabilitySupport{};
		settings.enableHZBOcclusion = support.backendSupported;
		const auto diagnostics = NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver);
		if (diagnostics.editorValidationDisableHZBOcclusion)
		{
			settings.enableHZBOcclusion = false;
			if (diagnostics.logRenderDrawPath)
				NLS_LOG_INFO("[BaseSceneRenderer][HZB] disabled by editor validation override");
		}
		return settings;
	}();

	SceneOcclusionFrameInput occlusionFrameInput;
	occlusionFrameInput.enabled = occlusionSettings.enableHZBOcclusion;
	occlusionFrameInput.backendSupported = occlusionSettings.enableHZBOcclusion;
	occlusionFrameInput.historyTextureValid = occlusionSettings.enableHZBOcclusion;
	occlusionFrameInput.frameSerial = ++m_hzbOcclusionFrameSerial;
	occlusionFrameInput.maxHistoryAge = occlusionSettings.maxOcclusionHistoryAge;
	occlusionFrameInput.viewKey = BuildHZBViewKey(m_frameDescriptor);
	occlusionFrameInput.viewCompatibilityHash = HashCameraViewCompatibility(camera);
	occlusionFrameInput.projectionHash = HashMatrix(camera.GetProjectionMatrix());
	occlusionFrameInput.jitterHash = 0u;
	occlusionFrameInput.depthFormatKey = ResolveDepthFormatKey(m_frameDescriptor);
	occlusionFrameInput.viewportWidth = static_cast<uint32_t>(m_frameDescriptor.renderWidth);
	occlusionFrameInput.viewportHeight = static_cast<uint32_t>(m_frameDescriptor.renderHeight);
	m_lastHZBOcclusionFrameInput = occlusionFrameInput;

	std::unordered_map<uint64_t, std::vector<SceneOcclusionPrimitiveInput>> previousHZBOcclusionPrimitiveInputsByScene;
	if (occlusionSettings.enableHZBOcclusion && !previousHZBOcclusionPrimitiveInputs.empty())
	{
		NLS_PROFILE_NAMED_SCOPE("BaseSceneRenderer::ParseScene::PartitionPreviousHZBInputs");
		previousHZBOcclusionPrimitiveInputsByScene.reserve(previousHZBOcclusionPrimitiveInputs.size());
		for (auto& input : previousHZBOcclusionPrimitiveInputs)
		{
			const auto sceneId = input.handle.sceneId;
			previousHZBOcclusionPrimitiveInputsByScene[sceneId].push_back(std::move(input));
		}
	}
	const std::vector<SceneOcclusionPrimitiveInput> emptyPreviousHZBOcclusionPrimitiveInputs;

	uint64_t rebuiltCachedCommandCount = 0u;
	uint64_t rawVisibleObjectCount = 0u;
	uint64_t submittedSceneDrawCount = 0u;
	uint64_t dynamicInstanceGroupCount = 0u;
	uint64_t largestInstanceGroupSize = 0u;
	uint64_t objectDataOverflowDroppedObjectCount = 0u;
	const uint64_t streamingFrameSerial = ++m_streamingResidencyFrameSerial;
	std::vector<uint64_t> currentFrameStreamingDependencyPins;
	std::unordered_set<uint64_t> currentFrameStreamingDependencyPinSet;
	RepresentationResidencySnapshot currentFrameRepresentationResidency;
	StreamingResidencyPlanInput allSceneStreamingInput;
	allSceneStreamingInput.frameSerial = streamingFrameSerial;
	NLS::Render::Data::LargeSceneTelemetry occlusionPruneTelemetry;

	auto recordOcclusionPrune = [&occlusionPruneTelemetry](
		const SceneOcclusionHistoryPruneStats& stats,
		const uint64_t elapsedNs)
	{
		occlusionPruneTelemetry.hzbHistoryPruneTouchedHandleCount += stats.touchedHandleCount;
		occlusionPruneTelemetry.hzbHistoryPruneRemovedHandleCount += stats.removedHandleCount;
		occlusionPruneTelemetry.hzbHistoryPruneRemovedKeyCount += stats.removedKeyCount;
		occlusionPruneTelemetry.hzbHistoryPruneTimeNs += elapsedNs;
	};

	auto appendSceneDrawables = [&](
		SceneSystem::Scene& scene,
		RenderScene& renderScene,
		const bool includeSkyboxes,
		const bool requireExplicitMaterialTextures)
	{
		RenderSceneSyncStats syncStats;
		{
			NLS_PROFILE_NAMED_SCOPE("BaseSceneRenderer::ParseScene::SynchronizeRenderScene");
			syncStats = renderScene.Synchronize(scene, {
				ResolveDefaultSceneMaterial(),
				overrideMaterial,
				requireExplicitMaterialTextures,
				&occlusionSettings
			});
		}
		rebuiltCachedCommandCount += syncStats.rebuiltCachedCommandCount;
		if (occlusionSettings.enableHZBOcclusion && !renderScene.GetLastRemovedPrimitiveHandles().empty())
		{
			const auto pruneStart = std::chrono::steady_clock::now();
			const auto pruneStats =
				m_hzbOcclusionHistory.PruneHandles(renderScene.GetLastRemovedPrimitiveHandles());
			recordOcclusionPrune(pruneStats, ElapsedNanoseconds(pruneStart));
		}
		const auto previousInputsIt = previousHZBOcclusionPrimitiveInputsByScene.find(renderScene.GetSceneId());
		const auto& previousSceneHZBOcclusionPrimitiveInputs =
			previousInputsIt != previousHZBOcclusionPrimitiveInputsByScene.end()
				? previousInputsIt->second
				: emptyPreviousHZBOcclusionPrimitiveInputs;
		SceneOcclusionState occlusionState;
		occlusionState.frameInput = occlusionFrameInput;
		occlusionState.history = &m_hzbOcclusionHistory;
		if (occlusionSettings.enableHZBOcclusion && !previousSceneHZBOcclusionPrimitiveInputs.empty())
			occlusionState.primitiveInputs = &previousSceneHZBOcclusionPrimitiveInputs;
		uint64_t hzbBuildTimeNs = 0u;
		RenderSceneVisibleQueues retainedDrawables;
		{
			NLS_PROFILE_NAMED_SCOPE("BaseSceneRenderer::ParseScene::GatherVisibleCommands");
			retainedDrawables = renderScene.GatherVisibleCommands({
				frustum ? &frustum.value() : nullptr,
				camera.GetPosition(),
				camera.GetVisibleLayerMask(),
				&occlusionSettings,
				1.0f,
				0u,
				true,
				false,
				{},
				ShouldPublishCullReasonDebugSnapshots(),
				GetCullReasonDebugSnapshotMaxEntries(),
				occlusionState.primitiveInputs != nullptr ? &occlusionState : nullptr,
				&m_lastRepresentationResidency
			});
		}
		const auto sceneStats = renderScene.GetLastDrawCallOptimizationStats();
		renderScene.AppendPickablePrimitiveDrawSourcesForHandles(
			renderScene.GetLastVisiblePrimitiveHandles(),
			m_lastVisiblePickablePrimitiveDrawSources);
		dynamicInstanceGroupCount += sceneStats.dynamicInstanceGroupCount;
		largestInstanceGroupSize = std::max(largestInstanceGroupSize, sceneStats.largestInstanceGroupSize);
		objectDataOverflowDroppedObjectCount += sceneStats.objectDataOverflowDroppedObjectCount;
		auto largeSceneTelemetry = renderScene.GetLastLargeSceneTelemetry();
		SetLastCullReasonDebugSnapshot(renderScene.GetLastCullReasonDebugSnapshot());
		if (occlusionSettings.enableHZBOcclusion)
		{
			NLS_PROFILE_NAMED_SCOPE("BaseSceneRenderer::ParseScene::BuildHZBObservationPackets");
			const auto candidateStart = std::chrono::steady_clock::now();
			const auto hzbObservationCandidateHandles = SceneOcclusionSystem::BuildHZBObservationCandidateHandles(
				renderScene.GetLastVisiblePrimitiveHandles(),
				previousSceneHZBOcclusionPrimitiveInputs,
				renderScene.GetSceneId());
			hzbBuildTimeNs += ElapsedNanoseconds(candidateStart);
			const auto snapshotStart = std::chrono::steady_clock::now();
			const auto hzbPrimitiveSnapshot = renderScene.CreatePrimitiveSnapshotForHandles(
				hzbObservationCandidateHandles,
				{});
			hzbBuildTimeNs += ElapsedNanoseconds(snapshotStart);

			const auto packetSourceStart = std::chrono::steady_clock::now();
			const auto packetSources = SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
				hzbPrimitiveSnapshot,
				hzbObservationCandidateHandles);
			hzbBuildTimeNs += ElapsedNanoseconds(packetSourceStart);
			SceneOcclusionPrimitivePacketBuildInput packetInput;
			packetInput.viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
			packetInput.viewportWidth = static_cast<uint32_t>(m_frameDescriptor.renderWidth);
			packetInput.viewportHeight = static_cast<uint32_t>(m_frameDescriptor.renderHeight);
			const auto packetBuildStart = std::chrono::steady_clock::now();
			const auto packetBuild = SceneOcclusionSystem::BuildHZBPrimitivePackets(
				packetInput,
				packetSources.sources);
			hzbBuildTimeNs += ElapsedNanoseconds(packetBuildStart);
			m_lastHZBOcclusionPrimitivePacketBuildResult.rejectedPrimitiveCount +=
				packetSources.rejectedPrimitiveCount + packetBuild.rejectedPrimitiveCount;
			m_lastHZBOcclusionPrimitivePacketBuildResult.primitiveInputs.insert(
				m_lastHZBOcclusionPrimitivePacketBuildResult.primitiveInputs.end(),
				packetBuild.primitiveInputs.begin(),
				packetBuild.primitiveInputs.end());
			m_lastHZBOcclusionPrimitivePacketBuildResult.primitivePackets.insert(
				m_lastHZBOcclusionPrimitivePacketBuildResult.primitivePackets.end(),
				packetBuild.primitivePackets.begin(),
				packetBuild.primitivePackets.end());
		}
		{
			NLS_PROFILE_NAMED_SCOPE("BaseSceneRenderer::ParseScene::RegisterStreamingDependencies");
			RegisterRuntimeStreamingDependencies(
				m_streamingResidency,
				renderScene.GetLastVisiblePrimitiveHandles(),
				renderScene.GetLastRepresentationStreamingInterest());
			AppendUniqueRuntimeStreamingDependencyPins(
				currentFrameStreamingDependencyPins,
				currentFrameStreamingDependencyPinSet,
				BuildRuntimeStreamingDependencyPins(
					renderScene.GetLastVisiblePrimitiveHandles(),
					renderScene.GetLastRepresentationStreamingInterest()));
			allSceneStreamingInput.visiblePrimitiveHandles.insert(
				allSceneStreamingInput.visiblePrimitiveHandles.end(),
				renderScene.GetLastVisiblePrimitiveHandles().begin(),
				renderScene.GetLastVisiblePrimitiveHandles().end());
			allSceneStreamingInput.representationStreamingInterest.insert(
				allSceneStreamingInput.representationStreamingInterest.end(),
				renderScene.GetLastRepresentationStreamingInterest().begin(),
				renderScene.GetLastRepresentationStreamingInterest().end());
		}
		largeSceneTelemetry.hzbBuildTimeNs += hzbBuildTimeNs;
		m_rendererStats.RecordLargeSceneTelemetry(largeSceneTelemetry);
		if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
		{
			NLS_LOG_INFO(
				"[BaseSceneRenderer][LargeScene] registered=" +
				std::to_string(largeSceneTelemetry.registeredPrimitiveCount) +
				" visible=" +
				std::to_string(largeSceneTelemetry.visiblePrimitiveCount) +
				" rawDraws=" +
				std::to_string(largeSceneTelemetry.rawVisibleDrawCount) +
				" submittedDraws=" +
				std::to_string(largeSceneTelemetry.submittedDrawCount) +
				" occlusionTests=" +
				std::to_string(largeSceneTelemetry.occlusionTestCount) +
				" occlusionCulled=" +
				std::to_string(largeSceneTelemetry.occlusionCulledCount) +
				" hzbPackets=" +
				std::to_string(m_lastHZBOcclusionPrimitivePacketBuildResult.primitivePackets.size()));
		}

		rawVisibleObjectCount += sceneStats.rawVisibleObjectCount;
		submittedSceneDrawCount += sceneStats.submittedSceneDrawCount;
		{
			NLS_PROFILE_NAMED_SCOPE("BaseSceneRenderer::ParseScene::AppendVisibleDrawables");
			opaques.insert(
				opaques.end(),
				std::make_move_iterator(retainedDrawables.opaques.begin()),
				std::make_move_iterator(retainedDrawables.opaques.end()));
			decals.insert(
				decals.end(),
				std::make_move_iterator(retainedDrawables.decals.begin()),
				std::make_move_iterator(retainedDrawables.decals.end()));
			transparents.insert(
				transparents.end(),
				std::make_move_iterator(retainedDrawables.transparents.begin()),
				std::make_move_iterator(retainedDrawables.transparents.end()));
		}

		if (!includeSkyboxes)
			return;

		const auto& fastAccess = scene.GetFastAccessComponents();
		skyboxes.reserve(skyboxes.size() + fastAccess.skyboxs.size());

		for (auto* skybox : fastAccess.skyboxs)
		{
			if (!skybox)
				continue;
			auto* owner = skybox->gameobject();
			if (!owner || !owner->IsActive())
				continue;

			if (auto mesh = skybox->GetMesh())
			{
				if (auto material = skybox->GetMaterial())
				{
					auto& transform = owner->GetTransform()->GetTransform();
					Render::Entities::Drawable drawable;
					drawable.mesh = mesh;
					drawable.material = material;
					drawable.stateMask = material->GenerateStateMask();
					drawable.AddDescriptor<EngineDrawableDescriptor>({ transform.GetWorldMatrix() });
					skyboxes.emplace_back(0.0f, std::move(drawable));
				}
			}
		}
	};

		appendSceneDrawables(
			sceneDescriptor.scene,
			m_renderScene,
			sceneDescriptor.includeSkyboxes,
			sceneDescriptor.requireExplicitMaterialTextures);
	for (auto it = m_additiveRenderScenes.begin(); it != m_additiveRenderScenes.end();)
	{
		const auto* cachedScene = it->first;
		const auto isStillActive = std::find(
			sceneDescriptor.additiveScenes.begin(),
			sceneDescriptor.additiveScenes.end(),
			cachedScene) != sceneDescriptor.additiveScenes.end();
		if (isStillActive)
			++it;
		else
		{
			if (occlusionSettings.enableHZBOcclusion)
			{
				const auto droppedSceneHandles = it->second.GetLivePrimitiveHandles();
				const auto pruneStart = std::chrono::steady_clock::now();
				const auto pruneStats = m_hzbOcclusionHistory.PruneHandles(droppedSceneHandles);
				recordOcclusionPrune(pruneStats, ElapsedNanoseconds(pruneStart));
			}
			it = m_additiveRenderScenes.erase(it);
		}
	}
	for (auto* additiveScene : sceneDescriptor.additiveScenes)
	{
		if (!additiveScene)
			continue;
		auto& additiveRenderScene = m_additiveRenderScenes[additiveScene];
		appendSceneDrawables(*additiveScene, additiveRenderScene, false, true);
	}
	if (occlusionPruneTelemetry.hzbHistoryPruneTouchedHandleCount > 0u ||
		occlusionPruneTelemetry.hzbHistoryPruneRemovedHandleCount > 0u ||
		occlusionPruneTelemetry.hzbHistoryPruneRemovedKeyCount > 0u ||
		occlusionPruneTelemetry.hzbHistoryPruneTimeNs > 0u)
	{
		m_rendererStats.RecordLargeSceneTelemetry(occlusionPruneTelemetry);
	}
	const auto streamingCommitStart = std::chrono::steady_clock::now();
	const auto allSceneStreamingPlan = m_streamingResidency.Plan(allSceneStreamingInput, occlusionSettings);
	StreamingResidencyFramePins framePins;
	framePins.pinnedDependencyIds =
		NLS::Render::Context::DriverRendererAccess::CollectStreamingDependencyPins(m_driver);
	const auto allSceneStreamingCommit = m_streamingResidency.Commit(
		allSceneStreamingPlan,
		occlusionSettings,
		framePins);
	MergeRepresentationResidencySnapshot(
		currentFrameRepresentationResidency,
		BuildRepresentationResidencySnapshotFromStreamingCommit(
			allSceneStreamingPlan,
			allSceneStreamingCommit));
	NLS::Render::Data::LargeSceneTelemetry streamingTelemetry;
	MergeStreamingTelemetry(streamingTelemetry, allSceneStreamingPlan, allSceneStreamingCommit);
	streamingTelemetry.streamingCommitTimeNs += ElapsedNanoseconds(streamingCommitStart);
	m_rendererStats.RecordLargeSceneTelemetry(streamingTelemetry);
	m_lastStreamingDependencyPins = std::move(currentFrameStreamingDependencyPins);
	m_lastRepresentationResidency = std::move(currentFrameRepresentationResidency);

	SortSceneDrawables(decals, std::greater<float>{});
	SortSceneDrawables(transparents, std::greater<float>{});

	uint32_t nextObjectIndex = 0u;
	auto reassignObjectIndices = [&nextObjectIndex, &objectDataOverflowDroppedObjectCount](auto& queue)
	{
		for (auto& entry : queue)
		{
			EngineDrawableDescriptor descriptor;
			if (!entry.second.template TryGetDescriptor<EngineDrawableDescriptor>(descriptor))
				continue;

			const uint32_t objectCount = std::max<uint32_t>(1u, descriptor.objectCount);
			uint32_t lastObjectIndex = 0u;
			if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(
				nextObjectIndex,
				objectCount,
				lastObjectIndex))
			{
				descriptor.objectIndex = EngineDrawableDescriptor::kInvalidObjectIndex;
				objectDataOverflowDroppedObjectCount += objectCount;
			}
			else
			{
				descriptor.objectIndex = nextObjectIndex;
				nextObjectIndex = lastObjectIndex + 1u;
			}
			entry.second.template RemoveDescriptor<EngineDrawableDescriptor>();
			entry.second.template AddDescriptor<EngineDrawableDescriptor>(std::move(descriptor));
		}
	};
	reassignObjectIndices(opaques);
	reassignObjectIndices(decals);
	reassignObjectIndices(skyboxes);
	reassignObjectIndices(transparents);

	if (!PumpOneVisibleMaterialTexture(opaques) &&
		!PumpOneVisibleMaterialTexture(decals))
	{
		PumpOneVisibleMaterialTexture(transparents);
	}

	SortSceneDrawables(skyboxes, std::less<float>{});
	m_rendererStats.RecordSceneParse(
		static_cast<uint64_t>(opaques.size()),
		static_cast<uint64_t>(decals.size() + transparents.size()),
		static_cast<uint64_t>(skyboxes.size()));
	auto optimizationStats = m_renderScene.GetLastDrawCallOptimizationStats();
	optimizationStats.cachedCommandRebuildCount = rebuiltCachedCommandCount;
	optimizationStats.rawVisibleObjectCount = rawVisibleObjectCount + static_cast<uint64_t>(skyboxes.size());
	optimizationStats.submittedSceneDrawCount = submittedSceneDrawCount + static_cast<uint64_t>(skyboxes.size());
	optimizationStats.dynamicInstanceGroupCount = dynamicInstanceGroupCount;
	optimizationStats.largestInstanceGroupSize = largestInstanceGroupSize;
	optimizationStats.objectDataOverflowDroppedObjectCount = objectDataOverflowDroppedObjectCount;
	m_rendererStats.RecordDrawCallOptimizationStats(optimizationStats);
	return { opaques, decals, transparents, skyboxes };
}
}
