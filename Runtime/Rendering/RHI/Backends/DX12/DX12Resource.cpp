#include "Rendering/RHI/Backends/DX12/DX12Resource.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <Debug/Logger.h>
#include "Rendering/RHI/Backends/DX12/DX12Command.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"

#if defined(_WIN32)
#include <Windows.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	namespace
	{
		UINT64 AlignUp(UINT64 value, UINT64 alignment)
		{
			if (alignment == 0)
				return value;

			return (value + (alignment - 1u)) & ~(alignment - 1u);
		}

		std::wstring Utf8ToWideString(const std::string& value)
		{
			if (value.empty())
				return {};

			const int requiredChars = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
			if (requiredChars <= 1)
				return {};

			std::wstring wide(static_cast<size_t>(requiredChars), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), requiredChars);
			wide.resize(static_cast<size_t>(requiredChars - 1));
			return wide;
		}

		void SetDx12ObjectName(ID3D12Object* object, const std::string& debugName)
		{
			if (object == nullptr || debugName.empty())
				return;

			const std::wstring wideName = Utf8ToWideString(debugName);
			if (wideName.empty())
				return;

			object->SetName(wideName.c_str());
		}

		NLS::Render::RHI::ResourceState ResolveUploadedTextureState(const NLS::Render::RHI::RHITextureDesc& desc)
		{
			using NLS::Render::RHI::ResourceState;
			using NLS::Render::RHI::TextureUsageFlags;

			if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Sampled))
				return ResourceState::ShaderRead;
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Storage))
				return ResourceState::ShaderWrite;
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::ColorAttachment))
				return ResourceState::RenderTarget;
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::DepthStencilAttachment))
				return ResourceState::DepthWrite;
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::CopySrc))
				return ResourceState::CopySrc;
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::CopyDst))
				return ResourceState::CopyDst;
			if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Present))
				return ResourceState::Present;
			return ResourceState::Unknown;
		}

		bool UploadInitialTextureData(
			ID3D12Device* device,
			ID3D12CommandQueue* graphicsQueue,
			ID3D12Resource* textureResource,
			const NLS::Render::RHI::RHITextureDesc& desc,
			const void* initialData,
			const std::string& debugName,
			NLS::Render::RHI::ResourceState& outFinalState)
		{
			if (device == nullptr || graphicsQueue == nullptr || textureResource == nullptr || initialData == nullptr)
				return false;

			const auto uploadPlan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);
			if (uploadPlan.subresources.empty() || uploadPlan.totalBytes == 0)
			{
				NLS_LOG_ERROR("UploadInitialTextureData: no upload plan for texture \"" + debugName + "\"");
				return false;
			}

			const UINT subresourceCount = static_cast<UINT>(uploadPlan.subresources.size());
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
			std::vector<UINT> rowCounts(subresourceCount);
			std::vector<UINT64> rowSizes(subresourceCount);
			UINT64 uploadBufferSize = 0;

			const D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
			device->GetCopyableFootprints(
				&textureDesc,
				0,
				subresourceCount,
				0,
				layouts.data(),
				rowCounts.data(),
				rowSizes.data(),
				&uploadBufferSize);

			if (uploadBufferSize == 0)
			{
				NLS_LOG_ERROR("UploadInitialTextureData: GetCopyableFootprints returned zero upload size for texture \"" + debugName + "\"");
				return false;
			}

			D3D12_HEAP_PROPERTIES uploadHeapProperties{};
			uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
			uploadHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			uploadHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			uploadHeapProperties.CreationNodeMask = 1;
			uploadHeapProperties.VisibleNodeMask = 1;

			D3D12_RESOURCE_DESC uploadBufferDesc{};
			uploadBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			uploadBufferDesc.Width = uploadBufferSize;
			uploadBufferDesc.Height = 1;
			uploadBufferDesc.DepthOrArraySize = 1;
			uploadBufferDesc.MipLevels = 1;
			uploadBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
			uploadBufferDesc.SampleDesc.Count = 1;
			uploadBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			uploadBufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
			HRESULT hr = device->CreateCommittedResource(
				&uploadHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&uploadBufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&uploadBuffer));
			if (FAILED(hr))
			{
				NLS_LOG_ERROR("UploadInitialTextureData: failed to create upload buffer for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			SetDx12ObjectName(uploadBuffer.Get(), debugName + "UploadBuffer");

			auto* uploadBase = static_cast<uint8_t*>(nullptr);
			D3D12_RANGE readRange{};
			hr = uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&uploadBase));
			if (FAILED(hr) || uploadBase == nullptr)
			{
				NLS_LOG_ERROR("UploadInitialTextureData: failed to map upload buffer for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			const auto* sourceBytes = static_cast<const uint8_t*>(initialData);
			for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
			{
				const auto& subresource = uploadPlan.subresources[subresourceIndex];
				const auto& layout = layouts[subresourceIndex];
				const size_t dstRowPitch = static_cast<size_t>(layout.Footprint.RowPitch);
				const size_t rowByteCount = static_cast<size_t>(rowSizes[subresourceIndex]);
				const size_t dstSlicePitch = dstRowPitch * static_cast<size_t>(rowCounts[subresourceIndex]);
				const auto* srcSubresource = sourceBytes + subresource.dataOffset;
				auto* dstSubresource = uploadBase + static_cast<size_t>(layout.Offset);

				for (uint32_t depthSlice = 0; depthSlice < subresource.depth; ++depthSlice)
				{
					const auto* srcSlice = srcSubresource + static_cast<size_t>(depthSlice) * subresource.slicePitch;
					auto* dstSlice = dstSubresource + static_cast<size_t>(depthSlice) * dstSlicePitch;
					for (UINT row = 0; row < rowCounts[subresourceIndex]; ++row)
					{
						std::memcpy(
							dstSlice + static_cast<size_t>(row) * dstRowPitch,
							srcSlice + static_cast<size_t>(row) * subresource.rowPitch,
							rowByteCount);
					}
				}
			}

			uploadBuffer->Unmap(0, nullptr);

			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
			hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
			if (FAILED(hr))
			{
				NLS_LOG_ERROR("UploadInitialTextureData: failed to create command allocator for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			SetDx12ObjectName(commandAllocator.Get(), debugName + "UploadAllocator");

			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
			if (FAILED(hr))
			{
				NLS_LOG_ERROR("UploadInitialTextureData: failed to create command list for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			SetDx12ObjectName(commandList.Get(), debugName + "UploadCommandList");

			for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
			{
				D3D12_TEXTURE_COPY_LOCATION srcLocation{};
				srcLocation.pResource = uploadBuffer.Get();
				srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				srcLocation.PlacedFootprint = layouts[subresourceIndex];

				D3D12_TEXTURE_COPY_LOCATION dstLocation{};
				dstLocation.pResource = textureResource;
				dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dstLocation.SubresourceIndex = subresourceIndex;

				commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
			}

			outFinalState = ResolveUploadedTextureState(desc);
			const D3D12_RESOURCE_STATES finalDxState =
				outFinalState == NLS::Render::RHI::ResourceState::Unknown
				? D3D12_RESOURCE_STATE_COMMON
				: NativeDX12CommandBuffer::ToD3D12ResourceState(outFinalState);

			if (finalDxState != D3D12_RESOURCE_STATE_COPY_DEST)
			{
				D3D12_RESOURCE_BARRIER barrier{};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition.pResource = textureResource;
				barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				barrier.Transition.StateAfter = finalDxState;
				barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				commandList->ResourceBarrier(1, &barrier);
			}

			hr = commandList->Close();
			if (FAILED(hr))
			{
				NLS_LOG_ERROR("UploadInitialTextureData: failed to close command list for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			ID3D12CommandList* commandLists[] = { commandList.Get() };
			graphicsQueue->ExecuteCommandLists(1, commandLists);

			Microsoft::WRL::ComPtr<ID3D12Fence> fence;
			hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
			if (FAILED(hr))
			{
				NLS_LOG_ERROR("UploadInitialTextureData: failed to create fence for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			SetDx12ObjectName(fence.Get(), debugName + "UploadFence");

			HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (fenceEvent == nullptr)
			{
				NLS_LOG_ERROR("UploadInitialTextureData: failed to create fence event for texture \"" + debugName + "\"");
				return false;
			}

			const UINT64 fenceValue = 1;
			hr = fence->SetEventOnCompletion(fenceValue, fenceEvent);
			if (FAILED(hr))
			{
				CloseHandle(fenceEvent);
				NLS_LOG_ERROR("UploadInitialTextureData: failed to set fence completion event for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			hr = graphicsQueue->Signal(fence.Get(), fenceValue);
			if (FAILED(hr))
			{
				CloseHandle(fenceEvent);
				NLS_LOG_ERROR("UploadInitialTextureData: failed to signal fence for texture \"" + debugName + "\" hr=" + std::to_string(hr));
				return false;
			}

			WaitForSingleObject(fenceEvent, INFINITE);
			CloseHandle(fenceEvent);
			return true;
		}
	}
#endif

	NativeDX12Buffer::NativeDX12Buffer(
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		const NLS::Render::RHI::RHIBufferDesc& desc,
		const void* initialData)
		: m_device(device)
		, m_graphicsQueue(graphicsQueue)
		, m_desc(desc)
	{
#if defined(_WIN32)
		if (device == nullptr)
			return;
		if (desc.size == 0u)
		{
			NLS_LOG_ERROR("NativeDX12Buffer: refused zero-sized buffer \"" + desc.debugName + "\"");
			return;
		}

		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapProps.CreationNodeMask = 1;
		heapProps.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resourceDesc.Height = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.MipLevels = 1;
		resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::BufferUsageFlags::Uniform))
			heapType = D3D12_HEAP_TYPE_UPLOAD;
		else if (desc.memoryUsage == NLS::Render::RHI::MemoryUsage::CPUToGPU)
			heapType = D3D12_HEAP_TYPE_UPLOAD;
		else if (desc.memoryUsage == NLS::Render::RHI::MemoryUsage::GPUToCPU)
			heapType = D3D12_HEAP_TYPE_READBACK;

		const bool isUniformBuffer =
			(static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::BufferUsageFlags::Uniform)) != 0u;
		const UINT64 resourceSize = isUniformBuffer
			? AlignUp(static_cast<UINT64>(desc.size), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
			: static_cast<UINT64>(desc.size);
		resourceDesc.Width = resourceSize;
		const bool isStorageBuffer =
			(static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::BufferUsageFlags::Storage)) != 0u;
		if (isStorageBuffer && heapType == D3D12_HEAP_TYPE_DEFAULT)
			resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		const bool needsDefaultHeapUpload = initialData != nullptr && heapType == D3D12_HEAP_TYPE_DEFAULT;

		heapProps.Type = heapType;
		const D3D12_RESOURCE_STATES initialState =
			heapType == D3D12_HEAP_TYPE_UPLOAD
				? D3D12_RESOURCE_STATE_GENERIC_READ
				: heapType == D3D12_HEAP_TYPE_READBACK
					? D3D12_RESOURCE_STATE_COPY_DEST
					: D3D12_RESOURCE_STATE_COMMON;

		const HRESULT createResourceResult = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			initialState,
			nullptr,
			IID_PPV_ARGS(&m_resource));
		if (FAILED(createResourceResult) || m_resource == nullptr)
			return;

		if (initialData != nullptr && heapType == D3D12_HEAP_TYPE_UPLOAD)
		{
			void* mappedData = nullptr;
			D3D12_RANGE readRange{};
			readRange.Begin = 0;
			readRange.End = 0;
			m_resource->Map(0, &readRange, &mappedData);
			if (mappedData != nullptr)
			{
				std::memcpy(mappedData, initialData, desc.size);
				D3D12_RANGE writeRange{};
				writeRange.Begin = 0;
				writeRange.End = desc.size;
				m_resource->Unmap(0, &writeRange);
			}
		}
		else if (needsDefaultHeapUpload && m_graphicsQueue != nullptr)
		{
			D3D12_HEAP_PROPERTIES uploadHeapProps{};
			uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
			uploadHeapProps.CreationNodeMask = 1;
			uploadHeapProps.VisibleNodeMask = 1;

			D3D12_RESOURCE_DESC uploadDesc = resourceDesc;
			uploadDesc.Width = static_cast<UINT64>(desc.size);
			uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
			Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
			const HRESULT uploadCreateResult = device->CreateCommittedResource(
				&uploadHeapProps,
				D3D12_HEAP_FLAG_NONE,
				&uploadDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
			if (FAILED(uploadCreateResult))
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload buffer hr=" + std::to_string(uploadCreateResult));
				return;
			}

			void* mappedData = nullptr;
			D3D12_RANGE readRange{};
			readRange.Begin = 0;
			readRange.End = 0;
			const HRESULT mapResult = uploadBuffer->Map(0, &readRange, &mappedData);
			if (FAILED(mapResult) || mappedData == nullptr)
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to map upload buffer hr=" + std::to_string(mapResult));
				return;
			}
			std::memcpy(mappedData, initialData, desc.size);
			D3D12_RANGE writeRange{};
			writeRange.Begin = 0;
			writeRange.End = desc.size;
			uploadBuffer->Unmap(0, &writeRange);

			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
			const HRESULT allocatorResult = device->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(commandAllocator.GetAddressOf()));
			if (FAILED(allocatorResult))
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload command allocator hr=" + std::to_string(allocatorResult));
				return;
			}

			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			const HRESULT commandListResult = device->CreateCommandList(
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				commandAllocator.Get(),
				nullptr,
				IID_PPV_ARGS(commandList.GetAddressOf()));
			if (FAILED(commandListResult))
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload command list hr=" + std::to_string(commandListResult));
				return;
			}

			D3D12_RESOURCE_BARRIER toCopyDestBarrier{};
			toCopyDestBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toCopyDestBarrier.Transition.pResource = m_resource.Get();
			toCopyDestBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			toCopyDestBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			toCopyDestBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &toCopyDestBarrier);

			commandList->CopyBufferRegion(m_resource.Get(), 0, uploadBuffer.Get(), 0, desc.size);

			D3D12_RESOURCE_BARRIER toCommonBarrier{};
			toCommonBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toCommonBarrier.Transition.pResource = m_resource.Get();
			toCommonBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			toCommonBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			toCommonBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &toCommonBarrier);

			const HRESULT closeResult = commandList->Close();
			if (FAILED(closeResult))
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to close upload command list hr=" + std::to_string(closeResult));
				return;
			}

			ID3D12CommandList* commandLists[] = { commandList.Get() };
			m_graphicsQueue->ExecuteCommandLists(1, commandLists);

			Microsoft::WRL::ComPtr<ID3D12Fence> fence;
			const HRESULT fenceResult = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
			if (FAILED(fenceResult))
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload fence hr=" + std::to_string(fenceResult));
				return;
			}

			HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (fenceEvent == nullptr)
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload fence event");
				return;
			}

			const HRESULT signalResult = m_graphicsQueue->Signal(fence.Get(), 1);
			if (FAILED(signalResult))
			{
				NLS_LOG_ERROR("NativeDX12Buffer: failed to signal upload fence hr=" + std::to_string(signalResult));
				CloseHandle(fenceEvent);
				return;
			}

			fence->SetEventOnCompletion(1, fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
			CloseHandle(fenceEvent);
		}

		if (heapType == D3D12_HEAP_TYPE_UPLOAD)
			m_state = NLS::Render::RHI::ResourceState::ShaderRead;
		else if (heapType == D3D12_HEAP_TYPE_READBACK)
			m_state = NLS::Render::RHI::ResourceState::CopyDst;
		else
			m_state = NLS::Render::RHI::ResourceState::Unknown;
#endif
	}

	NativeDX12Buffer::~NativeDX12Buffer()
	{
#if defined(_WIN32)
		if (m_resource != nullptr)
			m_resource.Reset();
#endif
	}

	std::string_view NativeDX12Buffer::GetDebugName() const { return m_desc.debugName; }
	const NLS::Render::RHI::RHIBufferDesc& NativeDX12Buffer::GetDesc() const { return m_desc; }
	NLS::Render::RHI::ResourceState NativeDX12Buffer::GetState() const { return m_state; }
	void NativeDX12Buffer::SetState(NLS::Render::RHI::ResourceState state) { m_state = state; }

	uint64_t NativeDX12Buffer::GetGPUAddress() const
	{
#if defined(_WIN32)
		if (m_resource == nullptr)
			return 0;
		return m_resource->GetGPUVirtualAddress();
#else
		return 0;
#endif
	}

	NLS::Render::RHI::NativeHandle NativeDX12Buffer::GetNativeBufferHandle()
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, static_cast<void*>(m_resource.Get()) };
#else
		return {};
#endif
	}

	NativeDX12Texture::NativeDX12Texture(ID3D12Device* device, const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData)
		: m_device(device)
		, m_desc(desc)
	{
#if defined(_WIN32)
		if (device == nullptr)
			return;
		if (desc.extent.width == 0u || desc.extent.height == 0u || desc.extent.depth == 0u)
		{
			NLS_LOG_ERROR(
				"NativeDX12Texture: refused zero-sized texture \"" + desc.debugName +
				"\" size=" + std::to_string(desc.extent.width) +
				"x" + std::to_string(desc.extent.height) +
				"x" + std::to_string(desc.extent.depth));
			return;
		}

		const bool isDepthTexture =
			NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment);
		const bool isColorAttachment =
			NLS::Render::RHI::HasTextureUsage(desc.usage, NLS::Render::RHI::TextureUsageFlags::ColorAttachment);
		const auto layerCount = static_cast<UINT16>(
			desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube
				? NLS::Render::RHI::GetTextureLayerCount(desc.dimension)
				: (std::max)(desc.arrayLayers, 1u));

		D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		if (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube)
			dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = dimension;
		resourceDesc.Width = desc.extent.width;
		resourceDesc.Height = desc.extent.height;
		resourceDesc.DepthOrArraySize = (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube) ? 6 : layerCount;
		resourceDesc.MipLevels = desc.mipLevels;
		resourceDesc.Format = ToDxgiFormat(desc.format);
		resourceDesc.SampleDesc.Count = desc.sampleCount;
		resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		if (isDepthTexture)
			resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		if (isColorAttachment)
			resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Storage))
			resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		const D3D12_HEAP_PROPERTIES heapProperties{
			D3D12_HEAP_TYPE_DEFAULT,
			D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
			D3D12_MEMORY_POOL_UNKNOWN,
			1,
			1
		};

		const D3D12_RESOURCE_STATES initialState =
			(initialData != nullptr && !isDepthTexture)
			? D3D12_RESOURCE_STATE_COPY_DEST
			: D3D12_RESOURCE_STATE_COMMON;

		D3D12_CLEAR_VALUE clearValue{};
		clearValue.Format = ToDxgiFormat(desc.format);
		if (isDepthTexture)
		{
			clearValue.DepthStencil = {
				desc.optimizedClearValue.depth,
				static_cast<UINT8>(desc.optimizedClearValue.stencil)
			};
		}
		else if (isColorAttachment)
		{
			clearValue.Color[0] = desc.optimizedClearValue.color[0];
			clearValue.Color[1] = desc.optimizedClearValue.color[1];
			clearValue.Color[2] = desc.optimizedClearValue.color[2];
			clearValue.Color[3] = desc.optimizedClearValue.color[3];
		}

		const bool hasClearValue = desc.optimizedClearValue.enabled && (isDepthTexture || isColorAttachment);
		device->CreateCommittedResource(
			&heapProperties,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			initialState,
			hasClearValue ? &clearValue : nullptr,
			IID_PPV_ARGS(&m_resource));

		if (m_resource != nullptr)
		{
			SetDx12ObjectName(m_resource.Get(), desc.debugName);
			m_state = initialState == D3D12_RESOURCE_STATE_COPY_DEST
				? NLS::Render::RHI::ResourceState::CopyDst
				: NLS::Render::RHI::ResourceState::Unknown;
		}
#endif
	}

	NativeDX12Texture::~NativeDX12Texture()
	{
#if defined(_WIN32)
		if (m_resource != nullptr)
			m_resource.Reset();
#endif
	}

	std::string_view NativeDX12Texture::GetDebugName() const { return m_desc.debugName; }
	const NLS::Render::RHI::RHITextureDesc& NativeDX12Texture::GetDesc() const { return m_desc; }
	NLS::Render::RHI::ResourceState NativeDX12Texture::GetState() const { return m_state; }
	void NativeDX12Texture::SetState(NLS::Render::RHI::ResourceState state) { m_state = state; }

	NLS::Render::RHI::NativeHandle NativeDX12Texture::GetNativeImageHandle()
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, static_cast<void*>(m_resource.Get()) };
#else
		return {};
#endif
	}

	void* NativeDX12Texture::GetNativeTextureHandle() const
	{
#if defined(_WIN32)
		return m_resource.Get();
#else
		return nullptr;
#endif
	}

#if defined(_WIN32)
	ID3D12Resource* NativeDX12Texture::GetResource() const
	{
		return m_resource.Get();
	}

	DXGI_FORMAT NativeDX12Texture::ToDxgiFormat(NLS::Render::RHI::TextureFormat format)
	{
		switch (format)
		{
		case NLS::Render::RHI::TextureFormat::RGB8:
		case NLS::Render::RHI::TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case NLS::Render::RHI::TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
		default: return DXGI_FORMAT_R8G8B8A8_UNORM;
		}
	}
#endif

	NativeDX12TextureView::NativeDX12TextureView(
		ID3D12Device* device,
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		const NLS::Render::RHI::RHITextureViewDesc& desc)
		: m_device(device)
		, m_texture(texture)
		, m_desc(desc)
	{
#if defined(_WIN32)
		if (device == nullptr || texture == nullptr)
			return;

		auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(texture.get());
		if (nativeTexture == nullptr)
			return;

		ID3D12Resource* resource = nativeTexture->GetResource();
		if (resource == nullptr)
			return;

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.NumDescriptors = 1;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap))))
			return;
		m_srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

		const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(texture->GetDesc(), desc);
		if (descriptors.hasSrv)
			device->CreateShaderResourceView(resource, &descriptors.srvDesc, m_srvHandle);

		if (descriptors.hasRtv)
		{
			D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
			rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rtvHeapDesc.NumDescriptors = 1;
			rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			if (SUCCEEDED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap))))
			{
				m_rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
				device->CreateRenderTargetView(resource, &descriptors.rtvDesc, m_rtvHandle);
			}
		}

		if (descriptors.hasDsv)
		{
			D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
			dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
			dsvHeapDesc.NumDescriptors = 1;
			dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			if (SUCCEEDED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap))))
			{
				m_dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
				device->CreateDepthStencilView(resource, &descriptors.dsvDesc, m_dsvHandle);
			}
		}
#endif
	}

	NativeDX12TextureView::~NativeDX12TextureView()
	{
#if defined(_WIN32)
		m_srvHeap.Reset();
		m_rtvHeap.Reset();
		m_dsvHeap.Reset();
#endif
	}

	std::string_view NativeDX12TextureView::GetDebugName() const { return m_desc.debugName; }
	const NLS::Render::RHI::RHITextureViewDesc& NativeDX12TextureView::GetDesc() const { return m_desc; }
	const std::shared_ptr<NLS::Render::RHI::RHITexture>& NativeDX12TextureView::GetTexture() const { return m_texture; }
	NLS::Render::RHI::NativeHandle NativeDX12TextureView::GetNativeRenderTargetView() { return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_rtvHandle.ptr) }; }
	NLS::Render::RHI::NativeHandle NativeDX12TextureView::GetNativeDepthStencilView() { return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_dsvHandle.ptr) }; }

	NLS::Render::RHI::NativeHandle NativeDX12TextureView::GetNativeShaderResourceView()
	{
#if defined(_WIN32)
		if (m_srvHeap == nullptr)
			return {};
		return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr) };
#else
		return {};
#endif
	}

#if defined(_WIN32)
	D3D12_GPU_DESCRIPTOR_HANDLE NativeDX12TextureView::GetGPUDescriptorHandle() const
	{
		return m_srvHeap != nullptr ? m_srvHeap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{};
	}

	ID3D12Resource* NativeDX12TextureView::GetResource() const
	{
		auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(m_texture.get());
		return nativeTexture ? nativeTexture->GetResource() : nullptr;
	}
#endif

	NativeDX12Sampler::NativeDX12Sampler(ID3D12Device*, const NLS::Render::RHI::SamplerDesc& desc, const std::string& debugName)
		: m_desc(desc)
		, m_debugName(debugName)
	{
	}

	std::string_view NativeDX12Sampler::GetDebugName() const { return m_debugName; }
	const NLS::Render::RHI::SamplerDesc& NativeDX12Sampler::GetDesc() const { return m_desc; }
	NLS::Render::RHI::NativeHandle NativeDX12Sampler::GetNativeSamplerHandle() { return { NLS::Render::RHI::BackendType::DX12, nullptr }; }

	std::shared_ptr<NLS::Render::RHI::RHITexture> CreateNativeDX12Texture(
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		const NLS::Render::RHI::RHITextureDesc& desc,
		const void* initialData)
	{
#if defined(_WIN32)
		if (device == nullptr)
			return nullptr;

		auto texture = std::make_shared<NativeDX12Texture>(device, desc, initialData);
		if (texture == nullptr)
			return nullptr;

		const bool needsInitialUpload =
			initialData != nullptr &&
			desc.format != NLS::Render::RHI::TextureFormat::Depth24Stencil8;

		if (needsInitialUpload)
		{
			auto* resource = texture->GetResource();
			if (resource == nullptr)
			{
				NLS_LOG_ERROR("CreateNativeDX12Texture: texture resource creation failed for \"" + desc.debugName + "\"");
				return nullptr;
			}

			NLS::Render::RHI::ResourceState finalState = NLS::Render::RHI::ResourceState::Unknown;
			const std::string textureName = desc.debugName.empty() ? "TextureResource" : desc.debugName;
			if (!UploadInitialTextureData(device, graphicsQueue, resource, desc, initialData, textureName, finalState))
			{
				NLS_LOG_ERROR("CreateNativeDX12Texture: initial upload failed for \"" + textureName + "\"");
				return nullptr;
			}

			texture->SetState(finalState);
		}

		return texture;
#else
		(void)device;
		(void)graphicsQueue;
		(void)desc;
		(void)initialData;
		return nullptr;
#endif
	}
}
