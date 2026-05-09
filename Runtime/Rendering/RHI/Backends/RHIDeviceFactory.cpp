#include "Rendering/RHI/Backends/RHIDeviceFactory.h"

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
#endif
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Debug/Logger.h"

#include <string>

namespace NLS::Render::Backend
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12Device(const NLS::Render::Settings::DriverSettings& settings);

	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateRhiDevice(const NLS::Render::Settings::DriverSettings& settings)
	{
		NLS_LOG_INFO(std::string("CreateRhiDevice: requested backend = ") + Render::Settings::ToString(settings.graphicsBackend));
		if (settings.graphicsBackend == NLS::Render::Settings::EGraphicsBackend::NONE)
		{
			NLS_LOG_WARNING("CreateRhiDevice: None backend requested; no runtime RHI device will be created.");
			return nullptr;
		}

		if (!Render::Settings::IsBackendEnabledForCurrentBuild(settings.graphicsBackend))
		{
			const auto restriction = Render::Settings::GetPhase1BackendRestrictionMessage(
				settings.graphicsBackend,
				"CreateRhiDevice");
			NLS_LOG_ERROR(
				restriction.has_value()
					? *restriction
					: std::string("CreateRhiDevice: ") +
						Render::Settings::ToString(settings.graphicsBackend) +
						" is gated unsupported in the current runtime validation matrix.");
			return nullptr;
		}

		if (settings.graphicsBackend != NLS::Render::Settings::EGraphicsBackend::DX12)
		{
			NLS_LOG_ERROR(
				std::string("CreateRhiDevice: phase-1 runtime only allows DX12 through the top-level selector. Requested backend: ") +
				Render::Settings::ToString(settings.graphicsBackend));
			return nullptr;
		}

#if defined(_WIN32)
		auto device = CreateDX12Device(settings);
		if (device == nullptr)
		{
			NLS_LOG_ERROR(
				"CreateRhiDevice: DX12 device creation failed and the phase-1 runtime does not permit alternate backend selection.");
		}
		return device;
#else
		NLS_LOG_ERROR("CreateRhiDevice: DX12 is unavailable on this platform/build.");
		return nullptr;
#endif
	}

#if defined(_WIN32)
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12Device(const NLS::Render::Settings::DriverSettings& settings)
	{
		return CreateDX12RhiDevice(settings.debugMode);
	}
#endif

	std::shared_ptr<NLS::Render::RHI::UploadContext> CreateUploadContextForRhiDevice(
		const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
	{
		if (device == nullptr)
			return NLS::Render::RHI::CreateDefaultUploadContext();

#if defined(_WIN32)
		const auto nativeInfo = device->GetNativeDeviceInfo();
		if (nativeInfo.backend == NLS::Render::RHI::NativeBackendType::DX12 && nativeInfo.device != nullptr)
		{
			auto backend = CreateDX12UploadBackend(static_cast<ID3D12Device*>(nativeInfo.device));
			if (backend != nullptr)
				return NLS::Render::RHI::CreateBackendUploadContext(std::move(backend));
		}
#endif

		return NLS::Render::RHI::CreateDefaultUploadContext();
	}
}
