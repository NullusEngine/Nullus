#include "Rendering/RHI/Backends/DX12/DX12Swapchain.h"

#include "Rendering/RHI/Backends/DX12/DX12Synchronization.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Debug/Logger.h>

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

namespace NLS::Render::Backend
{
	namespace
	{
		bool ShouldLogDx12FrameFlow()
		{
			return NLS::Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow;
		}

#if defined(_WIN32)
		UINT64 GetDx12StoredMessageCount(ID3D12Device* device)
		{
			if (device == nullptr)
				return 0;

			Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
				return 0;

			return infoQueue->GetNumStoredMessages();
		}

		void LogDx12DebugMessagesSince(
			ID3D12Device* device,
			const UINT64 firstMessage,
			const std::string& context)
		{
			if (device == nullptr)
				return;

			Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
				return;

			const UINT64 messageCount = infoQueue->GetNumStoredMessages();
			if (messageCount <= firstMessage)
				return;

			for (UINT64 messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
			{
				SIZE_T messageSize = 0;
				if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageSize)) || messageSize == 0)
					continue;

				std::vector<char> messageBytes(messageSize);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageBytes.data());
				if (FAILED(infoQueue->GetMessage(messageIndex, message, &messageSize)))
					continue;

				NLS_LOG_ERROR(
					context +
					": D3D12 message id=" + std::to_string(message->ID) +
					" severity=" + std::to_string(static_cast<int>(message->Severity)) +
					" text=" + std::string(message->pDescription));
			}
		}
#endif
	}

#if defined(_WIN32)
	NativeDX12BackbufferTexture::NativeDX12BackbufferTexture(
		Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer,
		DXGI_FORMAT format,
		uint32_t width,
		uint32_t height)
		: m_backbuffer(backbuffer)
	{
		m_desc.extent.width = width;
		m_desc.extent.height = height;
		m_desc.extent.depth = 1;
		m_desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
		m_desc.format = format == DXGI_FORMAT_R16G16B16A16_FLOAT
			? NLS::Render::RHI::TextureFormat::RGBA16F
			: NLS::Render::RHI::TextureFormat::RGBA8;
		m_desc.usage =
			NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
			NLS::Render::RHI::TextureUsageFlags::Present;
		m_desc.debugName = "BackbufferTexture";
	}
#endif

	std::string_view NativeDX12BackbufferTexture::GetDebugName() const
	{
		return m_desc.debugName;
	}

	const NLS::Render::RHI::RHITextureDesc& NativeDX12BackbufferTexture::GetDesc() const
	{
		return m_desc;
	}

	NLS::Render::RHI::ResourceState NativeDX12BackbufferTexture::GetState() const
	{
		return NLS::Render::RHI::ResourceState::Present;
	}

	bool NativeDX12BackbufferTexture::RequiresExternalClearValueMessageFilter() const
	{
		return true;
	}

	NLS::Render::RHI::NativeHandle NativeDX12BackbufferTexture::GetNativeImageHandle()
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, static_cast<void*>(m_backbuffer.Get()) };
#else
		return { NLS::Render::RHI::BackendType::DX12, nullptr };
#endif
	}

#if defined(_WIN32)
	NativeDX12BackbufferView::NativeDX12BackbufferView(
		Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer,
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
		DXGI_FORMAT format,
		uint32_t width,
		uint32_t height)
		: m_backbuffer(backbuffer)
		, m_rtvHandle(rtvHandle)
		, m_format(format)
	{
		m_texture = std::make_shared<NativeDX12BackbufferTexture>(backbuffer, format, width, height);
	}
#endif

	std::string_view NativeDX12BackbufferView::GetDebugName() const
	{
		return "BackbufferView";
	}

	const NLS::Render::RHI::RHITextureViewDesc& NativeDX12BackbufferView::GetDesc() const
	{
		return m_desc;
	}

	const std::shared_ptr<NLS::Render::RHI::RHITexture>& NativeDX12BackbufferView::GetTexture() const
	{
		return m_texture;
	}

	NLS::Render::RHI::NativeHandle NativeDX12BackbufferView::GetNativeRenderTargetView()
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_rtvHandle.ptr) };
#else
		return { NLS::Render::RHI::BackendType::DX12, nullptr };
#endif
	}

	NLS::Render::RHI::NativeHandle NativeDX12BackbufferView::GetNativeDepthStencilView()
	{
		return { NLS::Render::RHI::BackendType::DX12, nullptr };
	}

	NLS::Render::RHI::NativeHandle NativeDX12BackbufferView::GetNativeShaderResourceView()
	{
		return { NLS::Render::RHI::BackendType::DX12, nullptr };
	}

#if defined(_WIN32)
	NativeDX12Swapchain::NativeDX12Swapchain(
		Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain,
		ID3D12Device* device,
		const NLS::Render::RHI::SwapchainDesc& desc)
		: m_swapchain(swapchain)
		, m_device(device)
		, m_desc(desc)
		, m_imageCount(desc.imageCount > 0 ? desc.imageCount : 2)
	{
		NLS_LOG_INFO(
			"NativeDX12Swapchain created: swapchain=" +
			std::to_string(reinterpret_cast<uintptr_t>(m_swapchain.Get())) +
			" device=" +
			std::to_string(reinterpret_cast<uintptr_t>(device)) +
			" imageCount=" +
			std::to_string(m_imageCount));
		RecreateBackbufferViews();
		NLS_LOG_INFO("NativeDX12Swapchain created: backbufferViews.size=" + std::to_string(m_backbufferViews.size()));
	}

	std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateNativeDX12Swapchain(
		IDXGIFactory6* factory,
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		const NLS::Render::RHI::SwapchainDesc& desc)
	{
		if (factory == nullptr || device == nullptr || graphicsQueue == nullptr || desc.nativeWindowHandle == nullptr)
		{
			NLS_LOG_ERROR(
				"CreateNativeDX12Swapchain failed: factory=" +
				std::to_string(reinterpret_cast<uintptr_t>(factory)) +
				" device=" +
				std::to_string(reinterpret_cast<uintptr_t>(device)) +
				" queue=" +
				std::to_string(reinterpret_cast<uintptr_t>(graphicsQueue)) +
				" window=" +
				std::to_string(reinterpret_cast<uintptr_t>(desc.nativeWindowHandle)));
			return nullptr;
		}

		Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
		swapChainDesc.Width = desc.width;
		swapChainDesc.Height = desc.height;
		swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.BufferCount = desc.imageCount > 0 ? desc.imageCount : 2;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapChainDesc.Scaling = DXGI_SCALING_NONE;

		HRESULT hr = factory->CreateSwapChainForHwnd(
			graphicsQueue,
			static_cast<HWND>(desc.nativeWindowHandle),
			&swapChainDesc,
			nullptr,
			nullptr,
			&swapChain1);

		if (FAILED(hr) && swapChainDesc.Scaling == DXGI_SCALING_NONE)
		{
			swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
			hr = factory->CreateSwapChainForHwnd(
				graphicsQueue,
				static_cast<HWND>(desc.nativeWindowHandle),
				&swapChainDesc,
				nullptr,
				nullptr,
				&swapChain1);
		}

		if (FAILED(hr))
		{
			NLS_LOG_ERROR("CreateNativeDX12Swapchain: CreateSwapChainForHwnd failed with hr=" + std::to_string(hr));
			return nullptr;
		}

		Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
		swapChain1.As(&swapChain3);
		return std::make_shared<NativeDX12Swapchain>(swapChain3, device, desc);
	}
#endif

	std::string_view NativeDX12Swapchain::GetDebugName() const
	{
		return "NativeDX12Swapchain";
	}

	const NLS::Render::RHI::SwapchainDesc& NativeDX12Swapchain::GetDesc() const
	{
		return m_desc;
	}

	uint32_t NativeDX12Swapchain::GetImageCount() const
	{
		return m_imageCount;
	}

	std::optional<NLS::Render::RHI::RHIAcquiredImage> NativeDX12Swapchain::AcquireNextImage(
		const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
		const std::shared_ptr<NLS::Render::RHI::RHIFence>& fence)
	{
		(void)fence;
#if defined(_WIN32)
		if (m_swapchain == nullptr)
			return std::nullopt;

		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO("NativeDX12Swapchain::AcquireNextImage: called");
		}

		const UINT currentIndex = m_swapchain->GetCurrentBackBufferIndex();
		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO(
				"NativeDX12Swapchain::AcquireNextImage: currentIndex=" +
				std::to_string(currentIndex) +
				" backbufferViews.size=" +
				std::to_string(m_backbufferViews.size()));
		}

		NLS::Render::RHI::RHIAcquiredImage image;
		image.imageIndex = currentIndex;
		image.imageView = GetBackbufferView(currentIndex);

		auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(signalSemaphore.get());
		if (nativeSemaphore != nullptr && nativeSemaphore->GetFence() != nullptr)
		{
			if (!nativeSemaphore->SignalOnCpu())
			{
				NLS_LOG_ERROR("NativeDX12Swapchain::AcquireNextImage: failed to signal acquire semaphore");
			}
		}

		return image;
#else
		(void)signalSemaphore;
		return std::nullopt;
#endif
	}

	std::shared_ptr<NLS::Render::RHI::RHITextureView> NativeDX12Swapchain::GetBackbufferView(uint32_t index)
	{
#if defined(_WIN32)
		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO(
				"NativeDX12Swapchain::GetBackbufferView: index=" +
				std::to_string(index) +
				" backbufferViews.size=" +
				std::to_string(m_backbufferViews.size()) +
				" m_device=" +
				std::to_string(reinterpret_cast<uintptr_t>(m_device)) +
				" m_swapchain=" +
				std::to_string(reinterpret_cast<uintptr_t>(m_swapchain.Get())));
		}
		if (m_backbufferViews.empty() && m_device != nullptr && m_swapchain != nullptr)
		{
			if (ShouldLogDx12FrameFlow())
			{
				NLS_LOG_INFO("NativeDX12Swapchain::GetBackbufferView: calling RecreateBackbufferViews");
			}
			RecreateBackbufferViews();
			if (ShouldLogDx12FrameFlow())
			{
				NLS_LOG_INFO("NativeDX12Swapchain::GetBackbufferView: after RecreateBackbufferViews, size=" + std::to_string(m_backbufferViews.size()));
			}
		}
#endif

		if (index >= m_backbufferViews.size())
		{
			NLS_LOG_ERROR(
				"NativeDX12Swapchain::GetBackbufferView: index " +
				std::to_string(index) +
				" >= size " +
				std::to_string(m_backbufferViews.size()));
			return nullptr;
		}
		return m_backbufferViews[index];
	}

	bool NativeDX12Swapchain::Resize(uint32_t width, uint32_t height)
	{
		if (width == 0u || height == 0u)
			return false;

#if defined(_WIN32)
		if (m_swapchain != nullptr)
		{
			ReleaseBackbufferViews();
			const HRESULT hr = m_swapchain->ResizeBuffers(
				m_imageCount,
				width,
				height,
				DXGI_FORMAT_R8G8B8A8_UNORM,
				0);
			if (FAILED(hr))
			{
				NLS_LOG_ERROR(
					"NativeDX12Swapchain::Resize: ResizeBuffers failed with hr=" +
					std::to_string(hr) +
					" requested=" +
					std::to_string(width) + "x" + std::to_string(height));
				RecreateBackbufferViews();
				return false;
			}
		}
#endif
		m_desc.width = width;
		m_desc.height = height;
		RecreateBackbufferViews();
		return true;
	}

	NLS::Render::RHI::NativeHandle NativeDX12Swapchain::GetNativeSwapchainHandle()
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, m_swapchain.Get() };
#else
		return {};
#endif
	}

	void NativeDX12Swapchain::ReleaseBackbufferViews()
	{
		m_backbufferViews.clear();
#if defined(_WIN32)
		m_rtvHeap.Reset();
#endif
	}

	void NativeDX12Swapchain::RecreateBackbufferViews()
	{
		ReleaseBackbufferViews();
#if defined(_WIN32)
		if (m_device == nullptr || m_swapchain == nullptr)
			return;

		DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
		if (FAILED(m_swapchain->GetDesc1(&swapchainDesc)))
		{
			NLS_LOG_ERROR("NativeDX12Swapchain::RecreateBackbufferViews: failed to query swapchain desc.");
			return;
		}

		const DXGI_FORMAT backbufferFormat = swapchainDesc.Format == DXGI_FORMAT_UNKNOWN
			? DXGI_FORMAT_R8G8B8A8_UNORM
			: swapchainDesc.Format;
		const uint32_t backbufferWidth = swapchainDesc.Width != 0 ? swapchainDesc.Width : m_desc.width;
		const uint32_t backbufferHeight = swapchainDesc.Height != 0 ? swapchainDesc.Height : m_desc.height;
		const uint32_t backbufferCount = swapchainDesc.BufferCount != 0 ? swapchainDesc.BufferCount : m_imageCount;

		m_imageCount = backbufferCount;
		m_desc.width = backbufferWidth;
		m_desc.height = backbufferHeight;

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = backbufferCount;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap));
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("NativeDX12Swapchain::RecreateBackbufferViews: CreateDescriptorHeap failed with hr=" + std::to_string(hr));
			return;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		const UINT rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		for (uint32_t i = 0; i < backbufferCount; ++i)
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
			hr = m_swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffer));
			if (FAILED(hr) || backbuffer == nullptr)
			{
				NLS_LOG_ERROR(
					"NativeDX12Swapchain::RecreateBackbufferViews: GetBuffer failed for index=" +
					std::to_string(i) +
					" hr=" +
					std::to_string(hr));
				continue;
			}

			const UINT64 firstMessage = GetDx12StoredMessageCount(m_device);
			m_device->CreateRenderTargetView(backbuffer.Get(), nullptr, rtvHandle);
			LogDx12DebugMessagesSince(
				m_device,
				firstMessage,
				"NativeDX12Swapchain::RecreateBackbufferViews: CreateRenderTargetView index=" +
				std::to_string(i));

			auto view = std::make_shared<NativeDX12BackbufferView>(
				backbuffer,
				rtvHandle,
				backbufferFormat,
				backbufferWidth,
				backbufferHeight);
			m_backbufferViews.push_back(view);

			rtvHandle.ptr += rtvDescriptorSize;
		}
#endif
	}
}
