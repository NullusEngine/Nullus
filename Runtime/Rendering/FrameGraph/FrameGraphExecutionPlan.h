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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "Rendering/FrameGraph/FrameGraphExecutionTypes.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"

namespace NLS::Render::FrameGraph
{
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

	inline bool RDGPassMustExecute(const RDGPassContract& pass)
	{
		return pass.hasSideEffect || !pass.declaredResourceNames.empty();
	}

	inline bool RDGPassCanCull(const RDGPassContract& pass)
	{
		return !RDGPassMustExecute(pass);
	}

	inline std::string MakeRDGBufferResourceName(
		const Context::BufferResourceAccess& access,
		const size_t index)
	{
		std::ostringstream stream;
		stream << "Buffer[" << index << "]";
		if (access.buffer != nullptr && !access.buffer->GetDebugName().empty())
			stream << ":" << access.buffer->GetDebugName();
		return stream.str();
	}

	inline std::string MakeRDGTextureResourceName(
		const Context::TextureResourceAccess& access,
		const size_t index)
	{
		std::ostringstream stream;
		stream << "Texture[" << index << "]";
		if (access.texture != nullptr && !access.texture->GetDebugName().empty())
			stream << ":" << access.texture->GetDebugName();
		return stream.str();
	}

	inline RDGPassContract BuildRDGPassContract(
		const Context::RenderPassCommandInput& passInput,
		const ThreadedRenderScenePassMetadata& metadata)
	{
		RDGPassContract contract;
		contract.name = metadata.graphPassName != nullptr && metadata.graphPassName[0] != '\0'
			? std::string(metadata.graphPassName)
			: passInput.debugName;
		contract.queueType = metadata.queueType;
		contract.hasSideEffect = !metadata.propagatesColorOutput && !metadata.propagatesDepthOutput;

		contract.declaredResourceNames.reserve(
			passInput.bufferResourceAccesses.size() + passInput.textureResourceAccesses.size());
		for (size_t index = 0u; index < passInput.bufferResourceAccesses.size(); ++index)
			contract.declaredResourceNames.push_back(MakeRDGBufferResourceName(passInput.bufferResourceAccesses[index], index));
		for (size_t index = 0u; index < passInput.textureResourceAccesses.size(); ++index)
			contract.declaredResourceNames.push_back(MakeRDGTextureResourceName(passInput.textureResourceAccesses[index], index));

		contract.usedResourceNames = contract.declaredResourceNames;
		return contract;
	}

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

	inline void RecordTextureVisibilityTransitions(
		FrameGraphExecutionContext& executionContext,
		const std::vector<Context::TextureVisibilityTransition>& transitions)
	{
		if (transitions.empty())
			return;

		RHI::RHIBarrierDesc barriers;
		barriers.textureBarriers.reserve(transitions.size());
		for (const auto& transition : transitions)
		{
			if (transition.texture == nullptr)
				continue;

			barriers.textureBarriers.push_back({
				transition.texture,
				transition.before,
				transition.after,
				transition.subresourceRange,
				transition.sourceStages,
				transition.destinationStages,
				transition.sourceAccess,
				transition.destinationAccess
			});
		}

		if (barriers.textureBarriers.empty())
			return;

		if (executionContext.CanTrackExplicitResourceState())
		{
			executionContext.RecordResourceBarriers(barriers);
			return;
		}

		executionContext.commandBuffer->Barrier(barriers);
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

		RecordTextureVisibilityTransitions(executionContext, dispatchInput.textureVisibilityTransitionsBefore);

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

		RecordTextureVisibilityTransitions(executionContext, dispatchInput.exportedTextureVisibilityTransitions);
	}

	inline void PopulatePreparedComputeDispatchThreadedPassInput(
		const CompiledThreadedRenderSceneGraphPass& compiledPass,
		const Context::RecordedComputeDispatchInput& dispatchInput,
		Context::RenderPassCommandInput& passInput)
	{
		passInput.kind = compiledPass.metadata.commandKind;
		passInput.queueType = compiledPass.metadata.queueType;
		passInput.queueDependencyPolicy = compiledPass.metadata.queueDependencyPolicy;
		passInput.debugName = compiledPass.metadata.graphPassName;
		passInput.renderWidth = compiledPass.outputExecution.renderWidth;
		passInput.renderHeight = compiledPass.outputExecution.renderHeight;
		passInput.clearColor = compiledPass.outputExecution.clearColor;
		passInput.clearDepth = compiledPass.outputExecution.clearDepth;
		passInput.clearStencil = compiledPass.outputExecution.clearStencil;
		passInput.clearColorValue = compiledPass.outputExecution.clearValue;
		passInput.computeDispatchInputs.push_back(dispatchInput);
		passInput.bufferResourceAccesses.insert(
			passInput.bufferResourceAccesses.end(),
			dispatchInput.bufferResourceAccesses.begin(),
			dispatchInput.bufferResourceAccesses.end());
		passInput.textureResourceAccesses.insert(
			passInput.textureResourceAccesses.end(),
			dispatchInput.textureResourceAccesses.begin(),
			dispatchInput.textureResourceAccesses.end());
		passInput.textureVisibilityTransitions.insert(
			passInput.textureVisibilityTransitions.end(),
			dispatchInput.textureVisibilityTransitionsBefore.begin(),
			dispatchInput.textureVisibilityTransitionsBefore.end());
		passInput.exportedTextureVisibilityTransitions.insert(
			passInput.exportedTextureVisibilityTransitions.end(),
			dispatchInput.exportedTextureVisibilityTransitions.begin(),
			dispatchInput.exportedTextureVisibilityTransitions.end());

		for (const auto& buffer : dispatchInput.shaderReadBuffersBefore)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferVisibilityTransitions.push_back({
				buffer,
				RHI::ResourceState::Unknown,
				RHI::ResourceState::ShaderRead,
				RHI::PipelineStageMask::AllCommands,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::MemoryRead | RHI::AccessMask::MemoryWrite,
				RHI::AccessMask::ShaderRead
			});
		}
		for (const auto& buffer : dispatchInput.shaderWriteBuffersBefore)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferVisibilityTransitions.push_back({
				buffer,
				RHI::ResourceState::Unknown,
				RHI::ResourceState::ShaderWrite,
				RHI::PipelineStageMask::AllCommands,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::MemoryRead | RHI::AccessMask::MemoryWrite,
				RHI::AccessMask::ShaderWrite
			});
		}
		for (const auto& buffer : dispatchInput.uavBarrierBuffersAfter)
		{
			if (buffer == nullptr)
				continue;

			passInput.bufferVisibilityTransitions.push_back({
				buffer,
				RHI::ResourceState::ShaderWrite,
				RHI::ResourceState::ShaderWrite,
				RHI::PipelineStageMask::ComputeShader,
				RHI::PipelineStageMask::ComputeShader,
				RHI::AccessMask::ShaderWrite,
				RHI::AccessMask::ShaderRead | RHI::AccessMask::ShaderWrite
			});
		}
		for (const auto& buffer : dispatchInput.shaderReadBuffersAfter)
		{
			if (buffer == nullptr)
				continue;

			passInput.exportedBufferVisibilityTransitions.push_back({
				buffer,
				RHI::ResourceState::ShaderWrite,
				RHI::ResourceState::ShaderRead,
				RHI::PipelineStageMask::ComputeShader,
				RHI::PipelineStageMask::FragmentShader,
				RHI::AccessMask::ShaderWrite,
				RHI::AccessMask::ShaderRead
			});
		}
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

		PopulatePreparedComputeDispatchThreadedPassInput(compiledPass, *dispatchInput, passInput);
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

		PopulatePreparedComputeDispatchThreadedPassInput(compiledPass, *dispatchInput, passInput);
		return true;
	}

	inline Context::RenderPassCommandInput MakeCompiledThreadedRenderPassCommandInput(
		const CompiledThreadedRenderSceneGraphPass& compiledPass);

	inline const char* ToFrameGraphCompileDiagnosticCodeName(const FrameGraphCompileDiagnosticCode code)
	{
		switch (code)
		{
		case FrameGraphCompileDiagnosticCode::EmptyGraphPassName:
			return "EmptyGraphPassName";
		case FrameGraphCompileDiagnosticCode::MissingPassMetadata:
			return "MissingPassMetadata";
		case FrameGraphCompileDiagnosticCode::NullBufferResourceAccess:
			return "NullBufferResourceAccess";
		case FrameGraphCompileDiagnosticCode::NullTextureResourceAccess:
			return "NullTextureResourceAccess";
		case FrameGraphCompileDiagnosticCode::NullBufferVisibilityTransition:
			return "NullBufferVisibilityTransition";
		case FrameGraphCompileDiagnosticCode::NullTextureVisibilityTransition:
			return "NullTextureVisibilityTransition";
		case FrameGraphCompileDiagnosticCode::RecordedPassPropagatesOutput:
			return "RecordedPassPropagatesOutput";
		case FrameGraphCompileDiagnosticCode::ComputePassUsesNonComputeQueue:
			return "ComputePassUsesNonComputeQueue";
		case FrameGraphCompileDiagnosticCode::ComputePassPropagatesOutput:
			return "ComputePassPropagatesOutput";
		case FrameGraphCompileDiagnosticCode::MissingQueueDependencySource:
			return "MissingQueueDependencySource";
		case FrameGraphCompileDiagnosticCode::ConflictingBufferResourceAccess:
			return "ConflictingBufferResourceAccess";
		case FrameGraphCompileDiagnosticCode::ConflictingTextureResourceAccess:
			return "ConflictingTextureResourceAccess";
		case FrameGraphCompileDiagnosticCode::InvalidQueueDependency:
			return "InvalidQueueDependency";
		case FrameGraphCompileDiagnosticCode::UndeclaredRDGResourceUse:
			return "UndeclaredRDGResourceUse";
		default:
			return "Unknown";
		}
	}

	inline bool ThreadedPassMetadataHasGraphPassName(const ThreadedRenderScenePassMetadata& metadata)
	{
		return metadata.graphPassName != nullptr && metadata.graphPassName[0] != '\0';
	}

	inline bool ThreadedPassMetadataMatchesInput(
		const ThreadedRenderScenePassMetadata& metadata,
		const Context::RenderPassCommandInput& passInput)
	{
		if (!passInput.debugName.empty() && ThreadedPassMetadataHasGraphPassName(metadata))
			return passInput.debugName == metadata.graphPassName;

		return metadata.commandKind == passInput.kind;
	}

	inline bool IsRecordedThreadedPassOutputContractInvalid(const ThreadedRenderScenePassMetadata& metadata)
	{
		return metadata.executionMode == ThreadedRenderScenePassExecutionMode::Recorded &&
			(metadata.propagatesColorOutput || metadata.propagatesDepthOutput);
	}

	inline bool IsComputeThreadedPassMetadata(const ThreadedRenderScenePassMetadata& metadata)
	{
		return metadata.commandKind == Context::RenderPassCommandKind::Compute;
	}

	template<typename TMetadataRange>
	inline bool HasThreadedPassMetadataForInput(
		const TMetadataRange& metadataRange,
		const Context::RenderPassCommandInput& passInput,
		const size_t passIndex,
		const bool usePositionalMapping)
	{
		if (usePositionalMapping)
			return passIndex < static_cast<size_t>(std::distance(std::begin(metadataRange), std::end(metadataRange)));

		return std::any_of(
			std::begin(metadataRange),
			std::end(metadataRange),
			[&passInput](const auto& metadataEntry)
			{
				return ThreadedPassMetadataMatchesInput(
					ResolveThreadedRenderScenePassMetadata(metadataEntry),
					passInput);
			});
	}

	inline void AddFrameGraphCompileDiagnostic(
		FrameGraphCompileValidationResult& result,
		const FrameGraphCompileDiagnosticCode code,
		const size_t passIndex,
		const Context::RenderPassCommandKind passKind,
		std::string message)
	{
		result.diagnostics.push_back({
			FrameGraphCompileDiagnosticSeverity::Error,
			code,
			passIndex,
			passKind,
			std::move(message)
		});
	}

	inline FrameGraphCompileValidationResult ValidateRDGPassContract(const RDGPassContract& pass)
	{
		FrameGraphCompileValidationResult result;
		for (const auto& usedResourceName : pass.usedResourceNames)
		{
			if (std::find(
				pass.declaredResourceNames.begin(),
				pass.declaredResourceNames.end(),
				usedResourceName) != pass.declaredResourceNames.end())
			{
				continue;
			}

			AddFrameGraphCompileDiagnostic(
				result,
				FrameGraphCompileDiagnosticCode::UndeclaredRDGResourceUse,
				0u,
				pass.queueType == RHI::QueueType::Compute
					? Context::RenderPassCommandKind::Compute
					: Context::RenderPassCommandKind::Opaque,
				"RDG pass '" + pass.name + "' uses undeclared resource '" + usedResourceName + "'.");
		}
		return result;
	}

	inline bool ResourceAccessModeWrites(const Context::ResourceAccessMode mode)
	{
		return mode == Context::ResourceAccessMode::Write;
	}

	inline bool TextureAccessRangesOverlap(
		const RHI::RHISubresourceRange& left,
		const RHI::RHISubresourceRange& right)
	{
		return RHI::DoesSubresourceRangeOverlap(left, right);
	}

	inline std::vector<RHI::RHISubresourceRange> SubtractTextureAccessRange(
		std::vector<RHI::RHISubresourceRange> ranges,
		const RHI::RHISubresourceRange& consumedRange)
	{
		std::vector<RHI::RHISubresourceRange> remaining;
		for (const auto& range : ranges)
		{
			const auto remainders = RHI::SubtractSubresourceRange(range, consumedRange);
			for (const auto& remainder : remainders)
			{
				if (!RHI::IsEmptySubresourceRange(remainder))
					remaining.push_back(remainder);
			}
		}
		return remaining;
	}

	inline std::optional<RHI::RHISubresourceRange> NormalizeTextureAccessRange(
		const std::shared_ptr<RHI::RHITexture>& texture,
		const RHI::RHISubresourceRange& range)
	{
		if (texture == nullptr)
			return std::nullopt;

		return RHI::NormalizeTextureSubresourceRange(texture->GetDesc(), range);
	}

	inline bool BufferVectorContains(
		const std::vector<std::shared_ptr<RHI::RHIBuffer>>& buffers,
		const std::shared_ptr<RHI::RHIBuffer>& buffer)
	{
		return std::find(buffers.begin(), buffers.end(), buffer) != buffers.end();
	}

	inline bool IsPreparedComputeLifecycleBufferAccess(
		const Context::RenderPassCommandInput& passInput,
		const Context::BufferResourceAccess& access)
	{
		if (access.buffer == nullptr)
			return false;

		for (const auto& dispatchInput : passInput.computeDispatchInputs)
		{
			if (access.mode == Context::ResourceAccessMode::Read &&
				access.state == RHI::ResourceState::ShaderRead &&
				BufferVectorContains(dispatchInput.shaderReadBuffersBefore, access.buffer))
			{
				return true;
			}

			if (access.mode == Context::ResourceAccessMode::Write &&
				access.state == RHI::ResourceState::ShaderWrite &&
				(BufferVectorContains(dispatchInput.shaderWriteBuffersBefore, access.buffer) ||
					BufferVectorContains(dispatchInput.uavBarrierBuffersAfter, access.buffer)))
			{
				return true;
			}

			if (access.mode == Context::ResourceAccessMode::Write &&
				access.state == RHI::ResourceState::ShaderRead &&
				BufferVectorContains(dispatchInput.shaderReadBuffersAfter, access.buffer))
			{
				return true;
			}
		}

		return false;
	}

	template<typename TMetadataRange>
	inline FrameGraphCompileValidationResult ValidateThreadedRenderSceneExecutionInputs(
		const std::vector<Context::RenderPassCommandInput>& passInputs,
		const TMetadataRange& metadataRange)
	{
		FrameGraphCompileValidationResult result;
		const auto metadataCount = static_cast<size_t>(std::distance(std::begin(metadataRange), std::end(metadataRange)));
		const bool usePositionalMapping = metadataCount == passInputs.size();

		bool hasPreviousGraphicsPass = false;
		bool hasPreviousComputePass = false;
		size_t metadataIndex = 0u;
		for (const auto& metadataEntry : metadataRange)
		{
			const auto& metadata = ResolveThreadedRenderScenePassMetadata(metadataEntry);
			if (!ThreadedPassMetadataHasGraphPassName(metadata))
			{
				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::EmptyGraphPassName,
					metadataIndex,
					metadata.commandKind,
					"Threaded FrameGraph pass metadata has an empty graph pass name.");
			}

			if (IsRecordedThreadedPassOutputContractInvalid(metadata))
			{
				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::RecordedPassPropagatesOutput,
					metadataIndex,
					metadata.commandKind,
					"Recorded threaded FrameGraph passes must not propagate scene color or depth outputs.");
			}

			if (IsComputeThreadedPassMetadata(metadata))
			{
				if (metadata.queueType != RHI::QueueType::Compute)
				{
					AddFrameGraphCompileDiagnostic(
						result,
						FrameGraphCompileDiagnosticCode::ComputePassUsesNonComputeQueue,
						metadataIndex,
						metadata.commandKind,
						"Compute threaded FrameGraph passes must use the compute queue.");
				}

				if (metadata.propagatesColorOutput || metadata.propagatesDepthOutput)
				{
					AddFrameGraphCompileDiagnostic(
						result,
						FrameGraphCompileDiagnosticCode::ComputePassPropagatesOutput,
						metadataIndex,
						metadata.commandKind,
						"Compute threaded FrameGraph passes must not propagate scene color or depth outputs.");
				}
			}

			if (metadata.queueDependencyPolicy == Context::QueueDependencyPolicy::LastGraphics && !hasPreviousGraphicsPass)
			{
				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::MissingQueueDependencySource,
					metadataIndex,
					metadata.commandKind,
					"Threaded FrameGraph pass depends on the last graphics queue pass, but no previous graphics pass exists.");
			}
			else if (metadata.queueDependencyPolicy == Context::QueueDependencyPolicy::LastCompute && !hasPreviousComputePass)
			{
				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::MissingQueueDependencySource,
					metadataIndex,
					metadata.commandKind,
					"Threaded FrameGraph pass depends on the last compute queue pass, but no previous compute pass exists.");
			}

			if (metadata.queueType == RHI::QueueType::Graphics)
				hasPreviousGraphicsPass = true;
			else if (metadata.queueType == RHI::QueueType::Compute)
				hasPreviousComputePass = true;
			++metadataIndex;
		}

		for (size_t passIndex = 0u; passIndex < passInputs.size(); ++passIndex)
		{
			const auto& passInput = passInputs[passIndex];
			if (!HasThreadedPassMetadataForInput(metadataRange, passInput, passIndex, usePositionalMapping))
			{
				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::MissingPassMetadata,
					passIndex,
					passInput.kind,
					"Threaded FrameGraph pass input has no matching pass metadata.");
			}

			for (const auto& access : passInput.bufferResourceAccesses)
			{
				if (access.buffer != nullptr)
					continue;

				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::NullBufferResourceAccess,
					passIndex,
					passInput.kind,
					"Threaded FrameGraph pass declares a buffer resource access with a null buffer.");
			}

			for (size_t leftIndex = 0u; leftIndex < passInput.bufferResourceAccesses.size(); ++leftIndex)
			{
				const auto& left = passInput.bufferResourceAccesses[leftIndex];
				if (left.buffer == nullptr)
					continue;

				for (size_t rightIndex = leftIndex + 1u; rightIndex < passInput.bufferResourceAccesses.size(); ++rightIndex)
				{
					const auto& right = passInput.bufferResourceAccesses[rightIndex];
					if (right.buffer != left.buffer)
						continue;

					if (IsPreparedComputeLifecycleBufferAccess(passInput, left) &&
						IsPreparedComputeLifecycleBufferAccess(passInput, right))
					{
						continue;
					}

					if (!ResourceAccessModeWrites(left.mode) && !ResourceAccessModeWrites(right.mode))
						continue;

					AddFrameGraphCompileDiagnostic(
						result,
						FrameGraphCompileDiagnosticCode::ConflictingBufferResourceAccess,
						passIndex,
						passInput.kind,
						"Threaded FrameGraph pass declares conflicting buffer read/write access for the same buffer.");
				}
			}

			for (const auto& access : passInput.textureResourceAccesses)
			{
				if (access.texture != nullptr)
					continue;

				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::NullTextureResourceAccess,
					passIndex,
					passInput.kind,
					"Threaded FrameGraph pass declares a texture resource access with a null texture.");
			}

			for (size_t leftIndex = 0u; leftIndex < passInput.textureResourceAccesses.size(); ++leftIndex)
			{
				const auto& left = passInput.textureResourceAccesses[leftIndex];
				if (left.texture == nullptr)
					continue;

				for (size_t rightIndex = leftIndex + 1u; rightIndex < passInput.textureResourceAccesses.size(); ++rightIndex)
				{
					const auto& right = passInput.textureResourceAccesses[rightIndex];
					if (right.texture != left.texture)
						continue;

					if (!TextureAccessRangesOverlap(left.subresourceRange, right.subresourceRange))
						continue;

					if (!ResourceAccessModeWrites(left.mode) && !ResourceAccessModeWrites(right.mode))
						continue;

					AddFrameGraphCompileDiagnostic(
						result,
						FrameGraphCompileDiagnosticCode::ConflictingTextureResourceAccess,
						passIndex,
						passInput.kind,
						"Threaded FrameGraph pass declares conflicting texture read/write access for the same subresource range.");
				}
			}

			for (const auto& transition : passInput.bufferVisibilityTransitions)
			{
				if (transition.buffer != nullptr)
					continue;

				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::NullBufferVisibilityTransition,
					passIndex,
					passInput.kind,
					"Threaded FrameGraph pass declares a buffer visibility transition with a null buffer.");
			}

			for (const auto& transition : passInput.textureVisibilityTransitions)
			{
				if (transition.texture != nullptr)
					continue;

				AddFrameGraphCompileDiagnostic(
					result,
					FrameGraphCompileDiagnosticCode::NullTextureVisibilityTransition,
					passIndex,
					passInput.kind,
					"Threaded FrameGraph pass declares a texture visibility transition with a null texture.");
			}
		}

		return result;
	}

	inline std::string BuildFrameGraphCompileValidationErrorMessage(
		const FrameGraphCompileValidationResult& validation)
	{
		std::ostringstream stream;
		stream << "Invalid threaded FrameGraph compile inputs";
		for (const auto& diagnostic : validation.diagnostics)
		{
			if (diagnostic.severity != FrameGraphCompileDiagnosticSeverity::Error)
				continue;

			stream << "; pass[" << diagnostic.passIndex << "] "
				<< ToFrameGraphCompileDiagnosticCodeName(diagnostic.code)
				<< ": " << diagnostic.message;
		}
		return stream.str();
	}

	inline FrameGraphCompileValidationResult ValidateThreadedRenderSceneExecutionPlan(
		const ThreadedRenderSceneExecutionPlan& plan)
	{
		FrameGraphCompileValidationResult result;

		for (size_t dependencyIndex = 0u; dependencyIndex < plan.dependencies.size(); ++dependencyIndex)
		{
			const auto& dependency = plan.dependencies[dependencyIndex];
			const bool sourceOutOfRange = dependency.sourcePassIndex >= plan.passes.size();
			const bool targetOutOfRange = dependency.targetPassIndex >= plan.passes.size();
			const bool selfDependency = dependency.sourcePassIndex == dependency.targetPassIndex;
			if (!sourceOutOfRange && !targetOutOfRange && !selfDependency)
				continue;

			const auto passKind = targetOutOfRange
				? Context::RenderPassCommandKind::Opaque
				: plan.passes[dependency.targetPassIndex].commandInput.kind;
			AddFrameGraphCompileDiagnostic(
				result,
				FrameGraphCompileDiagnosticCode::InvalidQueueDependency,
				dependencyIndex,
				passKind,
				"Threaded FrameGraph dependency edge has an invalid source, target, or self-dependency.");
		}

		return result;
	}

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

	inline bool ThreadedRenderScenePassMayWriteDepthStencil(
		const ThreadedRenderScenePassMetadata& metadata,
		const bool clearDepth,
		const bool clearStencil)
	{
		if (clearDepth || clearStencil)
			return true;

		switch (metadata.commandKind)
		{
		case Context::RenderPassCommandKind::Opaque:
		case Context::RenderPassCommandKind::Skybox:
		case Context::RenderPassCommandKind::GBuffer:
			return true;
		default:
			return false;
		}
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
			commandInput.writesDepthStencilAttachment =
				commandInput.writesDepthStencilAttachment ||
				ThreadedRenderScenePassMayWriteDepthStencil(
					compiledPass.metadata,
					commandInput.clearDepth,
					commandInput.clearStencil);
			return;
		}

		commandInput.renderWidth = compiledPass.outputExecution.renderWidth;
		commandInput.renderHeight = compiledPass.outputExecution.renderHeight;
		commandInput.clearColor = compiledPass.outputExecution.clearColor;
		commandInput.clearDepth = compiledPass.outputExecution.clearDepth;
		commandInput.clearStencil = compiledPass.outputExecution.clearStencil;
		commandInput.clearColorValue = compiledPass.outputExecution.clearValue;
		commandInput.writesDepthStencilAttachment =
			commandInput.writesDepthStencilAttachment ||
			ThreadedRenderScenePassMayWriteDepthStencil(
				compiledPass.metadata,
				commandInput.clearDepth,
				commandInput.clearStencil);
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
		// Pass-level source indices only cover single write accesses that cover the read.
		// Per-subresource edges below are the authoritative sync path for partial texture
		// writes consumed by broader reads. The current HZB shader path writes mip0 only.
		if (passIndex >= passes.size())
			return std::nullopt;

		std::optional<size_t> latestSourceIndex;
		const auto updateLatestSource = [&latestSourceIndex](const size_t sourceIndex)
		{
			if (!latestSourceIndex.has_value() || sourceIndex > *latestSourceIndex)
				latestSourceIndex = sourceIndex;
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
					[&targetAccess](const Context::TextureResourceAccess& sourceAccess)
					{
						if (sourceAccess.mode != Context::ResourceAccessMode::Write ||
							sourceAccess.texture != targetAccess.texture)
						{
							return false;
						}

						const auto normalizedSourceRange = NormalizeTextureAccessRange(
							sourceAccess.texture,
							sourceAccess.subresourceRange);
						const auto normalizedTargetRange = NormalizeTextureAccessRange(
							targetAccess.texture,
							targetAccess.subresourceRange);
						return normalizedSourceRange.has_value() &&
							normalizedTargetRange.has_value() &&
							RHI::DoesSubresourceRangeCover(*normalizedSourceRange, *normalizedTargetRange);
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
		}

		for (const auto& targetAccess : targetPass.commandInput.textureResourceAccesses)
		{
			if (targetAccess.mode != Context::ResourceAccessMode::Read || targetAccess.texture == nullptr)
				continue;

			const auto normalizedTargetRange = NormalizeTextureAccessRange(
				targetAccess.texture,
				targetAccess.subresourceRange);
			if (!normalizedTargetRange.has_value())
				continue;

			std::vector<RHI::RHISubresourceRange> remainingTargetRanges{ *normalizedTargetRange };

			for (size_t reverseSourceIndex = passIndex; reverseSourceIndex > 0u && !remainingTargetRanges.empty(); --reverseSourceIndex)
			{
				const size_t sourceIndex = reverseSourceIndex - 1u;
				const auto& sourcePass = passes[sourceIndex];
				const auto edgeKind = sourcePass.queueType != targetPass.queueType
					? ThreadedRenderSceneDependencyKind::QueueSynchronization
					: ThreadedRenderSceneDependencyKind::ResourceVisibility;

				for (auto sourceIt = sourcePass.commandInput.textureResourceAccesses.rbegin();
					sourceIt != sourcePass.commandInput.textureResourceAccesses.rend() && !remainingTargetRanges.empty();
					++sourceIt)
				{
					if (sourceIt->mode != Context::ResourceAccessMode::Write ||
						sourceIt->texture != targetAccess.texture)
					{
						continue;
					}

					const auto normalizedSourceRange = NormalizeTextureAccessRange(
						sourceIt->texture,
						sourceIt->subresourceRange);
					if (!normalizedSourceRange.has_value())
						continue;

					std::vector<RHI::RHISubresourceRange> nextRemaining = remainingTargetRanges;
					for (const auto& remainingRange : remainingTargetRanges)
					{
						const auto intersection = RHI::IntersectSubresourceRanges(
							*normalizedSourceRange,
							remainingRange);
						if (!intersection.has_value())
							continue;

						auto sourceAccess = *sourceIt;
						sourceAccess.subresourceRange = *intersection;
						auto slicedTargetAccess = targetAccess;
						slicedTargetAccess.subresourceRange = *intersection;

						ThreadedRenderSceneDependencyEdge edge;
						edge.sourcePassIndex = sourceIndex;
						edge.targetPassIndex = passIndex;
						edge.kind = edgeKind;
						edge.resourceKind = ThreadedRenderSceneDependencyResourceKind::Texture;
						edge.sourceTextureAccess = sourceAccess;
						edge.targetTextureAccess = slicedTargetAccess;
						edges.push_back(std::move(edge));

						nextRemaining = SubtractTextureAccessRange(
							std::move(nextRemaining),
							*intersection);
						if (nextRemaining.empty())
							break;
					}
					remainingTargetRanges = std::move(nextRemaining);
				}
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

	inline void SeedScenePassOutputResourceState(
		ScenePassOutputResourceState& outputState,
		const ScenePassOutputChain& outputChain)
	{
		if (outputState.color < 0 && outputChain.color >= 0)
			outputState.color = outputChain.color;
		if (outputState.depth < 0 && outputChain.depth >= 0)
			outputState.depth = outputChain.depth;
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
	inline ThreadedRenderSceneExecutionPlanBuildResult TryBuildThreadedRenderSceneExecutionPlan(
		const std::vector<Context::RenderPassCommandInput>& passInputs,
		const TMetadataRange& metadataRange)
	{
		ThreadedRenderSceneExecutionPlanBuildResult result;
		const auto validation = ValidateThreadedRenderSceneExecutionInputs(passInputs, metadataRange);
		if (validation.HasErrors())
		{
			result.validation = validation;
			return result;
		}

		result.plan.passes.reserve(passInputs.size());
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
					if (ThreadedPassMetadataMatchesInput(metadata, passInput))
						break;
				}
			}

			if (metadataIt == std::end(metadataRange))
			{
				AddFrameGraphCompileDiagnostic(
					result.validation,
					FrameGraphCompileDiagnosticCode::MissingPassMetadata,
					passIndex,
					passInput.kind,
					"Missing ThreadedRenderScenePassMetadata for pass input kind.");
				return result;
			}

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
				plannedCommandInput.writesDepthStencilAttachment =
					plannedCommandInput.writesDepthStencilAttachment ||
					ThreadedRenderScenePassMayWriteDepthStencil(
						metadata,
						plannedCommandInput.clearDepth,
						plannedCommandInput.clearStencil);
			}

			result.plan.passes.push_back({
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

		result.plan.dependencies = BuildThreadedRenderSceneDependencyEdges(result.plan.passes);
		const auto planValidation = ValidateThreadedRenderSceneExecutionPlan(result.plan);
		if (planValidation.HasErrors())
		{
			result.validation = planValidation;
			return result;
		}

		result.succeeded = true;
		return result;
	}

	template<typename TMetadataRange>
	inline ThreadedRenderSceneExecutionPlan BuildThreadedRenderSceneExecutionPlan(
		const std::vector<Context::RenderPassCommandInput>& passInputs,
		const TMetadataRange& metadataRange)
	{
		auto result = TryBuildThreadedRenderSceneExecutionPlan(passInputs, metadataRange);
		if (!result.succeeded)
			throw std::invalid_argument(BuildFrameGraphCompileValidationErrorMessage(result.validation));
		return std::move(result.plan);
	}

	template<typename TMetadataRange>
	inline ThreadedRenderSceneExecutionCompileResult TryCompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const std::vector<Context::RenderPassCommandInput>& passInputs,
		const TMetadataRange& metadataRange)
	{
		ThreadedRenderSceneExecutionCompileResult result;
		result.execution.graphPasses = CompileThreadedRenderSceneGraphPasses(
			frame,
			importedColor,
			importedDepth,
			metadataRange);
		auto planResult = TryBuildThreadedRenderSceneExecutionPlan(
			passInputs,
			result.execution.graphPasses);
		if (!planResult.succeeded)
		{
			result.validation = std::move(planResult.validation);
			return result;
		}

		result.execution.threadedPlan = std::move(planResult.plan);
		result.succeeded = true;
		return result;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileThreadedRenderSceneExecution(
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const std::vector<Context::RenderPassCommandInput>& passInputs,
		const TMetadataRange& metadataRange)
	{
		auto result = TryCompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			passInputs,
			metadataRange);
		if (!result.succeeded)
			throw std::invalid_argument(BuildFrameGraphCompileValidationErrorMessage(result.validation));
		return std::move(result.execution);
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
		package.parallelDrawCommandBatches.clear();

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

		std::vector<uint64_t> firstWorkUnitIndexByPass;
		std::vector<uint64_t> lastWorkUnitIndexByPass;
		firstWorkUnitIndexByPass.reserve(plan.passes.size());
		lastWorkUnitIndexByPass.reserve(plan.passes.size());

		for (size_t passIndex = 0; passIndex < plan.passes.size(); ++passIndex)
		{
			const auto& plannedPass = plan.passes[passIndex];
			auto passCommandInput = plannedPass.commandInput;
			passCommandInput.dependencySourceWorkUnitIndex.reset();
			passCommandInput.requiresDependencyVisibility = plannedPass.requiresDependencyVisibility;

			package.passCommandInputs.push_back(passCommandInput);
			auto passWorkUnits = Context::BuildRecordedDrawCommandWorkUnitsForPass(
				passCommandInput,
				static_cast<uint64_t>(passIndex),
				static_cast<uint64_t>(package.parallelCommandWorkUnits.size()));
			for (auto& passWorkUnit : passWorkUnits)
			{
				if (passWorkUnit.debugName.empty())
					passWorkUnit.debugName = std::string(plannedPass.graphPassName);
				package.parallelCommandWorkUnits.push_back(std::move(passWorkUnit));
			}
			firstWorkUnitIndexByPass.push_back(static_cast<uint64_t>(package.parallelCommandWorkUnits.size() - passWorkUnits.size()));
			lastWorkUnitIndexByPass.push_back(static_cast<uint64_t>(package.parallelCommandWorkUnits.size() - 1u));
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
			if (dependency.sourcePassIndex >= lastWorkUnitIndexByPass.size() ||
				dependency.targetPassIndex >= firstWorkUnitIndexByPass.size())
			{
				continue;
			}

			Context::WorkUnitDependencyEdge workUnitDependency;
			workUnitDependency.sourceWorkUnitIndex = lastWorkUnitIndexByPass[dependency.sourcePassIndex];
			workUnitDependency.targetWorkUnitIndex = firstWorkUnitIndexByPass[dependency.targetPassIndex];
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
		package.parallelDrawCommandBatches = Context::BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
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
	inline ThreadedRenderSceneExecutionCompileResult TryCompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange)
	{
		auto result = TryCompileThreadedRenderSceneExecution(
			frame,
			importedColor,
			importedDepth,
			package.passCommandInputs,
			metadataRange);
		if (!result.succeeded)
			return result;

		ApplyThreadedRenderSceneExecutionPlan(package, result.execution.threadedPlan);
		return result;
	}

	template<typename TMetadataRange>
	inline CompiledThreadedRenderSceneExecution CompileAndApplyThreadedRenderSceneExecution(
		Context::RenderScenePackage& package,
		const Data::FrameDescriptor& frame,
		int32_t importedColor,
		int32_t importedDepth,
		const TMetadataRange& metadataRange)
	{
		auto result = TryCompileAndApplyThreadedRenderSceneExecution(
			package,
			frame,
			importedColor,
			importedDepth,
			metadataRange);
		if (!result.succeeded)
			throw std::invalid_argument(BuildFrameGraphCompileValidationErrorMessage(result.validation));
		return std::move(result.execution);
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
