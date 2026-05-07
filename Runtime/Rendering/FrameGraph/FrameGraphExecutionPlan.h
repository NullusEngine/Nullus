#pragma once

#include <algorithm>
#include <cstdint>
#include <fg/FrameGraph.hpp>
#include <functional>
#include <iterator>
#include <limits>
#include <Math/Vector4.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"

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

	template<typename T, typename = void>
	struct HasThreadedRenderScenePassMetadataMember : std::false_type
	{
	};

	template<typename T>
	struct HasThreadedRenderScenePassMetadataMember<T, std::void_t<decltype(std::declval<const T&>().metadata)>> : std::true_type
	{
	};

	template<typename T>
	inline const ThreadedRenderScenePassMetadata& ResolveThreadedRenderScenePassMetadata(const T& value)
	{
		if constexpr (HasThreadedRenderScenePassMetadataMember<T>::value)
			return value.metadata;
		else
			return value;
	}

	template<typename TPrefixRange, typename TMetadataRange>
	inline std::vector<ThreadedRenderScenePassMetadata> BuildThreadedRenderScenePassMetadataSequence(
		const TPrefixRange& prefixRange,
		const TMetadataRange& metadataRange)
	{
		const auto prefixCount = static_cast<size_t>(std::distance(std::begin(prefixRange), std::end(prefixRange)));
		const auto metadataCount = static_cast<size_t>(std::distance(std::begin(metadataRange), std::end(metadataRange)));

		std::vector<ThreadedRenderScenePassMetadata> sequence;
		sequence.reserve(prefixCount + metadataCount);

		for (const auto& prefixEntry : prefixRange)
			sequence.push_back(ResolveThreadedRenderScenePassMetadata(prefixEntry));

		for (const auto& metadataEntry : metadataRange)
			sequence.push_back(ResolveThreadedRenderScenePassMetadata(metadataEntry));

		return sequence;
	}

	template<typename TDescriptorRange>
	inline auto FindThreadedRenderSceneDescriptorByKind(
		const TDescriptorRange& descriptorRange,
		const Context::RenderPassCommandKind kind)
		-> const std::decay_t<decltype(*std::begin(descriptorRange))>*
	{
		for (const auto& descriptor : descriptorRange)
		{
			if (ResolveThreadedRenderScenePassMetadata(descriptor).commandKind == kind)
				return &descriptor;
		}
		return nullptr;
	}

	template<typename TDescriptorRange>
	inline const std::decay_t<decltype(*std::begin(std::declval<const TDescriptorRange&>()))>& RequireThreadedRenderSceneDescriptorByKind(
		const TDescriptorRange& descriptorRange,
		const Context::RenderPassCommandKind kind)
	{
		const auto* descriptor = FindThreadedRenderSceneDescriptorByKind(descriptorRange, kind);
		if (descriptor == nullptr)
			throw std::invalid_argument("Missing ThreadedRenderSceneGraphPassDescriptor for pass kind");
		return *descriptor;
	}

	struct ScenePassOutputChain
	{
		int32_t color = -1;
		int32_t depth = -1;
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

	inline std::vector<ThreadedRenderScenePassMetadata> BuildPreparedComputeDispatchPassMetadata(
		const std::vector<Context::RecordedComputeDispatchInput>& dispatchInputs)
	{
		std::vector<ThreadedRenderScenePassMetadata> metadata;
		metadata.reserve(dispatchInputs.size());

		for (const auto& dispatchInput : dispatchInputs)
		{
			ThreadedRenderScenePassMetadata passMetadata;
			passMetadata.commandKind = Context::RenderPassCommandKind::Compute;
			passMetadata.role = ThreadedRenderScenePassRole::Auxiliary;
			passMetadata.queueType = RHI::QueueType::Compute;
			passMetadata.queueDependencyPolicy = Context::QueueDependencyPolicy::None;
			SetThreadedRenderScenePassGraphPassName(passMetadata, dispatchInput.debugName);
			passMetadata.visibleDrawCountContribution = 0u;
			passMetadata.propagatesColorOutput = false;
			passMetadata.propagatesDepthOutput = false;
			metadata.push_back(passMetadata);
		}

		return metadata;
	}

	inline PreparedComputeDispatchSource BuildPreparedComputeDispatchSource(
		std::vector<Context::RecordedComputeDispatchInput> dispatchInputs)
	{
		PreparedComputeDispatchSource source;
		source.dispatchInputs = std::move(dispatchInputs);
		source.metadata = BuildPreparedComputeDispatchPassMetadata(source.dispatchInputs);
		return source;
	}

	inline void PromotePreparedComputeConsumerDependencies(
		std::vector<ThreadedRenderScenePassMetadata>& metadata)
	{
		bool pendingComputeProducer = false;
		for (auto& passMetadata : metadata)
		{
			if (passMetadata.queueType == RHI::QueueType::Compute ||
				passMetadata.commandKind == Context::RenderPassCommandKind::Compute)
			{
				pendingComputeProducer = true;
				continue;
			}

			if (!pendingComputeProducer)
				continue;

			if (passMetadata.queueDependencyPolicy == Context::QueueDependencyPolicy::Previous)
				passMetadata.queueDependencyPolicy = Context::QueueDependencyPolicy::LastCompute;
			pendingComputeProducer = false;
		}
	}

	template<typename TMetadataRange>
	inline std::vector<ThreadedRenderScenePassMetadata> BuildPreparedComputeAndScenePassMetadata(
		const PreparedComputeDispatchSource& preparedComputeSource,
		const TMetadataRange& scenePassMetadataRange)
	{
		auto metadata = BuildThreadedRenderScenePassMetadataSequence(
			preparedComputeSource.metadata,
			scenePassMetadataRange);
		PromotePreparedComputeConsumerDependencies(metadata);
		return metadata;
	}

	template<typename TMetadataRange>
	inline std::vector<ThreadedRenderScenePassMetadata> BuildPreparedComputeAndScenePassMetadata(
		const std::vector<Context::RecordedComputeDispatchInput>& preparedComputeDispatchInputs,
		const TMetadataRange& scenePassMetadataRange)
	{
		auto metadata = BuildThreadedRenderScenePassMetadataSequence(
			BuildPreparedComputeDispatchPassMetadata(preparedComputeDispatchInputs),
			scenePassMetadataRange);
		PromotePreparedComputeConsumerDependencies(metadata);
		return metadata;
	}

	inline const Context::RecordedComputeDispatchInput* FindPreparedComputeDispatchByName(
		const PreparedComputeDispatchSource& preparedComputeSource,
		const std::string_view debugName)
	{
		const auto it = std::find_if(
			preparedComputeSource.dispatchInputs.begin(),
			preparedComputeSource.dispatchInputs.end(),
			[debugName](const Context::RecordedComputeDispatchInput& dispatchInput)
			{
				return dispatchInput.debugName == debugName;
			});
		return it != preparedComputeSource.dispatchInputs.end() ? &(*it) : nullptr;
	}

	inline const Context::RecordedComputeDispatchInput* FindPreparedComputeDispatchByName(
		const std::vector<Context::RecordedComputeDispatchInput>& dispatchInputs,
		const std::string_view debugName)
	{
		const auto it = std::find_if(
			dispatchInputs.begin(),
			dispatchInputs.end(),
			[debugName](const Context::RecordedComputeDispatchInput& dispatchInput)
			{
				return dispatchInput.debugName == debugName;
			});
		return it != dispatchInputs.end() ? &(*it) : nullptr;
	}

	inline void RecordPreparedComputeDispatch(
		FrameGraphExecutionContext& executionContext,
		const Context::RecordedComputeDispatchInput& dispatchInput)
	{
		if (!executionContext.HasExplicitContext() || dispatchInput.pipeline == nullptr)
			return;

		auto& commandBuffer = *executionContext.commandBuffer;
		auto recordBufferBarriers = [&](const auto& buffers,
			const RHI::ResourceState before,
			const RHI::ResourceState after,
			const RHI::PipelineStageMask sourceStages,
			const RHI::PipelineStageMask destinationStages,
			const RHI::AccessMask sourceAccess,
			const RHI::AccessMask destinationAccess)
		{
			if (buffers.empty())
				return;

			RHI::RHIBarrierDesc barriers;
			barriers.bufferBarriers.reserve(buffers.size());
			for (const auto& buffer : buffers)
			{
				if (buffer == nullptr)
					continue;

				barriers.bufferBarriers.push_back({
					buffer,
					before,
					after,
					sourceStages,
					destinationStages,
					sourceAccess,
					destinationAccess
				});
			}

			if (!barriers.bufferBarriers.empty())
				commandBuffer.Barrier(barriers);
		};

		recordBufferBarriers(
			dispatchInput.shaderReadBuffersBefore,
			RHI::ResourceState::Unknown,
			RHI::ResourceState::ShaderRead,
			RHI::PipelineStageMask::AllCommands,
			RHI::PipelineStageMask::ComputeShader,
			RHI::AccessMask::MemoryRead | RHI::AccessMask::MemoryWrite,
			RHI::AccessMask::ShaderRead);

		recordBufferBarriers(
			dispatchInput.shaderWriteBuffersBefore,
			RHI::ResourceState::Unknown,
			RHI::ResourceState::ShaderWrite,
			RHI::PipelineStageMask::AllCommands,
			RHI::PipelineStageMask::ComputeShader,
			RHI::AccessMask::MemoryRead | RHI::AccessMask::MemoryWrite,
			RHI::AccessMask::ShaderWrite);

		recordBufferBarriers(
			dispatchInput.uavBarrierBuffersBefore,
			RHI::ResourceState::ShaderWrite,
			RHI::ResourceState::ShaderWrite,
			RHI::PipelineStageMask::ComputeShader,
			RHI::PipelineStageMask::ComputeShader,
			RHI::AccessMask::ShaderWrite,
			RHI::AccessMask::ShaderRead | RHI::AccessMask::ShaderWrite);

		commandBuffer.BindComputePipeline(dispatchInput.pipeline);
		for (const auto& bindingSet : dispatchInput.bindingSets)
		{
			if (bindingSet.bindingSet != nullptr)
				commandBuffer.BindBindingSet(bindingSet.setIndex, bindingSet.bindingSet);
		}

		commandBuffer.Dispatch(
			dispatchInput.groupCountX,
			dispatchInput.groupCountY,
			dispatchInput.groupCountZ);

		recordBufferBarriers(
			dispatchInput.uavBarrierBuffersAfter,
			RHI::ResourceState::ShaderWrite,
			RHI::ResourceState::ShaderWrite,
			RHI::PipelineStageMask::ComputeShader,
			RHI::PipelineStageMask::ComputeShader,
			RHI::AccessMask::ShaderWrite,
			RHI::AccessMask::ShaderRead | RHI::AccessMask::ShaderWrite);

		recordBufferBarriers(
			dispatchInput.shaderReadBuffersAfter,
			RHI::ResourceState::ShaderWrite,
			RHI::ResourceState::ShaderRead,
			RHI::PipelineStageMask::ComputeShader,
			RHI::PipelineStageMask::FragmentShader,
			RHI::AccessMask::ShaderWrite,
			RHI::AccessMask::ShaderRead);
	}

	inline bool TryAddPreparedComputeDispatchCompiledGraphPass(
		::FrameGraph& frameGraph,
		const PreparedComputeDispatchSource& preparedComputeSource,
		const CompiledThreadedRenderSceneGraphPass& compiledPass)
	{
		if (compiledPass.metadata.commandKind != Context::RenderPassCommandKind::Compute)
			return false;

		const auto* dispatchInput = FindPreparedComputeDispatchByName(
			preparedComputeSource,
			compiledPass.metadata.graphPassName);
		if (dispatchInput == nullptr)
			return false;

		frameGraph.addCallbackPass<PreparedComputeDispatchPassData>(
			compiledPass.metadata.graphPassName,
			[](::FrameGraph::Builder& builder, PreparedComputeDispatchPassData&)
			{
				builder.setSideEffect();
			},
			[dispatchInput = *dispatchInput](const PreparedComputeDispatchPassData&, ::FrameGraphPassResources&, void* context)
			{
				auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
				if (executionContext == nullptr)
					return;

				RecordPreparedComputeDispatch(*executionContext, dispatchInput);
			});
		return true;
	}

	inline bool TryAddPreparedComputeDispatchCompiledGraphPass(
		::FrameGraph& frameGraph,
		const std::vector<Context::RecordedComputeDispatchInput>& dispatchInputs,
		const CompiledThreadedRenderSceneGraphPass& compiledPass)
	{
		if (compiledPass.metadata.commandKind != Context::RenderPassCommandKind::Compute)
			return false;

		const auto* dispatchInput = FindPreparedComputeDispatchByName(
			dispatchInputs,
			compiledPass.metadata.graphPassName);
		if (dispatchInput == nullptr)
			return false;

		frameGraph.addCallbackPass<PreparedComputeDispatchPassData>(
			compiledPass.metadata.graphPassName,
			[](::FrameGraph::Builder& builder, PreparedComputeDispatchPassData&)
			{
				builder.setSideEffect();
			},
			[dispatchInput = *dispatchInput](const PreparedComputeDispatchPassData&, ::FrameGraphPassResources&, void* context)
			{
				auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
				if (executionContext == nullptr)
					return;

				RecordPreparedComputeDispatch(*executionContext, dispatchInput);
			});
		return true;
	}

	inline bool TryBuildPreparedComputeDispatchThreadedPassInput(
		const PreparedComputeDispatchSource& preparedComputeSource,
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		Context::RenderPassCommandInput& passInput)
	{
		if (compiledPass.metadata.commandKind != Context::RenderPassCommandKind::Compute)
			return false;

		const auto* dispatchInput = FindPreparedComputeDispatchByName(
			preparedComputeSource,
			compiledPass.metadata.graphPassName);
		if (dispatchInput == nullptr)
			return false;

		passInput.kind = compiledPass.metadata.commandKind;
		passInput.debugName = compiledPass.metadata.graphPassName;
		passInput.renderWidth = compiledPass.outputExecution.renderWidth;
		passInput.renderHeight = compiledPass.outputExecution.renderHeight;
		passInput.clearColor = compiledPass.outputExecution.clearColor;
		passInput.clearDepth = compiledPass.outputExecution.clearDepth;
		passInput.clearStencil = compiledPass.outputExecution.clearStencil;
		passInput.clearColorValue = compiledPass.outputExecution.clearValue;
		passInput.computeDispatchInputs.push_back(*dispatchInput);
		for (const auto& buffer : dispatchInput->shaderReadBuffersBefore)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Read,
				RHI::ResourceState::ShaderRead,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderRead
			});
		}
		for (const auto& buffer : dispatchInput->shaderWriteBuffersBefore)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Write,
				RHI::ResourceState::ShaderWrite,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderWrite
			});
		}
		for (const auto& buffer : dispatchInput->uavBarrierBuffersAfter)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Write,
				RHI::ResourceState::ShaderWrite,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderWrite
			});
		}
		for (const auto& buffer : dispatchInput->shaderReadBuffersAfter)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Write,
				RHI::ResourceState::ShaderRead,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderRead
			});
		}
		return true;
	}

	inline bool TryBuildPreparedComputeDispatchThreadedPassInput(
		const std::vector<Context::RecordedComputeDispatchInput>& dispatchInputs,
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		Context::RenderPassCommandInput& passInput)
	{
		if (compiledPass.metadata.commandKind != Context::RenderPassCommandKind::Compute)
			return false;

		const auto* dispatchInput = FindPreparedComputeDispatchByName(
			dispatchInputs,
			compiledPass.metadata.graphPassName);
		if (dispatchInput == nullptr)
			return false;

		passInput.kind = compiledPass.metadata.commandKind;
		passInput.debugName = compiledPass.metadata.graphPassName;
		passInput.renderWidth = compiledPass.outputExecution.renderWidth;
		passInput.renderHeight = compiledPass.outputExecution.renderHeight;
		passInput.clearColor = compiledPass.outputExecution.clearColor;
		passInput.clearDepth = compiledPass.outputExecution.clearDepth;
		passInput.clearStencil = compiledPass.outputExecution.clearStencil;
		passInput.clearColorValue = compiledPass.outputExecution.clearValue;
		passInput.computeDispatchInputs.push_back(*dispatchInput);
		for (const auto& buffer : dispatchInput->shaderReadBuffersBefore)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Read,
				RHI::ResourceState::ShaderRead,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderRead
			});
		}
		for (const auto& buffer : dispatchInput->shaderWriteBuffersBefore)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Write,
				RHI::ResourceState::ShaderWrite,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderWrite
			});
		}
		for (const auto& buffer : dispatchInput->uavBarrierBuffersAfter)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Write,
				RHI::ResourceState::ShaderWrite,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderWrite
			});
		}
		for (const auto& buffer : dispatchInput->shaderReadBuffersAfter)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferResourceAccesses.push_back({
				buffer,
				Context::ResourceAccessMode::Write,
				RHI::ResourceState::ShaderRead,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderRead
			});
		}
		return true;
	}

	inline Context::RenderPassCommandInput MakeCompiledThreadedRenderPassCommandInput(
		const CompiledThreadedRenderSceneGraphPass& compiledPass);

	template<typename TPreparedComputeSource>
	inline std::vector<Context::RenderPassCommandInput> BuildPreparedComputeAndScenePassInputs(
		const TPreparedComputeSource& preparedComputeSource,
		const std::vector<CompiledThreadedRenderSceneGraphPass>& compiledPasses,
		const std::vector<Context::RenderPassCommandInput>& scenePassInputs)
	{
		std::vector<Context::RenderPassCommandInput> passInputs;
		passInputs.reserve(compiledPasses.size());

		size_t scenePassIndex = 0u;
		for (const auto& compiledPass : compiledPasses)
		{
			Context::RenderPassCommandInput passInput;
			if (TryBuildPreparedComputeDispatchThreadedPassInput(
				preparedComputeSource,
				compiledPass,
				passInput))
			{
				passInputs.push_back(std::move(passInput));
				continue;
			}

			if (scenePassIndex < scenePassInputs.size() &&
				scenePassInputs[scenePassIndex].kind == compiledPass.metadata.commandKind)
			{
				passInputs.push_back(scenePassInputs[scenePassIndex++]);
				continue;
			}

			passInputs.push_back(MakeCompiledThreadedRenderPassCommandInput(compiledPass));
		}

		if (scenePassIndex != scenePassInputs.size())
			throw std::invalid_argument("Unused scene pass input while building compiled render-scene pass inputs");

		return passInputs;
	}

	inline OutputRenderPassExecutionDesc BuildOutputRenderPassExecutionDesc(
		const Data::FrameDescriptor& frame,
		const ThreadedRenderScenePassMetadata& metadata);

	inline RecordedRenderPassExecutionDesc BuildRecordedRenderPassExecutionDesc(
		uint16_t renderWidth,
		uint16_t renderHeight,
		const ThreadedRenderScenePassMetadata& metadata);

	inline Context::RenderPassCommandInput MakeCompiledThreadedRenderPassCommandInput(
		const CompiledThreadedRenderSceneGraphPass& compiledPass);

	inline ScenePassOutputChain ResolveScenePassOutputChain(
		int32_t previousColor,
		int32_t previousDepth,
		int32_t importedColor,
		int32_t importedDepth,
		const ThreadedRenderScenePassMetadata& metadata);

	inline bool UsesRecordedRenderPassExecution(const ThreadedRenderScenePassMetadata& metadata)
	{
		return metadata.executionMode == ThreadedRenderScenePassExecutionMode::Recorded;
	}

	template<typename T>
	inline void ApplyCompiledThreadedRenderSceneExecutionToCommandInput(
		const T&,
		Context::RenderPassCommandInput&)
	{
	}

	inline void ApplyCompiledThreadedRenderSceneExecutionToCommandInput(
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		Context::RenderPassCommandInput& commandInput)
	{
		commandInput.debugName = compiledPass.metadata.graphPassName;
		commandInput.queueType = compiledPass.metadata.queueType;
		commandInput.queueDependencyPolicy = compiledPass.metadata.queueDependencyPolicy;
		if (UsesRecordedRenderPassExecution(compiledPass.metadata))
		{
			commandInput.renderWidth = compiledPass.recordedExecution.renderWidth;
			commandInput.renderHeight = compiledPass.recordedExecution.renderHeight;
			commandInput.clearColor = compiledPass.recordedExecution.clearColor;
			commandInput.clearDepth = compiledPass.recordedExecution.clearDepth;
			commandInput.clearStencil = compiledPass.recordedExecution.clearStencil;
			commandInput.clearColorValue = compiledPass.recordedExecution.clearValue;
			return;
		}

		commandInput.renderWidth = compiledPass.outputExecution.renderWidth;
		commandInput.renderHeight = compiledPass.outputExecution.renderHeight;
		commandInput.clearColor = compiledPass.outputExecution.clearColor;
		commandInput.clearDepth = compiledPass.outputExecution.clearDepth;
		commandInput.clearStencil = compiledPass.outputExecution.clearStencil;
		commandInput.clearColorValue = compiledPass.outputExecution.clearValue;
	}

	inline std::optional<size_t> ResolveThreadedPassDependencySourceIndex(
		const std::vector<ThreadedRenderScenePassPlan>& passes,
		const size_t passIndex)
	{
		if (passIndex >= passes.size())
			return std::nullopt;

		const auto& pass = passes[passIndex];
		switch (pass.queueDependencyPolicy)
		{
		case Context::QueueDependencyPolicy::None:
			return std::nullopt;
		case Context::QueueDependencyPolicy::Previous:
			return passIndex > 0u ? std::optional<size_t>(passIndex - 1u) : std::nullopt;
		case Context::QueueDependencyPolicy::LastGraphics:
			for (size_t candidateIndex = passIndex; candidateIndex > 0u; --candidateIndex)
			{
				const size_t sourceIndex = candidateIndex - 1u;
				if (passes[sourceIndex].queueType == RHI::QueueType::Graphics)
					return sourceIndex;
			}
			return std::nullopt;
		case Context::QueueDependencyPolicy::LastCompute:
			for (size_t candidateIndex = passIndex; candidateIndex > 0u; --candidateIndex)
			{
				const size_t sourceIndex = candidateIndex - 1u;
				if (passes[sourceIndex].queueType == RHI::QueueType::Compute)
					return sourceIndex;
			}
			return std::nullopt;
		default:
			return std::nullopt;
		}
	}

	inline std::optional<size_t> ResolveLatestResourceDependencySourceIndex(
		const std::vector<ThreadedRenderScenePassPlan>& passes,
		const size_t passIndex)
	{
		if (passIndex >= passes.size())
			return std::nullopt;

		std::optional<size_t> latestSourceIndex;
		const auto updateLatestSource = [&latestSourceIndex](const size_t sourceIndex)
		{
			if (!latestSourceIndex.has_value() || sourceIndex > *latestSourceIndex)
				latestSourceIndex = sourceIndex;
		};

		const auto isEmptySubresourceRange = [](const RHI::RHISubresourceRange& range)
		{
			return range.mipLevelCount == 0u && range.arrayLayerCount == 0u;
		};

		const auto isCompatibleSubresourceRange =
			[&isEmptySubresourceRange](
				const RHI::RHISubresourceRange& lhs,
				const RHI::RHISubresourceRange& rhs)
		{
			if (isEmptySubresourceRange(lhs) || isEmptySubresourceRange(rhs))
				return true;

			return lhs.baseMipLevel == rhs.baseMipLevel &&
				lhs.mipLevelCount == rhs.mipLevelCount &&
				lhs.baseArrayLayer == rhs.baseArrayLayer &&
				lhs.arrayLayerCount == rhs.arrayLayerCount;
		};

		const auto& targetPass = passes[passIndex];
		for (size_t sourceIndex = 0u; sourceIndex < passIndex; ++sourceIndex)
		{
			const auto& sourcePass = passes[sourceIndex];

			for (const auto& targetAccess : targetPass.commandInput.bufferResourceAccesses)
			{
				if (targetAccess.mode != Context::ResourceAccessMode::Read || targetAccess.buffer == nullptr)
					continue;

				const auto sourceIt = std::find_if(
					sourcePass.commandInput.bufferResourceAccesses.rbegin(),
					sourcePass.commandInput.bufferResourceAccesses.rend(),
					[&targetAccess](const Context::BufferResourceAccess& sourceAccess)
					{
						return sourceAccess.mode == Context::ResourceAccessMode::Write &&
							sourceAccess.buffer == targetAccess.buffer;
					});
				if (sourceIt != sourcePass.commandInput.bufferResourceAccesses.rend())
					updateLatestSource(sourceIndex);
			}

			for (const auto& targetAccess : targetPass.commandInput.textureResourceAccesses)
			{
				if (targetAccess.mode != Context::ResourceAccessMode::Read || targetAccess.texture == nullptr)
					continue;

				const auto sourceIt = std::find_if(
					sourcePass.commandInput.textureResourceAccesses.rbegin(),
					sourcePass.commandInput.textureResourceAccesses.rend(),
					[&targetAccess, &isCompatibleSubresourceRange](const Context::TextureResourceAccess& sourceAccess)
					{
						return sourceAccess.mode == Context::ResourceAccessMode::Write &&
							sourceAccess.texture == targetAccess.texture &&
							isCompatibleSubresourceRange(sourceAccess.subresourceRange, targetAccess.subresourceRange);
					});
				if (sourceIt != sourcePass.commandInput.textureResourceAccesses.rend())
					updateLatestSource(sourceIndex);
			}
		}

		return latestSourceIndex;
	}

	inline bool HasExplicitVisibilityTransitions(const Context::RenderPassCommandInput& commandInput)
	{
		return !commandInput.bufferVisibilityTransitions.empty() ||
			!commandInput.textureVisibilityTransitions.empty();
	}

	inline std::vector<ThreadedRenderSceneDependencyEdge> BuildThreadedRenderSceneResourceDependencyEdges(
		const std::vector<ThreadedRenderScenePassPlan>& passes,
		const size_t passIndex)
	{
		std::vector<ThreadedRenderSceneDependencyEdge> edges;
		if (passIndex >= passes.size())
			return edges;

		const auto isEmptySubresourceRange = [](const RHI::RHISubresourceRange& range)
		{
			return range.mipLevelCount == 0u && range.arrayLayerCount == 0u;
		};

		const auto isCompatibleSubresourceRange =
			[&isEmptySubresourceRange](
				const RHI::RHISubresourceRange& lhs,
				const RHI::RHISubresourceRange& rhs)
		{
			if (isEmptySubresourceRange(lhs) || isEmptySubresourceRange(rhs))
				return true;

			return lhs.baseMipLevel == rhs.baseMipLevel &&
				lhs.mipLevelCount == rhs.mipLevelCount &&
				lhs.baseArrayLayer == rhs.baseArrayLayer &&
				lhs.arrayLayerCount == rhs.arrayLayerCount;
		};

		const auto& targetPass = passes[passIndex];
		for (size_t sourceIndex = 0u; sourceIndex < passIndex; ++sourceIndex)
		{
			const auto& sourcePass = passes[sourceIndex];
			const auto edgeKind = sourcePass.queueType != targetPass.queueType
				? ThreadedRenderSceneDependencyKind::QueueSynchronization
				: ThreadedRenderSceneDependencyKind::ResourceVisibility;

			for (const auto& targetAccess : targetPass.commandInput.bufferResourceAccesses)
			{
				if (targetAccess.mode != Context::ResourceAccessMode::Read || targetAccess.buffer == nullptr)
					continue;

				const auto sourceIt = std::find_if(
					sourcePass.commandInput.bufferResourceAccesses.rbegin(),
					sourcePass.commandInput.bufferResourceAccesses.rend(),
					[&targetAccess](const Context::BufferResourceAccess& sourceAccess)
					{
						return sourceAccess.mode == Context::ResourceAccessMode::Write &&
							sourceAccess.buffer == targetAccess.buffer;
					});
				if (sourceIt == sourcePass.commandInput.bufferResourceAccesses.rend())
					continue;

				ThreadedRenderSceneDependencyEdge edge;
				edge.sourcePassIndex = sourceIndex;
				edge.targetPassIndex = passIndex;
				edge.kind = edgeKind;
				edge.resourceKind = ThreadedRenderSceneDependencyResourceKind::Buffer;
				edge.sourceBufferAccess = *sourceIt;
				edge.targetBufferAccess = targetAccess;
				edges.push_back(std::move(edge));
			}

			for (const auto& targetAccess : targetPass.commandInput.textureResourceAccesses)
			{
				if (targetAccess.mode != Context::ResourceAccessMode::Read || targetAccess.texture == nullptr)
					continue;

				const auto sourceIt = std::find_if(
					sourcePass.commandInput.textureResourceAccesses.rbegin(),
					sourcePass.commandInput.textureResourceAccesses.rend(),
					[&targetAccess, &isCompatibleSubresourceRange](const Context::TextureResourceAccess& sourceAccess)
					{
						return sourceAccess.mode == Context::ResourceAccessMode::Write &&
							sourceAccess.texture == targetAccess.texture &&
							isCompatibleSubresourceRange(sourceAccess.subresourceRange, targetAccess.subresourceRange);
					});
				if (sourceIt == sourcePass.commandInput.textureResourceAccesses.rend())
					continue;

				ThreadedRenderSceneDependencyEdge edge;
				edge.sourcePassIndex = sourceIndex;
				edge.targetPassIndex = passIndex;
				edge.kind = edgeKind;
				edge.resourceKind = ThreadedRenderSceneDependencyResourceKind::Texture;
				edge.sourceTextureAccess = *sourceIt;
				edge.targetTextureAccess = targetAccess;
				edges.push_back(std::move(edge));
			}
		}

		return edges;
	}

	inline std::vector<ThreadedRenderSceneDependencyEdge> BuildThreadedRenderSceneDependencyEdges(
		std::vector<ThreadedRenderScenePassPlan>& passes)
	{
		std::vector<ThreadedRenderSceneDependencyEdge> edges;
		edges.reserve(passes.size());

		for (size_t passIndex = 0u; passIndex < passes.size(); ++passIndex)
		{
			auto& pass = passes[passIndex];
			const auto policyDependencySourcePassIndex = ResolveThreadedPassDependencySourceIndex(passes, passIndex);
			auto resourceEdges = BuildThreadedRenderSceneResourceDependencyEdges(passes, passIndex);
			const auto resourceDependencySourcePassIndex = ResolveLatestResourceDependencySourceIndex(passes, passIndex);
			auto dependencySourcePassIndex = policyDependencySourcePassIndex;
			if (resourceDependencySourcePassIndex.has_value() &&
				(!dependencySourcePassIndex.has_value() || *resourceDependencySourcePassIndex > *dependencySourcePassIndex))
			{
				dependencySourcePassIndex = resourceDependencySourcePassIndex;
			}
			pass.dependencySourcePassIndex = dependencySourcePassIndex;
			pass.requiresDependencyVisibility =
				!resourceEdges.empty() ||
				HasExplicitVisibilityTransitions(pass.commandInput);
			pass.commandInput.requiresDependencyVisibility = pass.requiresDependencyVisibility;
			edges.insert(
				edges.end(),
				std::make_move_iterator(resourceEdges.begin()),
				std::make_move_iterator(resourceEdges.end()));

			if (!policyDependencySourcePassIndex.has_value())
				continue;

			const auto policyDependencyKind = passes[*policyDependencySourcePassIndex].queueType != pass.queueType
				? ThreadedRenderSceneDependencyKind::QueueSynchronization
				: ThreadedRenderSceneDependencyKind::ResourceVisibility;
			const bool alreadyRepresentedByResourceEdge = std::any_of(
				edges.begin(),
				edges.end(),
				[passIndex, policyDependencySourcePassIndex, policyDependencyKind](const ThreadedRenderSceneDependencyEdge& edge)
				{
					return edge.sourcePassIndex == *policyDependencySourcePassIndex &&
						edge.targetPassIndex == passIndex &&
						edge.kind == policyDependencyKind;
				});
			if (alreadyRepresentedByResourceEdge)
				continue;

			ThreadedRenderSceneDependencyEdge edge;
			edge.sourcePassIndex = *policyDependencySourcePassIndex;
			edge.targetPassIndex = passIndex;
			edge.kind = policyDependencyKind;
			edges.push_back(edge);
		}

		return edges;
	}

	inline Context::RenderPassCommandInput MakeCompiledThreadedRenderPassCommandInput(
		const CompiledThreadedRenderSceneGraphPass& compiledPass)
	{
		Context::RenderPassCommandInput commandInput;
		commandInput.kind = compiledPass.metadata.commandKind;
		ApplyCompiledThreadedRenderSceneExecutionToCommandInput(compiledPass, commandInput);
		return commandInput;
	}

	inline ScenePassOutputChain ResolveScenePassOutputChain(
		int32_t previousColor,
		int32_t previousDepth,
		int32_t importedColor,
		int32_t importedDepth)
	{
		ScenePassOutputChain chain;
		chain.color = previousColor >= 0 ? previousColor : importedColor;
		chain.depth = previousDepth >= 0 ? previousDepth : importedDepth;
		chain.requiresSideEffect = chain.color < 0 && chain.depth < 0;
		return chain;
	}

	inline ScenePassOutputChain ResolveScenePassOutputChain(
		const ScenePassOutputResourceState& outputState,
		const ThreadedRenderScenePassMetadata& metadata)
	{
		ScenePassOutputChain chain;
		chain.color = metadata.propagatesColorOutput ? outputState.color : -1;
		chain.depth = metadata.propagatesDepthOutput ? outputState.depth : -1;
		chain.requiresSideEffect = chain.color < 0 && chain.depth < 0;
		return chain;
	}

	inline void AdvanceScenePassOutputResourceState(
		ScenePassOutputResourceState& outputState,
		const ThreadedRenderScenePassMetadata& metadata,
		const FrameGraphResource writtenColor,
		const FrameGraphResource writtenDepth)
	{
		if (metadata.propagatesColorOutput && writtenColor >= 0)
			outputState.color = writtenColor;
		if (metadata.propagatesDepthOutput && writtenDepth >= 0)
			outputState.depth = writtenDepth;
	}

	template<typename TMetadataRange>
	inline std::vector<CompiledThreadedRenderSceneGraphPass> CompileThreadedRenderSceneGraphPasses(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange)
	{
		std::vector<CompiledThreadedRenderSceneGraphPass> compiledPasses;
		compiledPasses.reserve(static_cast<size_t>(std::distance(std::begin(metadataRange), std::end(metadataRange))));

		auto previousColor = -1;
		auto previousDepth = -1;
		for (const auto& metadataEntry : metadataRange)
		{
			const auto& metadata = ResolveThreadedRenderScenePassMetadata(metadataEntry);
			auto& compiledPass = compiledPasses.emplace_back();
			compiledPass.metadata = metadata;
			compiledPass.outputChain = ResolveScenePassOutputChain(
				previousColor,
				previousDepth,
				importedColor,
				importedDepth,
				compiledPass.metadata);
			compiledPass.outputExecution = BuildOutputRenderPassExecutionDesc(frame, compiledPass.metadata);
			compiledPass.recordedExecution = BuildRecordedRenderPassExecutionDesc(
				frame.renderWidth,
				frame.renderHeight,
				compiledPass.metadata);
			previousColor = compiledPass.outputChain.color;
			previousDepth = compiledPass.outputChain.depth;
		}

		return compiledPasses;
	}

	inline OutputRenderPassExecutionDesc BuildOutputRenderPassExecutionDesc(
		const Data::FrameDescriptor& frame,
		const ThreadedRenderScenePassMetadata& metadata)
	{
		OutputRenderPassExecutionDesc desc;
		desc.renderWidth = frame.renderWidth;
		desc.renderHeight = frame.renderHeight;

		if (metadata.execution.useFrameClearState && frame.camera != nullptr)
		{
			desc.clearColor = frame.camera->GetClearColorBuffer();
			desc.clearDepth = frame.camera->GetClearDepthBuffer();
			desc.clearStencil = frame.camera->GetClearStencilBuffer();
			desc.clearValue = {
				frame.camera->GetClearColor().x,
				frame.camera->GetClearColor().y,
				frame.camera->GetClearColor().z,
				1.0f
			};
			return desc;
		}

		desc.clearColor = metadata.execution.clearColor;
		desc.clearDepth = metadata.execution.clearDepth;
		desc.clearStencil = metadata.execution.clearStencil;
		desc.clearValue = metadata.execution.clearValue;
		return desc;
	}

	inline RecordedRenderPassExecutionDesc BuildRecordedRenderPassExecutionDesc(
		uint16_t renderWidth,
		uint16_t renderHeight,
		const ThreadedRenderScenePassMetadata& metadata)
	{
		RecordedRenderPassExecutionDesc desc;
		desc.renderWidth = renderWidth;
		desc.renderHeight = renderHeight;
		desc.clearColor = metadata.execution.clearColor;
		desc.clearDepth = metadata.execution.clearDepth;
		desc.clearStencil = metadata.execution.clearStencil;
		desc.clearValue = metadata.execution.clearValue;
		return desc;
	}

	inline ScenePassOutputChain ResolveScenePassOutputChain(
		int32_t previousColor,
		int32_t previousDepth,
		int32_t importedColor,
		int32_t importedDepth,
		const ThreadedRenderScenePassMetadata& metadata)
	{
		auto chain = ResolveScenePassOutputChain(previousColor, previousDepth, importedColor, importedDepth);
		if (!metadata.propagatesColorOutput)
			chain.color = -1;
		if (!metadata.propagatesDepthOutput)
			chain.depth = -1;
		chain.requiresSideEffect = chain.color < 0 && chain.depth < 0;
		return chain;
	}

	template<typename TPassData, typename TSetupFn, typename TExecuteFn>
	inline const TPassData& AddMetadataCallbackPass(
		::FrameGraph& frameGraph,
		const ThreadedRenderScenePassMetadata& metadata,
		TSetupFn&& setup,
		TExecuteFn&& execute)
	{
		return frameGraph.addCallbackPass<TPassData>(
			metadata.graphPassName,
			std::forward<TSetupFn>(setup),
			std::forward<TExecuteFn>(execute));
	}

	template<typename TPassData, auto TColorMember, auto TDepthMember, typename TSetupFn, typename TExecuteFn>
	inline const TPassData& AddSceneOutputCallbackPass(
		::FrameGraph& frameGraph,
		const ThreadedRenderScenePassMetadata& metadata,
		int32_t previousColor,
		int32_t previousDepth,
		int32_t importedColor,
		int32_t importedDepth,
		TSetupFn&& setup,
		TExecuteFn&& execute)
	{
		return AddMetadataCallbackPass<TPassData>(
			frameGraph,
			metadata,
			[=](::FrameGraph::Builder& builder, TPassData& data)
			{
				setup(builder, data);
				const auto outputChain = ResolveScenePassOutputChain(
					previousColor,
					previousDepth,
					importedColor,
					importedDepth,
					metadata);
				if constexpr (TColorMember != nullptr)
				{
					if (outputChain.color >= 0)
						data.*TColorMember = builder.write(outputChain.color);
				}
				if constexpr (TDepthMember != nullptr)
				{
					if (outputChain.depth >= 0)
						data.*TDepthMember = builder.write(outputChain.depth);
				}
				if (outputChain.requiresSideEffect)
					builder.setSideEffect();
			},
			std::forward<TExecuteFn>(execute));
	}

	template<typename TPassData, auto TColorMember, auto TDepthMember, typename TExecuteFn>
	inline const TPassData& AddSceneOutputCallbackPass(
		::FrameGraph& frameGraph,
		const ThreadedRenderScenePassMetadata& metadata,
		int32_t previousColor,
		int32_t previousDepth,
		int32_t importedColor,
		int32_t importedDepth,
		TExecuteFn&& execute)
	{
		return AddSceneOutputCallbackPass<TPassData, TColorMember, TDepthMember>(
			frameGraph,
			metadata,
			previousColor,
			previousDepth,
			importedColor,
			importedDepth,
			[](::FrameGraph::Builder&, TPassData&)
			{
			},
			std::forward<TExecuteFn>(execute));
	}

	template<typename TBeginFn, typename TDrawFn, typename TEndFn>
	inline void ExecuteOutputRenderPass(
		const OutputRenderPassExecutionDesc& desc,
		TBeginFn&& beginRenderPass,
		TDrawFn&& draw,
		TEndFn&& endRenderPass)
	{
		const bool startedRenderPass = beginRenderPass(desc);
		draw();
		endRenderPass(startedRenderPass, desc);
	}

	template<typename TBeginFn, typename TRecordedFn, typename TEndFn>
	inline void ExecuteRecordedRenderPass(
		const RecordedRenderPassExecutionDesc& desc,
		TBeginFn&& beginRenderPass,
		TRecordedFn&& drawRecorded,
		TEndFn&& endRenderPass)
	{
		const bool startedRenderPass = beginRenderPass(desc);
		if (!startedRenderPass)
			return;

		drawRecorded();
		endRenderPass();
	}

	template<typename TPassData, typename TSetupFn, typename TExecuteFn>
	inline const TPassData& AddExecutingMetadataPass(
		::FrameGraph& frameGraph,
		const ThreadedRenderScenePassMetadata& metadata,
		uint16_t renderWidth,
		uint16_t renderHeight,
		TSetupFn&& setup,
		TExecuteFn&& execute)
	{
		return AddMetadataCallbackPass<TPassData>(
			frameGraph,
			metadata,
			std::forward<TSetupFn>(setup),
			[=](const TPassData& data, ::FrameGraphPassResources& resources, void* context)
			{
				const auto desc = BuildRecordedRenderPassExecutionDesc(renderWidth, renderHeight, metadata);
				execute(data, resources, context, desc);
			});
	}

	template<typename TPassData, typename TSetupFn, typename TExecuteFn>
	inline const TPassData& AddExecutingMetadataPass(
		::FrameGraph& frameGraph,
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		TSetupFn&& setup,
		TExecuteFn&& execute)
	{
		return AddMetadataCallbackPass<TPassData>(
			frameGraph,
			compiledPass.metadata,
			std::forward<TSetupFn>(setup),
			[=](const TPassData& data, ::FrameGraphPassResources& resources, void* context)
			{
				execute(data, resources, context, compiledPass.recordedExecution);
			});
	}

	template<typename TPassData, auto TColorMember, auto TDepthMember, typename TSetupFn, typename TExecuteFn>
	inline const TPassData& AddExecutingSceneOutputPass(
		::FrameGraph& frameGraph,
		const Data::FrameDescriptor& frame,
		const ThreadedRenderScenePassMetadata& metadata,
		int32_t previousColor,
		int32_t previousDepth,
		int32_t importedColor,
		int32_t importedDepth,
		TSetupFn&& setup,
		TExecuteFn&& execute)
	{
		return AddSceneOutputCallbackPass<TPassData, TColorMember, TDepthMember>(
			frameGraph,
			metadata,
			previousColor,
			previousDepth,
			importedColor,
			importedDepth,
			std::forward<TSetupFn>(setup),
			[=, &frame](const TPassData& data, ::FrameGraphPassResources& resources, void* context)
			{
				const auto desc = BuildOutputRenderPassExecutionDesc(frame, metadata);
				execute(data, resources, context, desc);
			});
	}

	template<typename TPassData, auto TColorMember, auto TDepthMember, typename TSetupFn, typename TExecuteFn>
	inline const TPassData& AddExecutingSceneOutputPass(
		::FrameGraph& frameGraph,
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		ScenePassOutputResourceState& outputState,
		TSetupFn&& setup,
		TExecuteFn&& execute)
	{
		return AddMetadataCallbackPass<TPassData>(
			frameGraph,
			compiledPass.metadata,
			[&outputState, compiledPass, setup = std::forward<TSetupFn>(setup)](::FrameGraph::Builder& builder, TPassData& data)
			{
				const auto outputChain = ResolveScenePassOutputChain(outputState, compiledPass.metadata);
				FrameGraphResource writtenColor = -1;
				FrameGraphResource writtenDepth = -1;

				setup(builder, data);
				if constexpr (TColorMember != nullptr)
				{
					if (outputChain.color >= 0)
					{
						writtenColor = builder.write(outputChain.color);
						data.*TColorMember = writtenColor;
					}
				}
				else if (outputChain.color >= 0)
				{
					writtenColor = builder.write(outputChain.color);
				}

				if constexpr (TDepthMember != nullptr)
				{
					if (outputChain.depth >= 0)
					{
						writtenDepth = builder.write(outputChain.depth);
						data.*TDepthMember = writtenDepth;
					}
				}
				else if (outputChain.depth >= 0)
				{
					writtenDepth = builder.write(outputChain.depth);
				}

				AdvanceScenePassOutputResourceState(
					outputState,
					compiledPass.metadata,
					writtenColor,
					writtenDepth);

				if (outputChain.requiresSideEffect)
					builder.setSideEffect();
			},
			[=](const TPassData& data, ::FrameGraphPassResources& resources, void* context)
			{
				execute(data, resources, context, compiledPass.outputExecution);
			});
	}

	template<typename TPassData, auto TColorMember, auto TDepthMember, typename TExecuteFn>
	inline const TPassData& AddExecutingSceneOutputPass(
		::FrameGraph& frameGraph,
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		ScenePassOutputResourceState& outputState,
		TExecuteFn&& execute)
	{
		return AddExecutingSceneOutputPass<TPassData, TColorMember, TDepthMember>(
			frameGraph,
			compiledPass,
			outputState,
			[](::FrameGraph::Builder&, TPassData&)
			{
			},
			std::forward<TExecuteFn>(execute));
	}

	template<typename TPassData, auto TColorMember, auto TDepthMember, typename TExecuteFn>
	inline const TPassData& AddExecutingSceneOutputPass(
		::FrameGraph& frameGraph,
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		TExecuteFn&& execute)
	{
		ScenePassOutputResourceState outputState;
		outputState.color = compiledPass.outputChain.color;
		outputState.depth = compiledPass.outputChain.depth;
		return AddExecutingSceneOutputPass<TPassData, TColorMember, TDepthMember>(
			frameGraph,
			compiledPass,
			outputState,
			[](::FrameGraph::Builder&, TPassData&)
			{
			},
			std::forward<TExecuteFn>(execute));
	}

	template<typename TPassData, auto TColorMember, auto TDepthMember, typename TExecuteFn>
	inline const TPassData& AddExecutingSceneOutputPass(
		::FrameGraph& frameGraph,
		const Data::FrameDescriptor& frame,
		const ThreadedRenderScenePassMetadata& metadata,
		int32_t previousColor,
		int32_t previousDepth,
		int32_t importedColor,
		int32_t importedDepth,
		TExecuteFn&& execute)
	{
		return AddExecutingSceneOutputPass<TPassData, TColorMember, TDepthMember>(
			frameGraph,
			frame,
			metadata,
			previousColor,
			previousDepth,
			importedColor,
			importedDepth,
			[](::FrameGraph::Builder&, TPassData&)
			{
			},
			std::forward<TExecuteFn>(execute));
	}

	template<typename TMetadataRange>
	inline ThreadedRenderSceneExecutionPlan BuildThreadedRenderSceneExecutionPlan(
		const std::vector<Context::RenderPassCommandInput>& passInputs,
		const TMetadataRange& metadataRange)
	{
		ThreadedRenderSceneExecutionPlan plan;
		plan.passes.reserve(passInputs.size());
		const auto metadataCount = static_cast<size_t>(std::distance(std::begin(metadataRange), std::end(metadataRange)));
		const bool usePositionalMapping = metadataCount == passInputs.size();

		for (size_t passIndex = 0; passIndex < passInputs.size(); ++passIndex)
		{
			const auto& passInput = passInputs[passIndex];
			auto metadataIt = std::begin(metadataRange);
			if (usePositionalMapping)
			{
				std::advance(metadataIt, static_cast<std::ptrdiff_t>(passIndex));
			}
			else
			{
				for (; metadataIt != std::end(metadataRange); ++metadataIt)
				{
					const auto& metadata = ResolveThreadedRenderScenePassMetadata(*metadataIt);
					if (metadata.commandKind == passInput.kind)
						break;
				}
			}

			if (metadataIt == std::end(metadataRange))
				throw std::invalid_argument("Missing ThreadedRenderScenePassMetadata for pass input kind");

			auto plannedCommandInput = passInput;
			const auto& metadata = ResolveThreadedRenderScenePassMetadata(*metadataIt);
			if (plannedCommandInput.debugName.empty())
				plannedCommandInput.debugName = metadata.graphPassName;
			const bool hasExplicitClearState =
				passInput.clearColor || passInput.clearDepth || passInput.clearStencil;
			const auto explicitClearColor = passInput.clearColor;
			const auto explicitClearDepth = passInput.clearDepth;
			const auto explicitClearStencil = passInput.clearStencil;
			const auto explicitClearValue = passInput.clearColorValue;
			ApplyCompiledThreadedRenderSceneExecutionToCommandInput(*metadataIt, plannedCommandInput);
			if (hasExplicitClearState)
			{
				plannedCommandInput.clearColor = explicitClearColor;
				plannedCommandInput.clearDepth = explicitClearDepth;
				plannedCommandInput.clearStencil = explicitClearStencil;
				plannedCommandInput.clearColorValue = explicitClearValue;
			}

			plan.passes.push_back({
				std::move(plannedCommandInput),
				metadata.role,
				metadata.queueType,
				metadata.queueDependencyPolicy,
				std::nullopt,
				false,
				metadata.graphPassName,
				metadata.visibleDrawCountContribution == kThreadedPlanUsePassDrawCount
					? passInput.drawCount
					: metadata.visibleDrawCountContribution
			});
		}

		plan.dependencies = BuildThreadedRenderSceneDependencyEdges(plan.passes);

		return plan;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const std::vector<Context::RenderPassCommandInput>& passInputs,
		const TMetadataRange& metadataRange)
	{
		CompiledThreadedRenderSceneExecution compiledExecution;
		compiledExecution.graphPasses = CompileThreadedRenderSceneGraphPasses(
			frame,
			importedColor,
			importedDepth,
			metadataRange);
		compiledExecution.threadedPlan = BuildThreadedRenderSceneExecutionPlan(
			passInputs,
			compiledExecution.graphPasses);
		return compiledExecution;
	}

	template<
		typename TMetadataRange,
		typename TBuildPassInputsFn,
		typename = std::enable_if_t<
			std::is_invocable_v<TBuildPassInputsFn&, const std::vector<CompiledThreadedRenderSceneGraphPass>&>>>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		CompiledThreadedRenderSceneExecution compiledExecution;
		compiledExecution.graphPasses = CompileThreadedRenderSceneGraphPasses(
			frame,
			importedColor,
			importedDepth,
			metadataRange);
		const auto passInputs = buildPassInputs(compiledExecution.graphPasses);
		compiledExecution.threadedPlan = BuildThreadedRenderSceneExecutionPlan(
			passInputs,
			compiledExecution.graphPasses);
		return compiledExecution;
	}

	template<
		typename TMetadataRange,
		typename TBuildPassInputsFn,
		typename = std::enable_if_t<
			std::is_invocable_v<TBuildPassInputsFn&, const std::vector<CompiledThreadedRenderSceneGraphPass>&>>>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const PreparedComputeDispatchSource& preparedComputeSource,
		const TMetadataRange& scenePassMetadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		return CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			BuildPreparedComputeAndScenePassMetadata(preparedComputeSource, scenePassMetadataRange),
			std::forward<TBuildPassInputsFn>(buildPassInputs));
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const PreparedComputeDispatchSource& preparedComputeSource,
		const TMetadataRange& scenePassMetadataRange,
		const std::vector<Context::RenderPassCommandInput>& passInputs)
	{
		return CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			BuildPreparedComputeAndScenePassMetadata(preparedComputeSource, scenePassMetadataRange),
			[&preparedComputeSource, &passInputs](const auto& compiledPasses)
			{
				return BuildPreparedComputeAndScenePassInputs(
					preparedComputeSource,
					compiledPasses,
					passInputs);
			});
	}

	template<
		typename TMetadataRange,
		typename TBuildPassInputsFn,
		typename = std::enable_if_t<
			std::is_invocable_v<TBuildPassInputsFn&, const std::vector<CompiledThreadedRenderSceneGraphPass>&>>>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const std::vector<Context::RecordedComputeDispatchInput>& preparedComputeDispatchInputs,
		const TMetadataRange& scenePassMetadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		return CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			BuildPreparedComputeAndScenePassMetadata(preparedComputeDispatchInputs, scenePassMetadataRange),
			std::forward<TBuildPassInputsFn>(buildPassInputs));
	}

	template<
		typename TPreparedComputeSourceFactoryFn,
		typename TMetadataRange,
		typename TBuildPassInputsFn,
		typename = std::enable_if_t<std::is_invocable_v<TPreparedComputeSourceFactoryFn&>>>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		TPreparedComputeSourceFactoryFn&& buildPreparedComputeSource,
		const TMetadataRange& scenePassMetadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		auto preparedComputeSource = buildPreparedComputeSource();
		return CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			preparedComputeSource,
			scenePassMetadataRange,
			[&preparedComputeSource, &buildPassInputs](const auto& compiledPasses)
			{
				return buildPassInputs(preparedComputeSource, compiledPasses);
			});
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange)
	{
		CompiledThreadedRenderSceneExecution compiledExecution;
		compiledExecution.graphPasses = CompileThreadedRenderSceneGraphPasses(
			frame,
			importedColor,
			importedDepth,
			metadataRange);
		return compiledExecution;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const PreparedComputeDispatchSource& preparedComputeSource,
		const TMetadataRange& scenePassMetadataRange)
	{
		return CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			BuildPreparedComputeAndScenePassMetadata(preparedComputeSource, scenePassMetadataRange));
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const std::vector<Context::RecordedComputeDispatchInput>& preparedComputeDispatchInputs,
		const TMetadataRange& scenePassMetadataRange)
	{
		return CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			BuildPreparedComputeAndScenePassMetadata(preparedComputeDispatchInputs, scenePassMetadataRange));
	}

	template<
		typename TPreparedComputeSourceFactoryFn,
		typename TMetadataRange,
		typename = std::enable_if_t<std::is_invocable_v<TPreparedComputeSourceFactoryFn&>>>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		TPreparedComputeSourceFactoryFn&& buildPreparedComputeSource,
		const TMetadataRange& scenePassMetadataRange)
	{
		auto preparedComputeSource = buildPreparedComputeSource();
		return CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			preparedComputeSource,
			scenePassMetadataRange);
	}

	inline void ApplyThreadedRenderSceneExecutionPlan(
		Context::RenderScenePackage& package,
		const ThreadedRenderSceneExecutionPlan& plan,
		std::vector<Context::ParallelCommandWorkUnit> additionalWorkUnits = {})
	{
		package.passCommandInputs.clear();
		package.passCommandInputs.reserve(plan.passes.size());
		package.parallelCommandWorkUnits.clear();
		package.parallelCommandWorkUnits.reserve(additionalWorkUnits.size() + plan.passes.size());
		package.workUnitDependencyEdges.clear();

		package.visibleDrawCount = 0u;
		package.opaqueDrawCount = 0u;
		package.transparentDrawCount = 0u;
		package.skyboxDrawCount = 0u;
		package.helperDrawCount = 0u;
		package.hasOpaquePass = false;
		package.hasTransparentPass = false;
		package.hasSkyboxPass = false;
		package.hasHelperPass = false;

		for (auto& workUnit : additionalWorkUnits)
			package.parallelCommandWorkUnits.push_back(std::move(workUnit));

		const uint64_t passWorkUnitBaseIndex = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
		for (const auto& plannedPass : plan.passes)
		{
			auto passCommandInput = plannedPass.commandInput;
			passCommandInput.dependencySourceWorkUnitIndex.reset();
			passCommandInput.requiresDependencyVisibility = plannedPass.requiresDependencyVisibility;

			package.passCommandInputs.push_back(passCommandInput);
			Context::ParallelCommandWorkUnit workUnit;
			workUnit.debugName = !passCommandInput.debugName.empty()
				? passCommandInput.debugName
				: std::string(plannedPass.graphPassName);
			workUnit.commandInput = std::move(passCommandInput);
			package.parallelCommandWorkUnits.push_back(std::move(workUnit));
			package.visibleDrawCount += plannedPass.visibleDrawCountContribution;

			switch (plannedPass.role)
			{
			case ThreadedRenderScenePassRole::Opaque:
				package.opaqueDrawCount += plannedPass.commandInput.drawCount;
				package.hasOpaquePass = true;
				break;
			case ThreadedRenderScenePassRole::Transparent:
				package.transparentDrawCount += plannedPass.commandInput.drawCount;
				package.hasTransparentPass = true;
				break;
			case ThreadedRenderScenePassRole::Skybox:
				package.skyboxDrawCount += plannedPass.commandInput.drawCount;
				package.hasSkyboxPass = true;
				break;
			case ThreadedRenderScenePassRole::Helper:
				package.helperDrawCount += plannedPass.commandInput.drawCount;
				package.hasHelperPass = true;
				break;
			case ThreadedRenderScenePassRole::Auxiliary:
				break;
			}
		}

		for (size_t index = 0; index < package.parallelCommandWorkUnits.size(); ++index)
		{
			auto& workUnit = package.parallelCommandWorkUnits[index];
			workUnit.workUnitIndex = static_cast<uint64_t>(index);
			workUnit.submissionOrder = workUnit.workUnitIndex;
			workUnit.incomingDependencyEdges.clear();
		}

		package.workUnitDependencyEdges.reserve(plan.dependencies.size());
		for (const auto& dependency : plan.dependencies)
		{
			Context::WorkUnitDependencyEdge workUnitDependency;
			workUnitDependency.sourceWorkUnitIndex =
				passWorkUnitBaseIndex + static_cast<uint64_t>(dependency.sourcePassIndex);
			workUnitDependency.targetWorkUnitIndex =
				passWorkUnitBaseIndex + static_cast<uint64_t>(dependency.targetPassIndex);
			workUnitDependency.kind = dependency.kind;
			workUnitDependency.resourceKind = dependency.resourceKind;
			workUnitDependency.sourceBufferAccess = dependency.sourceBufferAccess;
			workUnitDependency.targetBufferAccess = dependency.targetBufferAccess;
			workUnitDependency.sourceTextureAccess = dependency.sourceTextureAccess;
			workUnitDependency.targetTextureAccess = dependency.targetTextureAccess;
			package.workUnitDependencyEdges.push_back(workUnitDependency);

			if (workUnitDependency.targetWorkUnitIndex < package.parallelCommandWorkUnits.size())
			{
				package.parallelCommandWorkUnits[static_cast<size_t>(workUnitDependency.targetWorkUnitIndex)]
					.incomingDependencyEdges.push_back(std::move(workUnitDependency));
			}
		}

		package.passPlanCount = static_cast<uint64_t>(package.passCommandInputs.size());
		package.hasVisibleDraws = package.visibleDrawCount > 0u;
		package.drawCommandCount = !package.recordedDrawCommands.empty()
			? static_cast<uint64_t>(package.recordedDrawCommands.size())
			: package.visibleDrawCount;
		package.materialBatchCount = package.drawCommandCount;
		package.renderTargetUseCount = package.targetsSwapchain ? 1u : 2u;
		package.containsCommandInputs = !package.passCommandInputs.empty();
		package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
		package.containsParallelCommandWorkUnits = !package.parallelCommandWorkUnits.empty();
		package.computeDispatchInputs.clear();
		package.containsComputeDispatchInputs = false;
		package.asyncComputeWorkloadCount = static_cast<uint64_t>(std::count_if(
			package.parallelCommandWorkUnits.begin(),
			package.parallelCommandWorkUnits.end(),
			[](const Context::ParallelCommandWorkUnit& workUnit)
			{
				return workUnit.commandInput.queueType == RHI::QueueType::Compute;
			}));
		package.hasAsyncComputeWorkload = package.asyncComputeWorkloadCount > 0u;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			package.passCommandInputs,
			metadataRange);
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<
		typename TPreparedComputeInputFactoryFn,
		typename TMetadataRange,
		typename TBuildPassInputsFn,
		typename = std::enable_if_t<
			std::is_invocable_v<TPreparedComputeInputFactoryFn&> &&
			std::is_same_v<
				std::decay_t<std::invoke_result_t<TPreparedComputeInputFactoryFn&>>,
				PreparedComputeCompilationInput>>>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		TPreparedComputeInputFactoryFn&& buildPreparedComputeInput,
		const TMetadataRange& scenePassMetadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		auto preparedComputeInput = buildPreparedComputeInput();
		if (preparedComputeInput.applyToPackage)
			preparedComputeInput.applyToPackage(package);

		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			preparedComputeInput.source,
			scenePassMetadataRange,
			[&preparedComputeInput, &buildPassInputs](const auto& compiledPasses)
			{
				return buildPassInputs(preparedComputeInput.source, compiledPasses);
			});
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<
		typename TPreparedComputeSourceFactoryFn,
		typename TPreparedComputePackageMutatorFn,
		typename TMetadataRange,
		typename = std::enable_if_t<
			std::is_invocable_v<TPreparedComputeSourceFactoryFn&> &&
			std::is_same_v<
				std::decay_t<std::invoke_result_t<TPreparedComputeSourceFactoryFn&>>,
				PreparedComputeDispatchSource> &&
			std::is_invocable_v<TPreparedComputePackageMutatorFn&, Context::RenderScenePackage&>>>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		TPreparedComputeSourceFactoryFn&& buildPreparedComputeSource,
		TPreparedComputePackageMutatorFn&& mutatePackageForPreparedCompute,
		const TMetadataRange& scenePassMetadataRange)
	{
		auto preparedComputeSource = buildPreparedComputeSource();
		mutatePackageForPreparedCompute(package);

		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			preparedComputeSource,
			scenePassMetadataRange,
			package.passCommandInputs);
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<
		typename TPreparedComputeSourceFactoryFn,
		typename TPreparedComputePackageMutatorFn,
		typename TMetadataRange,
		typename TBuildPassInputsFn,
		typename = std::enable_if_t<
			std::is_invocable_v<TPreparedComputeSourceFactoryFn&> &&
			std::is_same_v<
				std::decay_t<std::invoke_result_t<TPreparedComputeSourceFactoryFn&>>,
				PreparedComputeDispatchSource> &&
			std::is_invocable_v<TPreparedComputePackageMutatorFn&, Context::RenderScenePackage&>>>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		TPreparedComputeSourceFactoryFn&& buildPreparedComputeSource,
		TPreparedComputePackageMutatorFn&& mutatePackageForPreparedCompute,
		const TMetadataRange& scenePassMetadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		auto preparedComputeSource = buildPreparedComputeSource();
		mutatePackageForPreparedCompute(package);

		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			preparedComputeSource,
			scenePassMetadataRange,
			[&preparedComputeSource, &buildPassInputs](const auto& compiledPasses)
			{
				return buildPassInputs(preparedComputeSource, compiledPasses);
			});
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const PreparedComputeDispatchSource& preparedComputeSource,
		const TMetadataRange& scenePassMetadataRange)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			preparedComputeSource,
			scenePassMetadataRange,
			package.passCommandInputs);
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const std::vector<Context::RecordedComputeDispatchInput>& preparedComputeDispatchInputs,
		const TMetadataRange& scenePassMetadataRange)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			BuildPreparedComputeDispatchSource(preparedComputeDispatchInputs),
			scenePassMetadataRange,
			package.passCommandInputs);
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange,
		std::vector<Context::ParallelCommandWorkUnit> additionalWorkUnits)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			package.passCommandInputs,
			metadataRange);
		ApplyThreadedRenderSceneExecutionPlan(
			package,
			compiledExecution.threadedPlan,
			std::move(additionalWorkUnits));
		return compiledExecution;
	}

	template<typename TMetadataRange, typename TBuildPassInputsFn>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			metadataRange,
			std::forward<TBuildPassInputsFn>(buildPassInputs));
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<typename TMetadataRange, typename TBuildPassInputsFn>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const PreparedComputeDispatchSource& preparedComputeSource,
		const TMetadataRange& scenePassMetadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			preparedComputeSource,
			scenePassMetadataRange,
			std::forward<TBuildPassInputsFn>(buildPassInputs));
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<typename TMetadataRange, typename TBuildPassInputsFn>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const std::vector<Context::RecordedComputeDispatchInput>& preparedComputeDispatchInputs,
		const TMetadataRange& scenePassMetadataRange,
		TBuildPassInputsFn&& buildPassInputs)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			BuildPreparedComputeAndScenePassMetadata(preparedComputeDispatchInputs, scenePassMetadataRange),
			std::forward<TBuildPassInputsFn>(buildPassInputs));
		ApplyThreadedRenderSceneExecutionPlan(package, compiledExecution.threadedPlan);
		return compiledExecution;
	}

	template<typename TMetadataRange, typename TBuildPassInputsFn>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange,
		TBuildPassInputsFn&& buildPassInputs,
		std::vector<Context::ParallelCommandWorkUnit> additionalWorkUnits)
	{
		auto compiledExecution = CompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			metadataRange,
			std::forward<TBuildPassInputsFn>(buildPassInputs));
		ApplyThreadedRenderSceneExecutionPlan(
			package,
			compiledExecution.threadedPlan,
			std::move(additionalWorkUnits));
		return compiledExecution;
	}
}
