#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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
		BC1,
		BC3,
		BC5,
		BC7,
		BC6H,
		ASTC4x4,
		ETC2RGBA8,
		Depth32F,
		Depth24Stencil8,
		Count
	};

	enum class NLS_RENDER_API TextureColorSpace : uint8_t
	{
		Linear = 0,
		SRGB = 1
	};

	enum class NLS_RENDER_API TextureFormatFamily : uint8_t
	{
		Unknown = 0,
		Uncompressed,
		BC,
		ASTC,
		ETC2,
		DepthStencil
	};

	struct NLS_RENDER_API TextureFormatDescriptor
	{
		TextureFormat format = TextureFormat::RGBA8;
		const char* name = "";
		TextureFormatFamily family = TextureFormatFamily::Unknown;
		uint32_t blockWidth = 1u;
		uint32_t blockHeight = 1u;
		uint32_t blockDepth = 1u;
		uint32_t bytesPerBlock = 0u;
		uint32_t channelCount = 0u;
		bool isCompressed = false;
		bool hasAlpha = false;
		bool isHDR = false;
		bool isDepthStencil = false;
		bool supportsSrgbView = false;
		bool supportsUpload = false;
		bool sampled = false;
		bool colorAttachment = false;
		bool storage = false;
		bool requiresAlignedTopLevelBlocks = false;
	};

	struct NLS_RENDER_API TextureFormatCapability
	{
		TextureFormat format = TextureFormat::RGBA8;
		bool sampled = false;
		bool upload = false;
		bool colorAttachment = false;
		bool storage = false;
		bool supportsSrgbView = false;
		bool requiresAlignedTopLevelBlocks = false;
		bool supportsUnalignedBlockTextures = false;
		std::string diagnosticReason;
	};

	inline constexpr size_t TextureFormatToIndex(const TextureFormat format)
	{
		return static_cast<size_t>(format);
	}

	inline constexpr std::array<TextureFormatDescriptor, TextureFormatToIndex(TextureFormat::Count)> kTextureFormatDescriptors{ {
		{ TextureFormat::R8, "r8", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 1u, 1u, false, false, false, false, false, true, true, false, false, false },
		{ TextureFormat::RG8, "rg8", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 2u, 2u, false, false, false, false, false, true, true, false, false, false },
		{ TextureFormat::RGB8, "rgb8", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 3u, 3u, false, false, false, false, false, true, true, false, false, false },
		{ TextureFormat::RGBA8, "rgba8", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 4u, 4u, false, true, false, false, true, true, true, true, false, false },
		{ TextureFormat::R16F, "r16f", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 2u, 1u, false, false, true, false, false, true, true, false, false, false },
		{ TextureFormat::RG16F, "rg16f", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 4u, 2u, false, false, true, false, false, true, true, false, false, false },
		{ TextureFormat::RGBA16F, "rgba16f", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 8u, 4u, false, true, true, false, false, true, true, true, false, false },
		{ TextureFormat::R32F, "r32f", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 4u, 1u, false, false, true, false, false, true, true, false, false, false },
		{ TextureFormat::RG32F, "rg32f", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 8u, 2u, false, false, true, false, false, true, true, false, false, false },
		{ TextureFormat::RGBA32F, "rgba32f", TextureFormatFamily::Uncompressed, 1u, 1u, 1u, 16u, 4u, false, true, true, false, false, true, true, false, false, false },
		{ TextureFormat::BC1, "bc1", TextureFormatFamily::BC, 4u, 4u, 1u, 8u, 4u, true, true, false, false, true, true, true, false, false, true },
		{ TextureFormat::BC3, "bc3", TextureFormatFamily::BC, 4u, 4u, 1u, 16u, 4u, true, true, false, false, true, true, true, false, false, true },
		{ TextureFormat::BC5, "bc5", TextureFormatFamily::BC, 4u, 4u, 1u, 16u, 2u, true, false, false, false, false, true, true, false, false, true },
		{ TextureFormat::BC7, "bc7", TextureFormatFamily::BC, 4u, 4u, 1u, 16u, 4u, true, true, false, false, true, true, true, false, false, true },
		{ TextureFormat::BC6H, "bc6h", TextureFormatFamily::BC, 4u, 4u, 1u, 16u, 3u, true, false, true, false, false, false, false, false, false, true },
		{ TextureFormat::ASTC4x4, "astc4x4", TextureFormatFamily::ASTC, 4u, 4u, 1u, 16u, 4u, true, true, false, false, true, false, false, false, false, true },
		{ TextureFormat::ETC2RGBA8, "etc2-rgba8", TextureFormatFamily::ETC2, 4u, 4u, 1u, 16u, 4u, true, true, false, false, true, false, false, false, false, true },
		{ TextureFormat::Depth32F, "depth32f", TextureFormatFamily::DepthStencil, 1u, 1u, 1u, 4u, 1u, false, false, false, true, false, false, false, false, false, false },
		{ TextureFormat::Depth24Stencil8, "depth24stencil8", TextureFormatFamily::DepthStencil, 1u, 1u, 1u, 4u, 2u, false, false, false, true, false, false, false, false, false, false }
	} };

	inline constexpr const TextureFormatDescriptor* GetTextureFormatDescriptor(const TextureFormat format)
	{
		const size_t index = TextureFormatToIndex(format);
		return index < kTextureFormatDescriptors.size()
			? &kTextureFormatDescriptors[index]
			: nullptr;
	}

	inline constexpr const char* GetTextureFormatName(const TextureFormat format)
	{
		const auto* descriptor = GetTextureFormatDescriptor(format);
		return descriptor != nullptr ? descriptor->name : "unknown";
	}

	inline constexpr TextureFormat ParseTextureFormatName(const std::string_view name)
	{
		if (name == "etc2")
			return TextureFormat::ETC2RGBA8;

		for (const auto& descriptor : kTextureFormatDescriptors)
		{
			if (name == descriptor.name)
				return descriptor.format;
		}
		return TextureFormat::Count;
	}

	inline constexpr bool IsTextureFormatCompressed(const TextureFormat format)
	{
		const auto* descriptor = GetTextureFormatDescriptor(format);
		return descriptor != nullptr && descriptor->isCompressed;
	}

	inline constexpr uint32_t CalculateTextureRowPitch(const TextureFormat format, const uint32_t width)
	{
		const auto* descriptor = GetTextureFormatDescriptor(format);
		if (descriptor == nullptr || descriptor->bytesPerBlock == 0u || descriptor->blockWidth == 0u)
			return 0u;

		const uint32_t clampedWidth = (std::max)(width, 1u);
		const uint32_t blocksWide = (clampedWidth + descriptor->blockWidth - 1u) / descriptor->blockWidth;
		return blocksWide * descriptor->bytesPerBlock;
	}

	inline constexpr uint32_t CalculateTextureSlicePitch(
		const TextureFormat format,
		const uint32_t width,
		const uint32_t height,
		const uint32_t depth = 1u)
	{
		const auto* descriptor = GetTextureFormatDescriptor(format);
		if (descriptor == nullptr ||
			descriptor->bytesPerBlock == 0u ||
			descriptor->blockHeight == 0u ||
			descriptor->blockDepth == 0u)
		{
			return 0u;
		}

		const uint32_t rowPitch = CalculateTextureRowPitch(format, width);
		const uint32_t clampedHeight = (std::max)(height, 1u);
		const uint32_t clampedDepth = (std::max)(depth, 1u);
		const uint32_t blocksHigh = (clampedHeight + descriptor->blockHeight - 1u) / descriptor->blockHeight;
		const uint32_t blocksDeep = (clampedDepth + descriptor->blockDepth - 1u) / descriptor->blockDepth;
		return rowPitch * blocksHigh * blocksDeep;
	}

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
		case TextureFormat::BC1:
		case TextureFormat::BC3:
		case TextureFormat::BC5:
		case TextureFormat::BC7:
		case TextureFormat::BC6H:
		case TextureFormat::ASTC4x4:
		case TextureFormat::ETC2RGBA8:
			return 0u;
		case TextureFormat::Depth32F:
		case TextureFormat::Depth24Stencil8: return 4u;
		case TextureFormat::Count:
			return 0u;
		case TextureFormat::RGBA8: return 4u;
		default:
			return 0u;
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
		InRenderPassChildCommandBuffers,
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
		bool supportsInRenderPassChildCommandBuffers = false;
		bool supportsTransientResourceAllocator = false;
		bool supportsCentralizedDescriptorManagement = false;
		bool supportsPipelineStateCache = false;
		uint32_t maxTextureDimension2D = 0;
		uint32_t maxColorAttachments = 0;
		RHIDeviceLimits limits{};
		std::array<RHIDeviceFeatureState, static_cast<size_t>(RHIDeviceFeature::Count)> features{};
		std::array<TextureFormatCapability, TextureFormatToIndex(TextureFormat::Count)> textureFormatCapabilities{};

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
			SetFeatureStateFromLegacy(RHIDeviceFeature::InRenderPassChildCommandBuffers, supportsInRenderPassChildCommandBuffers);
			SetFeatureStateFromLegacy(RHIDeviceFeature::TransientResourceAllocator, supportsTransientResourceAllocator);
			SetFeatureStateFromLegacy(RHIDeviceFeature::CentralizedDescriptorManagement, supportsCentralizedDescriptorManagement);
			SetFeatureStateFromLegacy(RHIDeviceFeature::PipelineStateCache, supportsPipelineStateCache);
		}

		const TextureFormatCapability& GetTextureFormatCapability(TextureFormat format) const
		{
			static const TextureFormatCapability kUnknownCapability{};
			const size_t index = TextureFormatToIndex(format);
			return index < textureFormatCapabilities.size()
				? textureFormatCapabilities[index]
				: kUnknownCapability;
		}

		void SetTextureFormatCapability(const TextureFormat format, TextureFormatCapability capability)
		{
			const size_t index = TextureFormatToIndex(format);
			if (index >= textureFormatCapabilities.size())
				return;

			capability.format = format;
			textureFormatCapabilities[index] = std::move(capability);
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
			case RHIDeviceFeature::InRenderPassChildCommandBuffers: supportsInRenderPassChildCommandBuffers = supported; break;
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

	enum class NLS_RENDER_API NativeRenderDeviceHandleKind : uint8_t
	{
		Unknown,
		DX12,
		Vulkan,
		OpenGL,
		Metal,
		DX11
	};

	struct NLS_RENDER_API NativeRenderDeviceHandle
	{
		NativeRenderDeviceHandleKind backend = NativeRenderDeviceHandleKind::Unknown;
		void* handle = nullptr;

		bool IsValid() const { return backend != NativeRenderDeviceHandleKind::Unknown && handle != nullptr; }
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

		NativeRenderDeviceHandleKind GetTaggedBackend() const
		{
			switch (backend)
			{
			case NativeBackendType::DX12: return NativeRenderDeviceHandleKind::DX12;
			case NativeBackendType::Vulkan: return NativeRenderDeviceHandleKind::Vulkan;
			case NativeBackendType::OpenGL: return NativeRenderDeviceHandleKind::OpenGL;
			case NativeBackendType::Metal: return NativeRenderDeviceHandleKind::Metal;
			case NativeBackendType::DX11: return NativeRenderDeviceHandleKind::DX11;
			case NativeBackendType::None:
			default:
				return NativeRenderDeviceHandleKind::Unknown;
			}
		}

		NativeRenderDeviceHandle MakeHandle(void* nativeHandle) const
		{
			return { GetTaggedBackend(), nativeHandle };
		}

		NativeRenderDeviceHandle GetInstanceHandle() const { return MakeHandle(instance); }
		NativeRenderDeviceHandle GetPhysicalDeviceHandle() const { return MakeHandle(physicalDevice); }
		NativeRenderDeviceHandle GetDeviceHandle() const { return MakeHandle(device); }
		NativeRenderDeviceHandle GetGraphicsQueueHandle() const { return MakeHandle(graphicsQueue); }
		NativeRenderDeviceHandle GetSurfaceHandle() const { return MakeHandle(surface); }
		NativeRenderDeviceHandle GetSwapchainHandle() const { return MakeHandle(swapchain); }
		NativeRenderDeviceHandle GetPlatformWindowHandle() const { return MakeHandle(platformWindow); }
		NativeRenderDeviceHandle GetNativeWindowHandle() const { return MakeHandle(nativeWindowHandle); }
		NativeRenderDeviceHandle GetCurrentCommandBufferHandle() const { return MakeHandle(currentCommandBuffer); }
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
