#pragma once

#include <algorithm>
#include <cstdint>
#include <fg/FrameGraphResource.hpp>
#include <functional>
#include <limits>
#include <Math/Vector4.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"

namespace NLS::Render::FrameGraph
{
	inline constexpr uint64_t kThreadedPlanUsePassDrawCount = (std::numeric_limits<uint64_t>::max)();

	struct RenderPassExecutionMetadata
	{
		bool useFrameClearState = false;
		bool clearColor = false;
		bool clearDepth = false;
		bool clearStencil = false;
		Maths::Vector4 clearValue = Maths::Vector4::Zero;
	};

	enum class ThreadedRenderScenePassRole : uint8_t
	{
		Opaque = 0,
		Transparent,
		Skybox,
		Helper,
		Auxiliary
	};

	enum class ThreadedRenderScenePassExecutionMode : uint8_t
	{
		Output = 0,
		Recorded
	};

	struct ThreadedRenderScenePassMetadata
	{
		Context::RenderPassCommandKind commandKind = Context::RenderPassCommandKind::Opaque;
		ThreadedRenderScenePassRole role = ThreadedRenderScenePassRole::Auxiliary;
		ThreadedRenderScenePassExecutionMode executionMode = ThreadedRenderScenePassExecutionMode::Output;
		RHI::QueueType queueType = RHI::QueueType::Graphics;
		Context::QueueDependencyPolicy queueDependencyPolicy = Context::QueueDependencyPolicy::Previous;
		const char* graphPassName = "";
		uint64_t visibleDrawCountContribution = kThreadedPlanUsePassDrawCount;
		bool propagatesColorOutput = true;
		bool propagatesDepthOutput = true;
		RenderPassExecutionMetadata execution;
		std::shared_ptr<std::string> ownedGraphPassName;
	};

	struct ScenePassOutputChain
	{
		FrameGraphResource color = -1;
		FrameGraphResource depth = -1;
		bool requiresSideEffect = false;
	};

	struct ScenePassOutputResourceState
	{
		FrameGraphResource color = -1;
		FrameGraphResource depth = -1;
	};

	struct OutputRenderPassExecutionDesc
	{
		uint16_t renderWidth = 0u;
		uint16_t renderHeight = 0u;
		bool clearColor = false;
		bool clearDepth = false;
		bool clearStencil = false;
		Maths::Vector4 clearValue = Maths::Vector4::Zero;
	};

	struct RecordedRenderPassExecutionDesc
	{
		uint16_t renderWidth = 0u;
		uint16_t renderHeight = 0u;
		bool clearColor = false;
		bool clearDepth = false;
		bool clearStencil = false;
		Maths::Vector4 clearValue = Maths::Vector4::Zero;
	};

	inline void SetThreadedRenderScenePassGraphPassName(
		ThreadedRenderScenePassMetadata& metadata,
		std::string graphPassName)
	{
		metadata.ownedGraphPassName = std::make_shared<std::string>(std::move(graphPassName));
		metadata.graphPassName = metadata.ownedGraphPassName->c_str();
	}

	template<typename TPassData = void>
	struct ThreadedRenderSceneGraphPassDescriptor
	{
		ThreadedRenderScenePassMetadata metadata;
	};

	struct ThreadedRenderScenePassPlan
	{
		Context::RenderPassCommandInput commandInput;
		ThreadedRenderScenePassRole role = ThreadedRenderScenePassRole::Auxiliary;
		RHI::QueueType queueType = RHI::QueueType::Graphics;
		Context::QueueDependencyPolicy queueDependencyPolicy = Context::QueueDependencyPolicy::Previous;
		std::optional<size_t> dependencySourcePassIndex;
		bool requiresDependencyVisibility = false;
		std::string_view graphPassName;
		uint64_t visibleDrawCountContribution = 0u;
	};

	using ThreadedRenderSceneDependencyKind = Context::ThreadedDependencyKind;
	using ThreadedRenderSceneDependencyResourceKind = Context::ThreadedDependencyResourceKind;

	struct ThreadedRenderSceneDependencyEdge
	{
		size_t sourcePassIndex = 0u;
		size_t targetPassIndex = 0u;
		ThreadedRenderSceneDependencyKind kind = ThreadedRenderSceneDependencyKind::ResourceVisibility;
		ThreadedRenderSceneDependencyResourceKind resourceKind = ThreadedRenderSceneDependencyResourceKind::None;
		std::optional<Context::BufferResourceAccess> sourceBufferAccess;
		std::optional<Context::BufferResourceAccess> targetBufferAccess;
		std::optional<Context::TextureResourceAccess> sourceTextureAccess;
		std::optional<Context::TextureResourceAccess> targetTextureAccess;
	};

	struct ThreadedRenderSceneExecutionPlan
	{
		std::vector<ThreadedRenderScenePassPlan> passes;
		std::vector<ThreadedRenderSceneDependencyEdge> dependencies;
	};

	struct CompiledThreadedRenderSceneGraphPass
	{
		ThreadedRenderScenePassMetadata metadata;
		ScenePassOutputChain outputChain;
		OutputRenderPassExecutionDesc outputExecution;
		RecordedRenderPassExecutionDesc recordedExecution;
	};

	struct CompiledThreadedRenderSceneExecution
	{
		std::vector<CompiledThreadedRenderSceneGraphPass> graphPasses;
		ThreadedRenderSceneExecutionPlan threadedPlan;
	};

	struct PreparedComputeDispatchPassData
	{
	};

	struct PreparedComputeDispatchSource
	{
		std::vector<Context::RecordedComputeDispatchInput> dispatchInputs;
		std::vector<ThreadedRenderScenePassMetadata> metadata;
	};

	struct PreparedComputeCompilationInput
	{
		PreparedComputeDispatchSource source;
		std::function<void(Context::RenderScenePackage&)> applyToPackage;
	};

	enum class FrameGraphCompileDiagnosticSeverity : uint8_t
	{
		Info = 0,
		Warning,
		Error
	};

	enum class FrameGraphCompileDiagnosticCode : uint8_t
	{
		EmptyGraphPassName = 0,
		MissingPassMetadata,
		NullBufferResourceAccess,
		NullTextureResourceAccess,
		NullBufferVisibilityTransition,
		NullTextureVisibilityTransition,
		RecordedPassPropagatesOutput,
		ComputePassUsesNonComputeQueue,
		ComputePassPropagatesOutput,
		MissingQueueDependencySource,
		ConflictingBufferResourceAccess,
		ConflictingTextureResourceAccess,
		InvalidQueueDependency,
		UndeclaredRDGResourceUse
	};

	struct FrameGraphCompileDiagnostic
	{
		FrameGraphCompileDiagnosticSeverity severity = FrameGraphCompileDiagnosticSeverity::Error;
		FrameGraphCompileDiagnosticCode code = FrameGraphCompileDiagnosticCode::EmptyGraphPassName;
		size_t passIndex = 0u;
		Context::RenderPassCommandKind passKind = Context::RenderPassCommandKind::Opaque;
		std::string message;
	};

	struct FrameGraphCompileValidationResult
	{
		std::vector<FrameGraphCompileDiagnostic> diagnostics;

		bool HasErrors() const
		{
			return std::any_of(
				diagnostics.begin(),
				diagnostics.end(),
				[](const FrameGraphCompileDiagnostic& diagnostic)
				{
					return diagnostic.severity == FrameGraphCompileDiagnosticSeverity::Error;
				});
		}

		bool ContainsError(const FrameGraphCompileDiagnosticCode code) const
		{
			return std::any_of(
				diagnostics.begin(),
				diagnostics.end(),
				[code](const FrameGraphCompileDiagnostic& diagnostic)
				{
					return diagnostic.severity == FrameGraphCompileDiagnosticSeverity::Error &&
						diagnostic.code == code;
				});
		}
	};

	struct ThreadedRenderSceneExecutionPlanBuildResult
	{
		bool succeeded = false;
		ThreadedRenderSceneExecutionPlan plan;
		FrameGraphCompileValidationResult validation;
	};

	struct ThreadedRenderSceneExecutionCompileResult
	{
		bool succeeded = false;
		CompiledThreadedRenderSceneExecution execution;
		FrameGraphCompileValidationResult validation;
	};

	struct RDGPassContract
	{
		std::string name;
		RHI::QueueType queueType = RHI::QueueType::Graphics;
		bool hasSideEffect = false;
		std::vector<std::string> declaredResourceNames;
		std::vector<std::string> usedResourceNames;
	};
}
