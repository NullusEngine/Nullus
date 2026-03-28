#include "Rendering/FrameGraph/FrameGraphBuffer.h"

#include <Debug/Assertion.h>

#include "Rendering/Context/Driver.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"

namespace NLS::Render::FrameGraph
{
	namespace
	{
		struct ExplicitBufferState
		{
			NLS::Render::RHI::ResourceState state = NLS::Render::RHI::ResourceState::Unknown;
			NLS::Render::RHI::PipelineStageMask stageMask = NLS::Render::RHI::PipelineStageMask::AllCommands;
			NLS::Render::RHI::AccessMask accessMask = NLS::Render::RHI::AccessMask::MemoryRead;
		};

		NLS::Render::RHI::BufferUsageFlags ToExplicitBufferUsage(const FrameGraphBuffer::Desc& desc)
		{
			NLS::Render::RHI::BufferUsageFlags flags = NLS::Render::RHI::BufferUsageFlags::None;
			switch (desc.type)
			{
			case NLS::Render::RHI::BufferType::Vertex: flags = flags | NLS::Render::RHI::BufferUsageFlags::Vertex; break;
			case NLS::Render::RHI::BufferType::Index: flags = flags | NLS::Render::RHI::BufferUsageFlags::Index; break;
			case NLS::Render::RHI::BufferType::Uniform: flags = flags | NLS::Render::RHI::BufferUsageFlags::Uniform; break;
			case NLS::Render::RHI::BufferType::ShaderStorage:
			default:
				flags = flags | NLS::Render::RHI::BufferUsageFlags::Storage;
				break;
			}

			if (desc.usage != NLS::Render::RHI::BufferUsage::StaticDraw)
				flags = flags | NLS::Render::RHI::BufferUsageFlags::CopyDst;

			return flags;
		}

		NLS::Render::RHI::MemoryUsage ToExplicitMemoryUsage(const NLS::Render::RHI::BufferUsage usage)
		{
			switch (usage)
			{
			case NLS::Render::RHI::BufferUsage::DynamicDraw:
			case NLS::Render::RHI::BufferUsage::StreamDraw:
				return NLS::Render::RHI::MemoryUsage::CPUToGPU;
			case NLS::Render::RHI::BufferUsage::StaticDraw:
			default:
				return NLS::Render::RHI::MemoryUsage::GPUOnly;
			}
		}

		NLS::Render::RHI::RHIBufferDesc ToExplicitBufferDesc(const FrameGraphBuffer::Desc& desc)
		{
			NLS::Render::RHI::RHIBufferDesc explicitDesc;
			explicitDesc.size = desc.size;
			explicitDesc.usage = ToExplicitBufferUsage(desc);
			explicitDesc.memoryUsage = ToExplicitMemoryUsage(desc.usage);
			explicitDesc.debugName = "FrameGraphBuffer";
			return explicitDesc;
		}

		ExplicitBufferState GetExplicitBufferReadState(const FrameGraphBuffer::Desc& desc)
		{
			switch (desc.type)
			{
			case NLS::Render::RHI::BufferType::Vertex:
				return {
					NLS::Render::RHI::ResourceState::VertexBuffer,
					NLS::Render::RHI::PipelineStageMask::VertexInput,
					NLS::Render::RHI::AccessMask::VertexRead
				};
			case NLS::Render::RHI::BufferType::Index:
				return {
					NLS::Render::RHI::ResourceState::IndexBuffer,
					NLS::Render::RHI::PipelineStageMask::VertexInput,
					NLS::Render::RHI::AccessMask::IndexRead
				};
			case NLS::Render::RHI::BufferType::Uniform:
				return {
					NLS::Render::RHI::ResourceState::UniformBuffer,
					NLS::Render::RHI::PipelineStageMask::AllGraphics | NLS::Render::RHI::PipelineStageMask::ComputeShader,
					NLS::Render::RHI::AccessMask::UniformRead
				};
			case NLS::Render::RHI::BufferType::ShaderStorage:
			default:
				return {
					NLS::Render::RHI::ResourceState::ShaderRead,
					NLS::Render::RHI::PipelineStageMask::AllGraphics | NLS::Render::RHI::PipelineStageMask::ComputeShader,
					NLS::Render::RHI::AccessMask::ShaderRead
				};
			}
		}

		ExplicitBufferState GetExplicitBufferWriteState(const FrameGraphBuffer::Desc& desc)
		{
			if (desc.type == NLS::Render::RHI::BufferType::ShaderStorage)
			{
				return {
					NLS::Render::RHI::ResourceState::ShaderWrite,
					NLS::Render::RHI::PipelineStageMask::AllGraphics | NLS::Render::RHI::PipelineStageMask::ComputeShader,
					NLS::Render::RHI::AccessMask::ShaderWrite
				};
			}

			return GetExplicitBufferReadState(desc);
		}
	}

	void FrameGraphBuffer::create(const Desc& desc, void* allocator)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(allocator);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphBuffer requires a valid frame graph execution context");
		auto& driver = executionContext->driver;
		auto* device = executionContext->device;

		if (id != 0)
		{
			return;
		}

		if (device != nullptr)
		{
			explicitBuffer = device->CreateBuffer(ToExplicitBufferDesc(desc));
			ownsResource = explicitBuffer != nullptr;
		}

		if (explicitBuffer != nullptr)
		{
			id = 0;
			return;
		}

		bufferResource = driver.CreateBufferResource(desc.type);
		id = bufferResource != nullptr ? bufferResource->GetResourceId() : driver.CreateBuffer();
		ownsResource = true;
		driver.BindBuffer(desc.type, id);
		driver.SetBufferData(desc.type, desc.size, nullptr, desc.usage);
		driver.BindBuffer(desc.type, 0);
	}

	void FrameGraphBuffer::destroy(const Desc&, void* allocator)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(allocator);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphBuffer requires a valid frame graph execution context");
		auto& driver = executionContext->driver;
		const bool hadExplicitResource = explicitBuffer != nullptr;

		if (id == 0 && explicitBuffer == nullptr && bufferResource == nullptr)
			return;

		explicitBuffer.reset();
		if (hadExplicitResource)
		{
			id = 0;
			ownsResource = false;
			return;
		}
		if (ownsResource)
		{
			if (bufferResource != nullptr)
				bufferResource.reset();
			else
				driver.DestroyBuffer(id);
		}
		id = 0;
		ownsResource = false;
	}

	void FrameGraphBuffer::preRead(const Desc& desc, uint32_t flags, void* context)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphBuffer requires a valid frame graph execution context");
		auto& driver = executionContext->driver;

		if (explicitBuffer != nullptr && executionContext->CanTrackExplicitResourceState())
		{
			const auto targetState = GetExplicitBufferReadState(desc);
			NLS::Render::RHI::RHIBarrierDesc barrierDesc;
			barrierDesc.bufferBarriers.push_back({
				explicitBuffer,
				NLS::Render::RHI::ResourceState::Unknown,
				targetState.state,
				NLS::Render::RHI::PipelineStageMask::AllCommands,
				targetState.stageMask,
				NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
				targetState.accessMask
			});
			executionContext->RecordResourceBarriers(barrierDesc);
			return;
		}

		if (id == 0)
			return;

		driver.BindBufferBase(desc.type, flags == 0xFFFFFFFFu ? 0u : flags, id);
	}

	void FrameGraphBuffer::preWrite(const Desc& desc, uint32_t flags, void* context)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphBuffer requires a valid frame graph execution context");

		if (explicitBuffer != nullptr && executionContext->CanTrackExplicitResourceState())
		{
			const auto targetState = GetExplicitBufferWriteState(desc);
			NLS::Render::RHI::RHIBarrierDesc barrierDesc;
			barrierDesc.bufferBarriers.push_back({
				explicitBuffer,
				NLS::Render::RHI::ResourceState::Unknown,
				targetState.state,
				NLS::Render::RHI::PipelineStageMask::AllCommands,
				targetState.stageMask,
				NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
				targetState.accessMask
			});
			executionContext->RecordResourceBarriers(barrierDesc);
			return;
		}

		preRead(desc, flags, context);
	}

	std::string FrameGraphBuffer::toString(const Desc& desc)
	{
		return std::to_string(desc.size) + " bytes";
	}
}
