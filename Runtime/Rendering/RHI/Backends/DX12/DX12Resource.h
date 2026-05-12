#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12Resource;

#if defined(_WIN32)
#include <d3d12.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::RHI
{
	class UploadBackend;
}

namespace NLS::Render::Backend
{
	class NativeDX12Buffer final : public NLS::Render::RHI::RHIBuffer
	{
	public:
		NativeDX12Buffer(
			ID3D12Device* device,
			ID3D12CommandQueue* graphicsQueue,
			const NLS::Render::RHI::RHIBufferDesc& desc,
			const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc);
		~NativeDX12Buffer() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override;
		NLS::Render::RHI::ResourceState GetState() const override;
		void SetState(NLS::Render::RHI::ResourceState state);
		uint64_t GetGPUAddress() const override;
		NLS::Render::RHI::RHIUpdateResult UpdateData(
			const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override;
		NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override;

	private:
		ID3D12Device* m_device = nullptr;
		ID3D12CommandQueue* m_graphicsQueue = nullptr;
		NLS::Render::RHI::RHIBufferDesc m_desc{};
		NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
#if defined(_WIN32)
		Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
#endif
	};

	class NativeDX12Texture final : public NLS::Render::RHI::RHITexture
	{
	public:
		NativeDX12Texture(
			ID3D12Device* device,
			const NLS::Render::RHI::RHITextureDesc& desc,
			const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc);
		~NativeDX12Texture() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHITextureDesc& GetDesc() const override;
		NLS::Render::RHI::ResourceState GetState() const override;
		void SetState(NLS::Render::RHI::ResourceState state);
		NLS::Render::RHI::NativeHandle GetNativeImageHandle() override;
#if defined(_WIN32)
		ID3D12Resource* GetResource() const;
		static DXGI_FORMAT ToDxgiFormat(NLS::Render::RHI::TextureFormat format);
#endif

	private:
		ID3D12Device* m_device = nullptr;
		NLS::Render::RHI::RHITextureDesc m_desc{};
		NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
#if defined(_WIN32)
		Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
#endif
	};

	class NativeDX12TextureView final : public NLS::Render::RHI::RHITextureView
	{
	public:
		NativeDX12TextureView(
			ID3D12Device* device,
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			const NLS::Render::RHI::RHITextureViewDesc& desc);
		~NativeDX12TextureView() override;

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override;
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override;
		NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override;
		NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override;
		NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override;
#if defined(_WIN32)
		D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle() const;
		ID3D12Resource* GetResource() const;
#endif

	private:
		ID3D12Device* m_device = nullptr;
		std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
		NLS::Render::RHI::RHITextureViewDesc m_desc{};
#if defined(_WIN32)
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
		D3D12_CPU_DESCRIPTOR_HANDLE m_srvHandle = {};
		D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
		D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle = {};
#endif
	};

	class NativeDX12Sampler final : public NLS::Render::RHI::RHISampler
	{
	public:
		NativeDX12Sampler(ID3D12Device* device, const NLS::Render::RHI::SamplerDesc& desc, const std::string& debugName);

		std::string_view GetDebugName() const override;
		const NLS::Render::RHI::SamplerDesc& GetDesc() const override;
		NLS::Render::RHI::NativeHandle GetNativeSamplerHandle() override;

	private:
		NLS::Render::RHI::SamplerDesc m_desc{};
		std::string m_debugName;
	};

	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHITexture> CreateNativeDX12Texture(
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		const NLS::Render::RHI::RHITextureDesc& desc,
		const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc);
	NLS_RENDER_API NLS::Render::RHI::RHIUpdateResult UpdateNativeDX12Texture(
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		const NLS::Render::RHI::RHITextureUpdateDesc& desc);
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::UploadBackend> CreateDX12UploadBackend(
		ID3D12Device* device);
}
