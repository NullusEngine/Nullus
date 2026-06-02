#include "Rendering/RHI/Backends/DX12/DX12Device.h"

#include <cwchar>

#include <Debug/Logger.h>
#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"

namespace NLS::Render::Backend
{
	bool DX12DeviceResources::IsValid() const
	{
#if defined(_WIN32)
		return device != nullptr &&
			graphicsQueue != nullptr &&
			factory != nullptr &&
			adapter != nullptr &&
			shaderModel6Supported &&
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

		std::string ShaderModelToString(D3D_SHADER_MODEL shaderModel)
		{
			const auto encoded = static_cast<unsigned int>(shaderModel);
			return std::to_string(encoded >> 4u) + "." + std::to_string(encoded & 0xfu);
		}

		struct DX12ShaderModelSupport
		{
			HRESULT hr = E_FAIL;
			D3D_SHADER_MODEL highestShaderModel = static_cast<D3D_SHADER_MODEL>(0);
			bool supported = false;
		};

		DX12ShaderModelSupport QueryDX12ShaderModel6Support(ID3D12Device* device)
		{
			DX12ShaderModelSupport support{};
			if (device == nullptr)
				return support;

			const D3D_SHADER_MODEL candidateShaderModels[] = {
				static_cast<D3D_SHADER_MODEL>(0x68),
				static_cast<D3D_SHADER_MODEL>(0x67),
				static_cast<D3D_SHADER_MODEL>(0x66),
				static_cast<D3D_SHADER_MODEL>(0x65),
				static_cast<D3D_SHADER_MODEL>(0x64),
				static_cast<D3D_SHADER_MODEL>(0x63),
				static_cast<D3D_SHADER_MODEL>(0x62),
				static_cast<D3D_SHADER_MODEL>(0x61),
				static_cast<D3D_SHADER_MODEL>(0x60)
			};

			for (const auto candidateShaderModel : candidateShaderModels)
			{
				D3D12_FEATURE_DATA_SHADER_MODEL shaderModelSupport{};
				shaderModelSupport.HighestShaderModel = candidateShaderModel;
				const HRESULT hr = device->CheckFeatureSupport(
					D3D12_FEATURE_SHADER_MODEL,
					&shaderModelSupport,
					sizeof(shaderModelSupport));
				if (FAILED(hr))
				{
					support.hr = hr;
					continue;
				}

				support.hr = hr;
				support.highestShaderModel = shaderModelSupport.HighestShaderModel;
				support.supported = shaderModelSupport.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
				return support;
			}

			return support;
		}

		std::string BuildShaderModelFailureDiagnostic(const DX12ShaderModelSupport& support)
		{
			if (FAILED(support.hr))
			{
				return "Shader Model 6.0 is required, but CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL) failed hr=" +
					std::to_string(support.hr);
			}

			return "Shader Model 6.0 is required, but DX12 reported Shader Model " +
				ShaderModelToString(support.highestShaderModel);
		}

		std::string NarrowAdapterName(const DXGI_ADAPTER_DESC1& adapterDesc)
		{
			return std::string(adapterDesc.Description, adapterDesc.Description + std::wcslen(adapterDesc.Description));
		}

		Microsoft::WRL::ComPtr<IDXGIAdapter1> FindHardwareAdapter(
			IDXGIFactory6* factory,
			std::string& rejectionDiagnostics)
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
					const auto shaderModelSupport = QueryDX12ShaderModel6Support(testDevice.Get());
					if (!shaderModelSupport.supported)
					{
						rejectionDiagnostics =
							NarrowAdapterName(adapterDesc) + ": " +
							BuildShaderModelFailureDiagnostic(shaderModelSupport);
						continue;
					}

					adapter = candidate;
					break;
				}
			}

			return adapter;
		}

		void PopulateDX12TextureFormatCapabilities(
			ID3D12Device* device,
			NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
		{
			using NLS::Render::RHI::TextureFormat;

			if (device == nullptr)
				return;

			for (uint32_t formatValue = 0u; formatValue < static_cast<uint32_t>(TextureFormat::Count); ++formatValue)
			{
				const auto format = static_cast<TextureFormat>(formatValue);
				D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
				support.Format = NLS::Render::RHI::DX12::ToDXGIFormat(format);
				bool supportsSrgbView = false;

				std::string diagnosticReason;
				if (support.Format == DXGI_FORMAT_UNKNOWN)
				{
					diagnosticReason = "DX12 has no native format mapping for this texture format";
					capabilities.SetTextureFormatCapability(
						format,
						NLS::Render::RHI::DX12::BuildDX12TextureFormatCapability(
							format,
							D3D12_FORMAT_SUPPORT1_NONE,
							D3D12_FORMAT_SUPPORT2_NONE,
							false,
							std::move(diagnosticReason)));
					continue;
				}

				const HRESULT hr = device->CheckFeatureSupport(
					D3D12_FEATURE_FORMAT_SUPPORT,
					&support,
					sizeof(support));
				if (FAILED(hr))
				{
					diagnosticReason = "CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) failed hr=" + std::to_string(hr);
					capabilities.SetTextureFormatCapability(
						format,
						NLS::Render::RHI::DX12::BuildDX12TextureFormatCapability(
							format,
							D3D12_FORMAT_SUPPORT1_NONE,
							D3D12_FORMAT_SUPPORT2_NONE,
							false,
							std::move(diagnosticReason)));
					continue;
				}

				const auto* descriptor = NLS::Render::RHI::GetTextureFormatDescriptor(format);
				const DXGI_FORMAT srgbFormat = NLS::Render::RHI::DX12::ToDXGIFormat(
					format,
					NLS::Render::RHI::TextureColorSpace::SRGB);
				if (descriptor != nullptr &&
					descriptor->supportsSrgbView &&
					srgbFormat != DXGI_FORMAT_UNKNOWN)
				{
					D3D12_FEATURE_DATA_FORMAT_SUPPORT srgbSupport{};
					srgbSupport.Format = srgbFormat;
					const HRESULT srgbHr = device->CheckFeatureSupport(
						D3D12_FEATURE_FORMAT_SUPPORT,
						&srgbSupport,
						sizeof(srgbSupport));
					supportsSrgbView =
						SUCCEEDED(srgbHr) &&
						(srgbSupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0 &&
						(srgbSupport.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
				}

				capabilities.SetTextureFormatCapability(
					format,
					NLS::Render::RHI::DX12::BuildDX12TextureFormatCapability(
						format,
						support.Support1,
						support.Support2,
						supportsSrgbView));
			}
		}

		NLS::Render::RHI::RHIDeviceCapabilities BuildDX12Capabilities(
			ID3D12Device* device,
			ID3D12CommandQueue* computeQueue)
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
				RHIDeviceFeature::InRenderPassChildCommandBuffers,
				false,
				"DX12 in-render-pass child bundles are disabled by default after model-load device-hung quarantine; re-enable only with DRED/RenderDoc coverage");
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
			PopulateDX12TextureFormatCapabilities(device, capabilities);
			capabilities.SynchronizeLegacyFields();
			return capabilities;
		}
	}
#endif

	DX12DeviceResources CreateDX12DeviceResources(bool debugMode)
	{
		DX12DeviceResources resources;
#if defined(_WIN32)
		resources.dredDiagnosticsEnabled = EnableDx12Dred();

		const UINT factoryFlags = BuildDx12FactoryFlags(debugMode);
		if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&resources.factory))))
		{
			resources.creationDiagnostics = "failed to create DXGI factory";
			NLS_LOG_ERROR("CreateDX12RhiDevice: " + resources.creationDiagnostics);
			return resources;
		}

		std::string adapterRejectionDiagnostics;
		resources.adapter = FindHardwareAdapter(resources.factory.Get(), adapterRejectionDiagnostics);
		if (resources.adapter == nullptr)
		{
			resources.creationDiagnostics =
				"failed to find suitable DX12 adapter" +
				(adapterRejectionDiagnostics.empty()
					? std::string{}
					: "; last rejected adapter: " + adapterRejectionDiagnostics);
			NLS_LOG_ERROR("CreateDX12RhiDevice: " + resources.creationDiagnostics);
			return resources;
		}

		DXGI_ADAPTER_DESC1 adapterDesc{};
		resources.adapter->GetDesc1(&adapterDesc);
		resources.hardware = NarrowAdapterName(adapterDesc);

		if (FAILED(D3D12CreateDevice(resources.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&resources.device))))
		{
			resources.creationDiagnostics = "failed to create DX12 device";
			NLS_LOG_ERROR("CreateDX12RhiDevice: " + resources.creationDiagnostics);
			return resources;
		}

		const auto shaderModelSupport = QueryDX12ShaderModel6Support(resources.device.Get());
		if (FAILED(shaderModelSupport.hr))
		{
			resources.creationDiagnostics = BuildShaderModelFailureDiagnostic(shaderModelSupport);
			NLS_LOG_ERROR("CreateDX12RhiDevice: " + resources.creationDiagnostics);
			return resources;
		}

		resources.confirmedShaderModel = static_cast<unsigned int>(shaderModelSupport.highestShaderModel);
		resources.shaderModelDiagnostics = "DX12 reported Shader Model " + ShaderModelToString(shaderModelSupport.highestShaderModel);
		if (!shaderModelSupport.supported)
		{
			resources.creationDiagnostics =
				BuildShaderModelFailureDiagnostic(shaderModelSupport) +
				"; DX12 backend initialization stopped before rendering";
			NLS_LOG_ERROR("CreateDX12RhiDevice: " + resources.creationDiagnostics);
			return resources;
		}
		resources.shaderModel6Supported = true;
		NLS_LOG_INFO("CreateDX12RhiDevice: " + resources.shaderModelDiagnostics);

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
			resources.creationDiagnostics = "failed to create DX12 command queue";
			NLS_LOG_ERROR("CreateDX12RhiDevice: " + resources.creationDiagnostics);
			return resources;
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

		resources.capabilities = BuildDX12Capabilities(resources.device.Get(), resources.computeQueue.Get());
		resources.vendor = "DX12";
#else
		(void)debugMode;
		NLS_LOG_WARNING("CreateDX12RhiDevice: DX12 only supported on Windows");
#endif
		return resources;
	}
}
