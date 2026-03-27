#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "RenderDef.h"

namespace NLS::Render::RHI
{
	enum class NLS_RENDER_API TextureDimension : uint8_t
	{
		Texture2D,
		TextureCube
	};

	enum class NLS_RENDER_API ShaderStage : uint8_t
	{
		Vertex,
		Fragment,
		Compute
	};

	enum class NLS_RENDER_API TextureFormat : uint8_t
	{
		RGB8,
		RGBA8,
		RGBA16F,
		Depth24Stencil8
	};

	inline constexpr uint32_t GetTextureLayerCount(TextureDimension dimension)
	{
		switch (dimension)
		{
		case TextureDimension::TextureCube: return 6u;
		case TextureDimension::Texture2D:
		default:
			return 1u;
		}
	}

	inline constexpr uint32_t GetTextureFormatBytesPerPixel(TextureFormat format)
	{
		switch (format)
		{
		case TextureFormat::RGB8: return 3u;
		case TextureFormat::RGBA16F: return 8u;
		case TextureFormat::RGBA8:
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

	enum class NLS_RENDER_API TextureWrap : uint8_t
	{
		ClampToEdge,
		Repeat
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

	struct NLS_RENDER_API TextureDesc
	{
		uint16_t width = 0;
		uint16_t height = 0;
		TextureDimension dimension = TextureDimension::Texture2D;
		TextureFormat format = TextureFormat::RGBA8;
		TextureFilter minFilter = TextureFilter::Nearest;
		TextureFilter magFilter = TextureFilter::Nearest;
		TextureWrap wrapS = TextureWrap::ClampToEdge;
		TextureWrap wrapT = TextureWrap::ClampToEdge;
		TextureUsage usage = TextureUsage::Sampled;
	};

	struct NLS_RENDER_API SamplerDesc
	{
		TextureFilter minFilter = TextureFilter::Linear;
		TextureFilter magFilter = TextureFilter::Linear;
		TextureWrap wrapU = TextureWrap::Repeat;
		TextureWrap wrapV = TextureWrap::Repeat;
		TextureWrap wrapW = TextureWrap::Repeat;
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

	struct NLS_RENDER_API BufferDesc
	{
		size_t size = 0;
		BufferType type = BufferType::ShaderStorage;
		BufferUsage usage = BufferUsage::DynamicDraw;
	};

	struct NLS_RENDER_API RHIDeviceCapabilities
	{
		bool backendReady = false;
		bool supportsGraphics = true;
		bool supportsCompute = false;
		bool supportsSwapchain = false;
		bool supportsFramebufferBlit = false;
		bool supportsDepthBlit = false;
		bool supportsCurrentSceneRenderer = false;
		bool supportsOffscreenFramebuffers = false;
		bool supportsFramebufferReadback = false;
		bool supportsUITextureHandles = false;
		bool supportsCubemaps = false;
		bool supportsMultiRenderTargets = false;
		uint32_t maxTextureDimension2D = 0;
		uint32_t maxColorAttachments = 0;
	};

	enum class NLS_RENDER_API NativeBackendType : uint8_t
	{
		None,
		OpenGL,
		Vulkan,
		DX12,
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
