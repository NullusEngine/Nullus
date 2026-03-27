#include "Rendering/RHI/Backends/DX11/DX11RenderDevice.h"

namespace NLS::Render::Backend
{
	namespace
	{
		NullRenderDeviceDescriptor BuildDX11Descriptor()
		{
			NullRenderDeviceDescriptor descriptor;
			descriptor.backend = NLS::Render::RHI::NativeBackendType::DX11;
			descriptor.vendor = "Direct3D 11";
			descriptor.hardware = "Tier B compatibility scaffold";
			descriptor.version = "Stub backend";
			descriptor.shadingLanguageVersion = "HLSL";
			// This slice only establishes formal RHI routing and backend identity for now.
			descriptor.capabilities.backendReady = false;
			descriptor.capabilities.supportsCurrentSceneRenderer = false;
			descriptor.capabilities.supportsCompute = false;
			descriptor.capabilities.supportsSwapchain = false;
			descriptor.capabilities.supportsFramebufferBlit = false;
			descriptor.capabilities.supportsDepthBlit = false;
			descriptor.capabilities.supportsOffscreenFramebuffers = false;
			descriptor.capabilities.supportsFramebufferReadback = false;
			descriptor.capabilities.supportsUITextureHandles = false;
			descriptor.capabilities.supportsCubemaps = false;
			descriptor.capabilities.supportsMultiRenderTargets = false;
			return descriptor;
		}
	}

	DX11RenderDevice::DX11RenderDevice()
		: NullRenderDevice(BuildDX11Descriptor())
	{
	}
}
