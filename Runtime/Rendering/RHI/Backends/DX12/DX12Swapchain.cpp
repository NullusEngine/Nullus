#include "Rendering/RHI/Backends/DX12/DX12Swapchain.h"

#include "Rendering/RHI/Backends/DX12/DX12Synchronization.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

#include <cstdint>
#include <memory>
#include <string>

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
		m_desc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
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

	void* NativeDX12Swapchain::GetNativeSwapchainHandle()
	{
#if defined(_WIN32)
		return m_swapchain.Get();
#else
		return nullptr;
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

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heapDesc.NumDescriptors = m_imageCount;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap))))
			return;

		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		const UINT rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		for (uint32_t i = 0; i < m_imageCount; ++i)
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
			if (SUCCEEDED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffer))))
			{
				D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
				rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = 0;

				m_device->CreateRenderTargetView(backbuffer.Get(), &rtvDesc, rtvHandle);

				auto view = std::make_shared<NativeDX12BackbufferView>(
					backbuffer,
					rtvHandle,
					DXGI_FORMAT_R8G8B8A8_UNORM,
					m_desc.width,
					m_desc.height);
				m_backbufferViews.push_back(view);

				rtvHandle.ptr += rtvDescriptorSize;
			}
		}
#endif
	}
}
