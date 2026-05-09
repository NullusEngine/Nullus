#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"

namespace NLS::Render::RHI
{
	enum class NLS_RENDER_API TextureDimension : uint8_t
	{
		Texture2D = 0,
		TextureCube = 1,
		Texture1D,
		Texture2DArray,
		Texture3D,
		TextureCubeArray
	};

	enum class NLS_RENDER_API ShaderStage : uint8_t
	{
		Vertex,
		Fragment,
		Compute
	};

	enum class NLS_RENDER_API TextureFormat : uint8_t
	{
		R8,
		RG8,
		RGB8,
		RGBA8,
		R16F,
		RG16F,
		RGBA16F,
		R32F,
		RG32F,
		RGBA32F,
		Depth32F,
		Depth24Stencil8
	};

	inline constexpr uint32_t GetTextureLayerCount(TextureDimension dimension, uint32_t requestedArrayLayers = 1u)
	{
		switch (dimension)
		{
		case TextureDimension::TextureCube: return 6u;
		case TextureDimension::TextureCubeArray: return requestedArrayLayers >= 6u ? requestedArrayLayers : 6u;
		case TextureDimension::Texture2DArray: return requestedArrayLayers > 0u ? requestedArrayLayers : 1u;
		case TextureDimension::Texture1D:
		case TextureDimension::Texture3D:
		case TextureDimension::Texture2D:
		default:
			return 1u;
		}
	}

	inline constexpr uint32_t GetTextureFormatBytesPerPixel(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::R8: return 1u;
		case TextureFormat::RG8: return 2u;
		case TextureFormat::RGB8: return 3u;
		case TextureFormat::R16F: return 2u;
		case TextureFormat::RG16F: return 4u;
		case TextureFormat::RGBA16F: return 8u;
		case TextureFormat::R32F: return 4u;
		case TextureFormat::RG32F: return 8u;
		case TextureFormat::RGBA32F: return 16u;
		case TextureFormat::RGBA8:
		case TextureFormat::Depth32F:
		case TextureFormat::Depth24Stencil8:
		default:
			return 4u;
		}
	}

	enum class NLS_RENDER_API TextureFilter : uint8_t
	{
		Nearest,
		Linear
	};

	enum class NLS_RENDER_API TextureMipFilter : uint8_t
	{
		Nearest,
		Linear
	};

	enum class NLS_RENDER_API TextureWrap : uint8_t
	{
		ClampToEdge,
		Repeat,
		MirrorRepeat,
		ClampToBorder
	};

	enum class NLS_RENDER_API TextureUsage : uint32_t
	{
		None = 0,
		Sampled = 1u << 0,
		ColorAttachment = 1u << 1,
		DepthStencilAttachment = 1u << 2,
		Storage = 1u << 3
	};

	inline constexpr TextureUsage operator|(TextureUsage lhs, TextureUsage rhs)
	{
		return static_cast<TextureUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
	}

	inline constexpr bool HasUsage(TextureUsage usage, TextureUsage flag)
	{
		return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0u;
	}

	enum class NLS_RENDER_API BufferType : uint8_t
	{
		Vertex,
		Index,
		ShaderStorage,
		Uniform
	};

	enum class NLS_RENDER_API BufferUsage : uint8_t
	{
		StaticDraw,
		DynamicDraw,
		StreamDraw
	};

	struct NLS_RENDER_API SamplerDesc
	{
		static constexpr float UnboundedMaxLod = 3.402823466e+38F;

		TextureFilter minFilter = TextureFilter::Linear;
		TextureFilter magFilter = TextureFilter::Linear;
		TextureMipFilter mipFilter = TextureMipFilter::Linear;
		TextureWrap wrapU = TextureWrap::Repeat;
		TextureWrap wrapV = TextureWrap::Repeat;
		TextureWrap wrapW = TextureWrap::Repeat;
		uint32_t maxAnisotropy = 1u;
		float minLod = 0.0f;
		float maxLod = UnboundedMaxLod;
		float mipLodBias = 0.0f;
		bool compareEnabled = false;
		NLS::Render::Settings::EComparaisonAlgorithm compareFunc =
			NLS::Render::Settings::EComparaisonAlgorithm::NEVER;
		std::array<float, 4> borderColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	};

	struct NLS_RENDER_API FramebufferColorAttachmentDesc
	{
		uint32_t textureId = 0;
		TextureFormat format = TextureFormat::RGBA8;
	};

	struct NLS_RENDER_API FramebufferDesc
	{
		std::vector<FramebufferColorAttachmentDesc> colorAttachments;
		uint32_t depthStencilTextureId = 0;
		TextureFormat depthStencilFormat = TextureFormat::Depth24Stencil8;
		uint32_t drawBufferCount = 0;
	};

	enum class NLS_RENDER_API RHIDeviceFeature : uint8_t
	{
		BackendReady,
		Graphics,
		Compute,
		AsyncCompute,
		DedicatedComputeQueue,
		CopyQueue,
		Swapchain,
		FramebufferBlit,
		DepthBlit,
		CurrentSceneRenderer,
		OffscreenFramebuffers,
		FramebufferReadback,
		EditorPickingReadback,
		UITextureHandles,
		Cubemaps,
		MultiRenderTargets,
		ExplicitBarriers,
		ParallelCommandRecording,
		ParallelCommandTranslation,
		TransientResourceAllocator,
		CentralizedDescriptorManagement,
		PipelineStateCache,
		Count
	};

	struct NLS_RENDER_API RHIDeviceFeatureState
	{
		bool supported = false;
		std::string reason;
	};

	struct NLS_RENDER_API RHIDeviceLimits
	{
		uint32_t maxTextureDimension2D = 0;
		uint32_t maxColorAttachments = 0;
	};

	struct NLS_RENDER_API RHIDeviceCapabilities
	{
		bool backendReady = false;
		bool supportsGraphics = true;
		bool supportsCompute = false;
		bool supportsAsyncCompute = false;
		bool supportsDedicatedComputeQueue = false;
		bool supportsCopyQueue = false;
		bool supportsSwapchain = false;
		bool supportsFramebufferBlit = false;
		bool supportsDepthBlit = false;
		bool supportsCurrentSceneRenderer = false;
		bool supportsOffscreenFramebuffers = false;
		bool supportsFramebufferReadback = false;
		bool supportsEditorPickingReadback = false;
		bool supportsUITextureHandles = false;
		bool supportsCubemaps = false;
		bool supportsMultiRenderTargets = false;
		bool supportsExplicitBarriers = false;
		bool supportsParallelCommandRecording = false;
		bool supportsParallelCommandTranslation = false;
		bool supportsTransientResourceAllocator = false;
		bool supportsCentralizedDescriptorManagement = false;
		bool supportsPipelineStateCache = false;
		uint32_t maxTextureDimension2D = 0;
		uint32_t maxColorAttachments = 0;
		RHIDeviceLimits limits{};
		std::array<RHIDeviceFeatureState, static_cast<size_t>(RHIDeviceFeature::Count)> features{};

		RHIDeviceFeatureState GetFeature(RHIDeviceFeature feature) const
		{
			auto copy = *this;
			copy.SynchronizeLegacyFields();
			return copy.features[static_cast<size_t>(feature)];
		}

		void SetFeature(RHIDeviceFeature feature, bool supported, std::string reason = {})
		{
			features[static_cast<size_t>(feature)] = { supported, std::move(reason) };
			SetLegacyFeatureFlag(feature, supported);
		}

		void SynchronizeLegacyFields()
		{
			limits.maxTextureDimension2D = maxTextureDimension2D;
			limits.maxColorAttachments = maxColorAttachments;
			SetFeatureStateFromLegacy(RHIDeviceFeature::BackendReady, backendReady);
			SetFeatureStateFromLegacy(RHIDeviceFeature::Graphics, supportsGraphics);
			SetFeatureStateFromLegacy(RHIDeviceFeature::Compute, supportsCompute);
			SetFeatureStateFromLegacy(RHIDeviceFeature::AsyncCompute, supportsAsyncCompute);
			SetFeatureStateFromLegacy(RHIDeviceFeature::DedicatedComputeQueue, supportsDedicatedComputeQueue);
			SetFeatureStateFromLegacy(RHIDeviceFeature::CopyQueue, supportsCopyQueue);
			SetFeatureStateFromLegacy(RHIDeviceFeature::Swapchain, supportsSwapchain);
			SetFeatureStateFromLegacy(RHIDeviceFeature::FramebufferBlit, supportsFramebufferBlit);
			SetFeatureStateFromLegacy(RHIDeviceFeature::DepthBlit, supportsDepthBlit);
			SetFeatureStateFromLegacy(RHIDeviceFeature::CurrentSceneRenderer, supportsCurrentSceneRenderer);
			SetFeatureStateFromLegacy(RHIDeviceFeature::OffscreenFramebuffers, supportsOffscreenFramebuffers);
			SetFeatureStateFromLegacy(RHIDeviceFeature::FramebufferReadback, supportsFramebufferReadback);
			SetFeatureStateFromLegacy(RHIDeviceFeature::EditorPickingReadback, supportsEditorPickingReadback);
			SetFeatureStateFromLegacy(RHIDeviceFeature::UITextureHandles, supportsUITextureHandles);
			SetFeatureStateFromLegacy(RHIDeviceFeature::Cubemaps, supportsCubemaps);
			SetFeatureStateFromLegacy(RHIDeviceFeature::MultiRenderTargets, supportsMultiRenderTargets);
			SetFeatureStateFromLegacy(RHIDeviceFeature::ExplicitBarriers, supportsExplicitBarriers);
			SetFeatureStateFromLegacy(RHIDeviceFeature::ParallelCommandRecording, supportsParallelCommandRecording);
			SetFeatureStateFromLegacy(RHIDeviceFeature::ParallelCommandTranslation, supportsParallelCommandTranslation);
			SetFeatureStateFromLegacy(RHIDeviceFeature::TransientResourceAllocator, supportsTransientResourceAllocator);
			SetFeatureStateFromLegacy(RHIDeviceFeature::CentralizedDescriptorManagement, supportsCentralizedDescriptorManagement);
			SetFeatureStateFromLegacy(RHIDeviceFeature::PipelineStateCache, supportsPipelineStateCache);
		}

	private:
		void SetFeatureStateFromLegacy(RHIDeviceFeature feature, bool supported)
		{
			auto& state = features[static_cast<size_t>(feature)];
			state.supported = supported;
			if (supported)
				state.reason.clear();
			else if (state.reason.empty())
				state.reason = "Feature is not reported by this RHI device";
		}

		void SetLegacyFeatureFlag(RHIDeviceFeature feature, bool supported)
		{
			switch (feature)
			{
			case RHIDeviceFeature::BackendReady: backendReady = supported; break;
			case RHIDeviceFeature::Graphics: supportsGraphics = supported; break;
			case RHIDeviceFeature::Compute: supportsCompute = supported; break;
			case RHIDeviceFeature::AsyncCompute: supportsAsyncCompute = supported; break;
			case RHIDeviceFeature::DedicatedComputeQueue: supportsDedicatedComputeQueue = supported; break;
			case RHIDeviceFeature::CopyQueue: supportsCopyQueue = supported; break;
			case RHIDeviceFeature::Swapchain: supportsSwapchain = supported; break;
			case RHIDeviceFeature::FramebufferBlit: supportsFramebufferBlit = supported; break;
			case RHIDeviceFeature::DepthBlit: supportsDepthBlit = supported; break;
			case RHIDeviceFeature::CurrentSceneRenderer: supportsCurrentSceneRenderer = supported; break;
			case RHIDeviceFeature::OffscreenFramebuffers: supportsOffscreenFramebuffers = supported; break;
			case RHIDeviceFeature::FramebufferReadback: supportsFramebufferReadback = supported; break;
			case RHIDeviceFeature::EditorPickingReadback: supportsEditorPickingReadback = supported; break;
			case RHIDeviceFeature::UITextureHandles: supportsUITextureHandles = supported; break;
			case RHIDeviceFeature::Cubemaps: supportsCubemaps = supported; break;
			case RHIDeviceFeature::MultiRenderTargets: supportsMultiRenderTargets = supported; break;
			case RHIDeviceFeature::ExplicitBarriers: supportsExplicitBarriers = supported; break;
			case RHIDeviceFeature::ParallelCommandRecording: supportsParallelCommandRecording = supported; break;
			case RHIDeviceFeature::ParallelCommandTranslation: supportsParallelCommandTranslation = supported; break;
			case RHIDeviceFeature::TransientResourceAllocator: supportsTransientResourceAllocator = supported; break;
			case RHIDeviceFeature::CentralizedDescriptorManagement: supportsCentralizedDescriptorManagement = supported; break;
			case RHIDeviceFeature::PipelineStateCache: supportsPipelineStateCache = supported; break;
			case RHIDeviceFeature::Count: break;
			}
		}
	};

	enum class NLS_RENDER_API NativeBackendType : uint8_t
	{
		None,
		OpenGL,
		Vulkan,
		DX12,
		DX11,
		Metal
	};

	struct NLS_RENDER_API NativeRenderDeviceInfo
	{
		NativeBackendType backend = NativeBackendType::None;
		void* instance = nullptr;
		void* physicalDevice = nullptr;
		void* device = nullptr;
		void* graphicsQueue = nullptr;
		void* surface = nullptr;
		void* swapchain = nullptr;
		void* uiRenderPass = nullptr;
		void* uiDescriptorPool = nullptr;
		void* platformWindow = nullptr;
		void* nativeWindowHandle = nullptr;
		void* currentCommandBuffer = nullptr;
		uint32_t graphicsQueueFamilyIndex = 0;
		uint32_t swapchainImageCount = 0;
	};

	struct NLS_RENDER_API SwapchainDesc
	{
		uint32_t width = 0;
		uint32_t height = 0;
		void* platformWindow = nullptr;
		void* nativeWindowHandle = nullptr;
		TextureFormat backbufferFormat = TextureFormat::RGBA8;
		uint32_t imageCount = 2;
		bool vsync = true;
	};
}
