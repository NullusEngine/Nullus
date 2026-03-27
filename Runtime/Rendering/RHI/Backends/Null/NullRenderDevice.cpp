#include "Rendering/RHI/Backends/Null/NullRenderDevice.h"

#include <memory>
#include <utility>

namespace NLS::Render::Backend
{
	namespace
	{
		class NullTextureResource final : public NLS::Render::RHI::IRHITexture
		{
		public:
			explicit NullTextureResource(NLS::Render::RHI::TextureDimension dimension) { m_desc.dimension = dimension; }
			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Texture; }
			uint32_t GetResourceId() const override { return 0; }
			NLS::Render::RHI::TextureDimension GetDimension() const override { return m_desc.dimension; }
			const NLS::Render::RHI::TextureDesc& GetDesc() const override { return m_desc; }
			void SetDesc(const NLS::Render::RHI::TextureDesc& desc) override { m_desc = desc; }
		private:
			NLS::Render::RHI::TextureDesc m_desc{};
		};

		class NullBufferResource final : public NLS::Render::RHI::IRHIBuffer
		{
		public:
			explicit NullBufferResource(NLS::Render::RHI::BufferType type) : m_type(type) {}
			NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Buffer; }
			uint32_t GetResourceId() const override { return 0; }
			NLS::Render::RHI::BufferType GetBufferType() const override { return m_type; }
			size_t GetSize() const override { return 0; }
			void SetSize(size_t) override {}
		private:
			NLS::Render::RHI::BufferType m_type;
		};
	}

	NullRenderDevice::NullRenderDevice(NullRenderDeviceDescriptor descriptor)
		: m_descriptor(std::move(descriptor))
	{
	}

	std::optional<NLS::Render::Data::PipelineState> NullRenderDevice::Init(const NLS::Render::Settings::DriverSettings&) { return NLS::Render::Data::PipelineState{}; }
	void NullRenderDevice::Clear(bool, bool, bool) {}
	void NullRenderDevice::ReadPixels(uint32_t, uint32_t, uint32_t, uint32_t, NLS::Render::Settings::EPixelDataFormat, NLS::Render::Settings::EPixelDataType, void*) {}
	void NullRenderDevice::DrawElements(NLS::Render::Settings::EPrimitiveMode, uint32_t) {}
	void NullRenderDevice::DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode, uint32_t, uint32_t) {}
	void NullRenderDevice::DrawArrays(NLS::Render::Settings::EPrimitiveMode, uint32_t) {}
	void NullRenderDevice::DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode, uint32_t, uint32_t) {}
	void NullRenderDevice::SetClearColor(float, float, float, float) {}
	void NullRenderDevice::SetRasterizationLinesWidth(float) {}
	void NullRenderDevice::SetRasterizationMode(NLS::Render::Settings::ERasterizationMode) {}
	void NullRenderDevice::SetCapability(NLS::Render::Settings::ERenderingCapability, bool) {}
	bool NullRenderDevice::GetCapability(NLS::Render::Settings::ERenderingCapability) { return false; }
	void NullRenderDevice::SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm, int32_t, uint32_t) {}
	void NullRenderDevice::SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm) {}
	void NullRenderDevice::SetStencilMask(uint32_t) {}
	void NullRenderDevice::SetStencilOperations(NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation) {}
	void NullRenderDevice::SetCullFace(NLS::Render::Settings::ECullFace) {}
	void NullRenderDevice::SetDepthWriting(bool) {}
	void NullRenderDevice::SetColorWriting(bool, bool, bool, bool) {}
	void NullRenderDevice::SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) {}
	void NullRenderDevice::BindGraphicsPipeline(const NLS::Render::RHI::GraphicsPipelineDesc&, const NLS::Render::Resources::BindingSetInstance*) {}
	std::shared_ptr<NLS::Render::RHI::IRHITexture> NullRenderDevice::CreateTextureResource(NLS::Render::RHI::TextureDimension dimension) { return std::make_shared<NullTextureResource>(dimension); }
	uint32_t NullRenderDevice::CreateTexture() { return 0; }
	void NullRenderDevice::DestroyTexture(uint32_t) {}
	void NullRenderDevice::BindTexture(NLS::Render::RHI::TextureDimension, uint32_t) {}
	void NullRenderDevice::ActivateTexture(uint32_t) {}
	void NullRenderDevice::SetupTexture(const NLS::Render::RHI::TextureDesc&, const void*) {}
	void NullRenderDevice::GenerateTextureMipmap(NLS::Render::RHI::TextureDimension) {}
	uint32_t NullRenderDevice::CreateFramebuffer() { return 0; }
	void NullRenderDevice::DestroyFramebuffer(uint32_t) {}
	void NullRenderDevice::BindFramebuffer(uint32_t) {}
	void NullRenderDevice::AttachFramebufferColorTexture(uint32_t, uint32_t, uint32_t) {}
	void NullRenderDevice::AttachFramebufferDepthStencilTexture(uint32_t, uint32_t) {}
	void NullRenderDevice::SetFramebufferDrawBufferCount(uint32_t, uint32_t) {}
	void NullRenderDevice::BlitDepth(uint32_t, uint32_t, uint32_t, uint32_t) {}
	std::shared_ptr<NLS::Render::RHI::IRHIBuffer> NullRenderDevice::CreateBufferResource(NLS::Render::RHI::BufferType type) { return std::make_shared<NullBufferResource>(type); }
	uint32_t NullRenderDevice::CreateBuffer() { return 0; }
	void NullRenderDevice::DestroyBuffer(uint32_t) {}
	void NullRenderDevice::BindBuffer(NLS::Render::RHI::BufferType, uint32_t) {}
	void NullRenderDevice::BindBufferBase(NLS::Render::RHI::BufferType, uint32_t, uint32_t) {}
	void NullRenderDevice::SetBufferData(NLS::Render::RHI::BufferType, size_t, const void*, NLS::Render::RHI::BufferUsage) {}
	void NullRenderDevice::SetBufferSubData(NLS::Render::RHI::BufferType, size_t, size_t, const void*) {}
	void* NullRenderDevice::GetUITextureHandle(uint32_t) const { return nullptr; }
	void NullRenderDevice::ReleaseUITextureHandles() {}
	bool NullRenderDevice::PrepareUIRender() { return false; }
	std::string NullRenderDevice::GetVendor() { return m_descriptor.vendor; }
	std::string NullRenderDevice::GetHardware() { return m_descriptor.hardware; }
	std::string NullRenderDevice::GetVersion() { return m_descriptor.version; }
	std::string NullRenderDevice::GetShadingLanguageVersion() { return m_descriptor.shadingLanguageVersion; }
	NLS::Render::RHI::RHIDeviceCapabilities NullRenderDevice::GetCapabilities() const { return m_descriptor.capabilities; }
	NLS::Render::RHI::NativeRenderDeviceInfo NullRenderDevice::GetNativeDeviceInfo() const
	{
		NLS::Render::RHI::NativeRenderDeviceInfo info;
		info.backend = m_descriptor.backend;
		return info;
	}
	bool NullRenderDevice::IsBackendReady() const { return m_descriptor.capabilities.backendReady; }
	bool NullRenderDevice::CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) { return false; }
	void NullRenderDevice::DestroySwapchain() {}
	void NullRenderDevice::ResizeSwapchain(uint32_t, uint32_t) {}
	void NullRenderDevice::PresentSwapchain() {}
}
