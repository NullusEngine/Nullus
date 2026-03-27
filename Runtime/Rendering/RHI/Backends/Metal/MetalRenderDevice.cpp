#include "Rendering/RHI/Backends/Metal/MetalRenderDevice.h"

#include <Debug/Logger.h>
#include <functional>
#include <utility>

namespace NLS::Render::Backend
{
	namespace
	{
		class MetalTextureResource final : public NLS::Render::RHI::IRHITexture
		{
		public:
			MetalTextureResource(uint32_t id, NLS::Render::RHI::TextureDesc desc, std::function<void(uint32_t)> destroy)
				: m_id(id), m_desc(desc), m_destroy(std::move(destroy))
			{
			}

			~MetalTextureResource() override
			{
				if (m_destroy && m_id != 0)
					m_destroy(m_id);
			}

			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Texture; }
			uint32_t GetResourceId() const override { return m_id; }
			NLS::Render::RHI::TextureDimension GetDimension() const override { return m_desc.dimension; }
			const NLS::Render::RHI::TextureDesc& GetDesc() const override { return m_desc; }
			void SetDesc(const NLS::Render::RHI::TextureDesc& desc) override { m_desc = desc; }

		private:
			uint32_t m_id = 0;
			NLS::Render::RHI::TextureDesc m_desc{};
			std::function<void(uint32_t)> m_destroy;
		};

		class MetalBufferResource final : public NLS::Render::RHI::IRHIBuffer
		{
		public:
			MetalBufferResource(uint32_t id, NLS::Render::RHI::BufferType type, std::function<void(uint32_t)> destroy)
				: m_id(id), m_type(type), m_destroy(std::move(destroy))
			{
			}

			~MetalBufferResource() override
			{
				if (m_destroy && m_id != 0)
					m_destroy(m_id);
			}

			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Buffer; }
			uint32_t GetResourceId() const override { return m_id; }
			NLS::Render::RHI::BufferType GetBufferType() const override { return m_type; }
			size_t GetSize() const override { return m_size; }
			void SetSize(size_t size) override { m_size = size; }

		private:
			uint32_t m_id = 0;
			NLS::Render::RHI::BufferType m_type = NLS::Render::RHI::BufferType::ShaderStorage;
			size_t m_size = 0;
			std::function<void(uint32_t)> m_destroy;
		};
	}

	struct MetalRenderDevice::Impl
	{
		NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
		NLS::Render::RHI::NativeRenderDeviceInfo nativeInfo{};
		std::string vendor = "Metal";
		std::string hardware = "Unavailable";
		std::string version = "Metal";
		std::string shadingLanguageVersion = "MSL";
		uint32_t nextTextureId = 1;
		uint32_t nextBufferId = 1;
		bool backendReady = false;
	};

	MetalRenderDevice::MetalRenderDevice() : m_impl(std::make_unique<Impl>()) {}
	MetalRenderDevice::~MetalRenderDevice() = default;

	std::optional<NLS::Render::Data::PipelineState> MetalRenderDevice::Init(const NLS::Render::Settings::DriverSettings&)
	{
#if defined(__APPLE__) && NLS_HAS_METAL
		m_impl->capabilities.supportsGraphics = true;
		m_impl->capabilities.supportsCompute = true;
		m_impl->capabilities.supportsSwapchain = true;
		m_impl->nativeInfo.backend = NLS::Render::RHI::NativeBackendType::Metal;
		NLS_LOG_WARNING("Metal backend scaffolding is enabled, but native encoder/swapchain execution is not implemented yet.");
#else
		NLS_LOG_WARNING("Metal backend requested on a non-Apple or non-Metal build.");
#endif
		return NLS::Render::Data::PipelineState{};
	}

	void MetalRenderDevice::Clear(bool, bool, bool) {}
	void MetalRenderDevice::ReadPixels(uint32_t, uint32_t, uint32_t, uint32_t, NLS::Render::Settings::EPixelDataFormat, NLS::Render::Settings::EPixelDataType, void*) {}
	void MetalRenderDevice::DrawElements(NLS::Render::Settings::EPrimitiveMode, uint32_t) {}
	void MetalRenderDevice::DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t) { DrawElements(primitiveMode, indexCount); }
	void MetalRenderDevice::DrawArrays(NLS::Render::Settings::EPrimitiveMode, uint32_t) {}
	void MetalRenderDevice::DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t) { DrawArrays(primitiveMode, vertexCount); }
	void MetalRenderDevice::SetClearColor(float, float, float, float) {}
	void MetalRenderDevice::SetRasterizationLinesWidth(float) {}
	void MetalRenderDevice::SetRasterizationMode(NLS::Render::Settings::ERasterizationMode) {}
	void MetalRenderDevice::SetCapability(NLS::Render::Settings::ERenderingCapability, bool) {}
	bool MetalRenderDevice::GetCapability(NLS::Render::Settings::ERenderingCapability) { return false; }
	void MetalRenderDevice::SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm, int32_t, uint32_t) {}
	void MetalRenderDevice::SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm) {}
	void MetalRenderDevice::SetStencilMask(uint32_t) {}
	void MetalRenderDevice::SetStencilOperations(NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation) {}
	void MetalRenderDevice::SetCullFace(NLS::Render::Settings::ECullFace) {}
	void MetalRenderDevice::SetDepthWriting(bool) {}
	void MetalRenderDevice::SetColorWriting(bool, bool, bool, bool) {}
	void MetalRenderDevice::SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) {}
	void MetalRenderDevice::BindGraphicsPipeline(const NLS::Render::RHI::GraphicsPipelineDesc&, const NLS::Render::Resources::BindingSetInstance*) {}

	std::shared_ptr<NLS::Render::RHI::IRHITexture> MetalRenderDevice::CreateTextureResource(NLS::Render::RHI::TextureDimension dimension)
	{
		NLS::Render::RHI::TextureDesc desc;
		desc.dimension = dimension;
		return std::make_shared<MetalTextureResource>(CreateTexture(), desc, [this](uint32_t id) { DestroyTexture(id); });
	}

	uint32_t MetalRenderDevice::CreateTexture() { return m_impl->nextTextureId++; }
	void MetalRenderDevice::DestroyTexture(uint32_t) {}
	void MetalRenderDevice::BindTexture(NLS::Render::RHI::TextureDimension, uint32_t) {}
	void MetalRenderDevice::ActivateTexture(uint32_t) {}
	void MetalRenderDevice::SetupTexture(const NLS::Render::RHI::TextureDesc&, const void*) {}
	void MetalRenderDevice::GenerateTextureMipmap(NLS::Render::RHI::TextureDimension) {}
	uint32_t MetalRenderDevice::CreateFramebuffer() { return 0; }
	void MetalRenderDevice::DestroyFramebuffer(uint32_t) {}
	void MetalRenderDevice::BindFramebuffer(uint32_t) {}
	void MetalRenderDevice::AttachFramebufferColorTexture(uint32_t, uint32_t, uint32_t) {}
	void MetalRenderDevice::AttachFramebufferDepthStencilTexture(uint32_t, uint32_t) {}
	void MetalRenderDevice::SetFramebufferDrawBufferCount(uint32_t, uint32_t) {}
	void MetalRenderDevice::BlitDepth(uint32_t, uint32_t, uint32_t, uint32_t) {}

	std::shared_ptr<NLS::Render::RHI::IRHIBuffer> MetalRenderDevice::CreateBufferResource(NLS::Render::RHI::BufferType type)
	{
		return std::make_shared<MetalBufferResource>(CreateBuffer(), type, [this](uint32_t id) { DestroyBuffer(id); });
	}

	uint32_t MetalRenderDevice::CreateBuffer() { return m_impl->nextBufferId++; }
	void MetalRenderDevice::DestroyBuffer(uint32_t) {}
	void MetalRenderDevice::BindBuffer(NLS::Render::RHI::BufferType, uint32_t) {}
	void MetalRenderDevice::BindBufferBase(NLS::Render::RHI::BufferType, uint32_t, uint32_t) {}
	void MetalRenderDevice::SetBufferData(NLS::Render::RHI::BufferType, size_t, const void*, NLS::Render::RHI::BufferUsage) {}
	void MetalRenderDevice::SetBufferSubData(NLS::Render::RHI::BufferType, size_t, size_t, const void*) {}
	void* MetalRenderDevice::GetUITextureHandle(uint32_t) const { return nullptr; }
	void MetalRenderDevice::ReleaseUITextureHandles() {}
	bool MetalRenderDevice::PrepareUIRender() { return false; }
	std::string MetalRenderDevice::GetVendor() { return m_impl->vendor; }
	std::string MetalRenderDevice::GetHardware() { return m_impl->hardware; }
	std::string MetalRenderDevice::GetVersion() { return m_impl->version; }
	std::string MetalRenderDevice::GetShadingLanguageVersion() { return m_impl->shadingLanguageVersion; }
	NLS::Render::RHI::RHIDeviceCapabilities MetalRenderDevice::GetCapabilities() const { return m_impl->capabilities; }
	NLS::Render::RHI::NativeRenderDeviceInfo MetalRenderDevice::GetNativeDeviceInfo() const { return m_impl->nativeInfo; }
	bool MetalRenderDevice::IsBackendReady() const { return m_impl->backendReady; }
	bool MetalRenderDevice::CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) { return m_impl->backendReady; }
	void MetalRenderDevice::DestroySwapchain() {}
	void MetalRenderDevice::ResizeSwapchain(uint32_t, uint32_t) {}
	void MetalRenderDevice::PresentSwapchain() {}
}
