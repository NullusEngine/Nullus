#include "Rendering/RHI/Backends/DX12/DX12Device.h"

#include <cwchar>

#include <Debug/Logger.h>

namespace NLS::Render::Backend
{
	bool DX12DeviceResources::IsValid() const
	{
#if defined(_WIN32)
		return device != nullptr &&
			graphicsQueue != nullptr &&
			factory != nullptr &&
			adapter != nullptr &&
			capabilities.GetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady).supported;
#else
		return false;
#endif
	}

#if defined(_WIN32)
	namespace
	{
		bool EnableDx12Dred()
		{
			Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings1;
			const HRESULT dredSettings1Hr = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings1));
			if (SUCCEEDED(dredSettings1Hr) && dredSettings1 != nullptr)
			{
				dredSettings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				NLS_LOG_INFO("CreateDX12RhiDevice: DRED v1 auto breadcrumbs, page faults, and breadcrumb context forced on");
				return true;
			}

			Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
			const HRESULT dredSettingsHr = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings));
			if (FAILED(dredSettingsHr) || dredSettings == nullptr)
			{
				NLS_LOG_WARNING(
					"CreateDX12RhiDevice: DRED settings unavailable hr=" + std::to_string(dredSettingsHr) +
					", settings1 hr=" + std::to_string(dredSettings1Hr));
				return false;
			}

			dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			NLS_LOG_INFO("CreateDX12RhiDevice: DRED auto breadcrumbs and page faults forced on");
			return true;
		}

		UINT BuildDx12FactoryFlags(bool debugMode)
		{
			UINT factoryFlags = 0;
#if defined(_DEBUG)
			if (debugMode)
			{
				Microsoft::WRL::ComPtr<ID3D12Debug5> debugController5;
				const HRESULT debug5Hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController5));
				if (SUCCEEDED(debug5Hr) && debugController5 != nullptr)
				{
					debugController5->EnableDebugLayer();
					debugController5->SetEnableGPUBasedValidation(TRUE);
					debugController5->SetEnableAutoName(TRUE);
					NLS_LOG_INFO("CreateDX12RhiDevice: enabled DX12 debug layer with GPU-based validation and auto names via ID3D12Debug5");
					factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
				}
				else
				{
					Microsoft::WRL::ComPtr<ID3D12Debug3> debugController3;
					const HRESULT debug3Hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController3));
					if (SUCCEEDED(debug3Hr) && debugController3 != nullptr)
					{
						debugController3->EnableDebugLayer();
						debugController3->SetEnableGPUBasedValidation(TRUE);
						debugController3->SetEnableSynchronizedCommandQueueValidation(TRUE);
						NLS_LOG_INFO("CreateDX12RhiDevice: enabled DX12 debug layer with GPU-based validation via ID3D12Debug3");
						factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
					}
					else
					{
						Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
						const HRESULT debugHr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
						if (SUCCEEDED(debugHr) && debugController != nullptr)
						{
							debugController->EnableDebugLayer();
							NLS_LOG_INFO("CreateDX12RhiDevice: enabled DX12 debug layer via ID3D12Debug");
							factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
						}
						else
						{
							NLS_LOG_WARNING(
								"CreateDX12RhiDevice: failed to acquire DX12 debug controller hr=" + std::to_string(debugHr) +
								", debug3 hr=" + std::to_string(debug3Hr) +
								", debug5 hr=" + std::to_string(debug5Hr));
						}
					}
				}
			}
#endif
			return factoryFlags;
		}

		Microsoft::WRL::ComPtr<IDXGIAdapter1> FindHardwareAdapter(IDXGIFactory6* factory)
		{
			Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
			if (factory == nullptr)
				return adapter;

			for (UINT adapterIndex = 0; ; ++adapterIndex)
			{
				Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
				if (factory->EnumAdapters1(adapterIndex, &candidate) == DXGI_ERROR_NOT_FOUND)
					break;

				DXGI_ADAPTER_DESC1 adapterDesc{};
				candidate->GetDesc1(&adapterDesc);
				if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
					continue;

				Microsoft::WRL::ComPtr<ID3D12Device> testDevice;
				if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice))))
				{
					adapter = candidate;
					break;
				}
			}

			return adapter;
		}

		NLS::Render::RHI::RHIDeviceCapabilities BuildDX12Capabilities(ID3D12CommandQueue* computeQueue)
		{
			using NLS::Render::RHI::RHIDeviceFeature;

			NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
			capabilities.SetFeature(RHIDeviceFeature::BackendReady, true);
			capabilities.SetFeature(RHIDeviceFeature::Graphics, true);
			capabilities.SetFeature(RHIDeviceFeature::Compute, true);
			capabilities.SetFeature(
				RHIDeviceFeature::AsyncCompute,
				false,
				"DX12 async compute is gated until cross-queue scheduling and lifetime validation are complete");
			capabilities.SetFeature(
				RHIDeviceFeature::DedicatedComputeQueue,
				computeQueue != nullptr,
				computeQueue != nullptr
					? std::string{}
					: "DX12 dedicated compute queue creation failed or is unavailable");
			capabilities.SetFeature(
				RHIDeviceFeature::CopyQueue,
				false,
				"DX12 copy queue is not wired into the RHI submission path");
			capabilities.SetFeature(RHIDeviceFeature::Swapchain, true);
			capabilities.SetFeature(RHIDeviceFeature::FramebufferBlit, true);
			capabilities.SetFeature(RHIDeviceFeature::DepthBlit, true);
			capabilities.SetFeature(RHIDeviceFeature::CurrentSceneRenderer, true);
			capabilities.SetFeature(RHIDeviceFeature::OffscreenFramebuffers, true);
			capabilities.SetFeature(RHIDeviceFeature::FramebufferReadback, true);
			capabilities.SetFeature(RHIDeviceFeature::EditorPickingReadback, true);
			capabilities.SetFeature(RHIDeviceFeature::UITextureHandles, true);
			capabilities.SetFeature(RHIDeviceFeature::Cubemaps, true);
			capabilities.SetFeature(RHIDeviceFeature::MultiRenderTargets, true);
			capabilities.SetFeature(RHIDeviceFeature::ParallelCommandRecording, true);
			capabilities.SetFeature(RHIDeviceFeature::ParallelCommandTranslation, true);
			capabilities.SetFeature(
				RHIDeviceFeature::TransientResourceAllocator,
				false,
				"DX12 transient resource allocator is not implemented yet");
			capabilities.SetFeature(RHIDeviceFeature::CentralizedDescriptorManagement, true);
			capabilities.SetFeature(RHIDeviceFeature::PipelineStateCache, true);
			capabilities.maxTextureDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
			capabilities.maxColorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
			capabilities.limits.maxTextureDimension2D = capabilities.maxTextureDimension2D;
			capabilities.limits.maxColorAttachments = capabilities.maxColorAttachments;
			capabilities.SetFeature(RHIDeviceFeature::ExplicitBarriers, true);
			capabilities.SynchronizeLegacyFields();
			return capabilities;
		}
	}
#endif

	DX12DeviceResources CreateDX12DeviceResources(bool debugMode)
	{
		DX12DeviceResources resources;
#if defined(_WIN32)
		if (debugMode)
			resources.dredDiagnosticsEnabled = EnableDx12Dred();

		const UINT factoryFlags = BuildDx12FactoryFlags(debugMode);
		if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&resources.factory))))
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to create DXGI factory");
			return {};
		}

		resources.adapter = FindHardwareAdapter(resources.factory.Get());
		if (resources.adapter == nullptr)
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to find suitable DX12 adapter");
			return {};
		}

		DXGI_ADAPTER_DESC1 adapterDesc{};
		resources.adapter->GetDesc1(&adapterDesc);
		resources.hardware.assign(adapterDesc.Description, adapterDesc.Description + std::wcslen(adapterDesc.Description));

		if (FAILED(D3D12CreateDevice(resources.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&resources.device))))
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to create DX12 device");
			return {};
		}

		Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(resources.device->QueryInterface(IID_PPV_ARGS(&infoQueue))) && infoQueue != nullptr)
			NLS_LOG_INFO("CreateDX12RhiDevice: DX12 info queue available");
		else
			NLS_LOG_WARNING("CreateDX12RhiDevice: DX12 info queue unavailable");

		const D3D12_COMMAND_QUEUE_DESC queueDesc{
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			0,
			D3D12_COMMAND_QUEUE_FLAG_NONE,
			0
		};

		if (FAILED(resources.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&resources.graphicsQueue))))
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to create DX12 command queue");
			return {};
		}

		const D3D12_COMMAND_QUEUE_DESC computeQueueDesc{
			D3D12_COMMAND_LIST_TYPE_COMPUTE,
			0,
			D3D12_COMMAND_QUEUE_FLAG_NONE,
			0
		};
		const HRESULT computeQueueHr = resources.device->CreateCommandQueue(&computeQueueDesc, IID_PPV_ARGS(&resources.computeQueue));
		if (FAILED(computeQueueHr))
		{
			NLS_LOG_WARNING("CreateDX12RhiDevice: failed to create dedicated compute queue hr=" + std::to_string(computeQueueHr));
		}

		resources.capabilities = BuildDX12Capabilities(resources.computeQueue.Get());
		resources.vendor = "DX12";
#else
		(void)debugMode;
		NLS_LOG_WARNING("CreateDX12RhiDevice: DX12 only supported on Windows");
#endif
		return resources;
	}
}
