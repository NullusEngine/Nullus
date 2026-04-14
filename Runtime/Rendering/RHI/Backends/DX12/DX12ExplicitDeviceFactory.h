#pragma once

#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGIFactory6;
struct IDXGIAdapter1;
struct ID3D12Resource;

namespace NLS::Render::RHI
{
	class RHIDevice;
}

namespace NLS::Render::Backend
{
	// Typed NativeHandle variants for DX12 backend
	// These provide type safety by preventing implicit conversions between different handle types
	struct DX12BufferHandle
	{
		ID3D12Resource* resource = nullptr;
	};

	struct DX12ImageHandle
	{
		ID3D12Resource* resource = nullptr;
	};

	struct DX12SamplerHandle
	{
		void* handle = nullptr;
	};

	struct DX12DescriptorHandle
	{
		uint64_t ptr = 0;  // GPU descriptor handle
	};

	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateNativeDX12ExplicitDevice(
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		IDXGIFactory6* factory,
		IDXGIAdapter1* adapter,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
		const std::string& vendor,
		const std::string& hardware);

	// Direct creation - creates DX12 Tier A device without IRenderDevice
	NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12RhiDevice(bool debugMode = false);
}
