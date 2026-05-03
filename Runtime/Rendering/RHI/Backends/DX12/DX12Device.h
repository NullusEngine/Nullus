#pragma once

#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIDevice.h"

struct ID3D12CommandQueue;
struct ID3D12Device;
struct IDXGIAdapter1;
struct IDXGIFactory6;

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
	struct NLS_RENDER_API DX12DeviceResources
	{
#if defined(_WIN32)
		Microsoft::WRL::ComPtr<ID3D12Device> device;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> computeQueue;
		Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
		Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
#endif
		NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
		std::string vendor;
		std::string hardware;
		bool dredDiagnosticsEnabled = false;

		bool IsValid() const;
	};

	NLS_RENDER_API DX12DeviceResources CreateDX12DeviceResources(bool debugMode);
}
