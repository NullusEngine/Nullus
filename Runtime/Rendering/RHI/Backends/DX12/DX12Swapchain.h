#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"

struct ID3D12DescriptorHeap;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;
struct IDXGIFactory6;
struct IDXGISwapChain3;

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
	class NativeDX12BackbufferTexture final : public NLS::Render::RHI::RHITexture
	{
	public:
#if defined(_WIN32)
		NativeDX12BackbufferTexture(
			Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer,
			DXGI_FORMAT format,
			uint32_t width,
			uint32_t height);
#endif

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHITextureDesc& GetDesc() const override;
		NLS::Render::RHI::ResourceState GetState() const override;
		bool RequiresExternalClearValueMessageFilter() const override;
		NLS::Render::RHI::NativeHandle GetNativeImageHandle() override;

	private:
		NLS::Render::RHI::RHITextureDesc m_desc{};
#if defined(_WIN32)
		Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffer;
#endif
	};

	class NativeDX12BackbufferView final : public NLS::Render::RHI::RHITextureView
	{
	public:
#if defined(_WIN32)
		NativeDX12BackbufferView(
			Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer,
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
			DXGI_FORMAT format,
			uint32_t width,
			uint32_t height);
#endif

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override;
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override;
		NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override;
		NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override;
		NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override;

	private:
		NLS::Render::RHI::RHITextureViewDesc m_desc{};
		std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
#if defined(_WIN32)
		Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffer;
		D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
		DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
#endif
	};

	class NativeDX12Swapchain final : public NLS::Render::RHI::RHISwapchain
	{
	public:
#if defined(_WIN32)
		NativeDX12Swapchain(
			Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain,
			ID3D12Device* device,
			const NLS::Render::RHI::SwapchainDesc& desc);
#endif

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::SwapchainDesc& GetDesc() const override;
		uint32_t GetImageCount() const override;
		std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
			const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
			const std::shared_ptr<NLS::Render::RHI::RHIFence>& fence) override;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override;
		bool Resize(uint32_t width, uint32_t height) override;
		NLS::Render::RHI::NativeHandle GetNativeSwapchainHandle() override;

	private:
		void ReleaseBackbufferViews();
		void RecreateBackbufferViews();

#if defined(_WIN32)
		Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
		ID3D12Device* m_device = nullptr;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
#else
		IDXGISwapChain3* m_swapchain = nullptr;
		ID3D12Device* m_device = nullptr;
#endif
		NLS::Render::RHI::SwapchainDesc m_desc{};
		uint32_t m_imageCount = 3;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_backbufferViews;
	};

#if defined(_WIN32)
	std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateNativeDX12Swapchain(
		IDXGIFactory6* factory,
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		const NLS::Render::RHI::SwapchainDesc& desc);
#endif
}
