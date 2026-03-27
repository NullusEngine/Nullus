#pragma once

#include <memory>

#include "Rendering/RHI/IRenderDevice.h"

namespace NLS::Render::Backend
{
	class NLS_RENDER_API VulkanRenderDevice final : public NLS::Render::RHI::IRenderDevice
	{
	public:
		VulkanRenderDevice();
		~VulkanRenderDevice() override;

		std::optional<NLS::Render::Data::PipelineState> Init(const NLS::Render::Settings::DriverSettings& settings) override;

		void Clear(bool colorBuffer, bool depthBuffer, bool stencilBuffer) override;
		void ReadPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, NLS::Render::Settings::EPixelDataFormat format, NLS::Render::Settings::EPixelDataType type, void* data) override;
		void DrawElements(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount) override;
		void DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t instances) override;
		void DrawArrays(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount) override;
		void DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t instances) override;
		void SetClearColor(float red, float green, float blue, float alpha) override;
		void SetRasterizationLinesWidth(float width) override;
		void SetRasterizationMode(NLS::Render::Settings::ERasterizationMode mode) override;
		void SetCapability(NLS::Render::Settings::ERenderingCapability capability, bool value) override;
		bool GetCapability(NLS::Render::Settings::ERenderingCapability capability) override;
		void SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm, int32_t reference, uint32_t mask) override;
		void SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm) override;
		void SetStencilMask(uint32_t mask) override;
		void SetStencilOperations(NLS::Render::Settings::EOperation stencilFail, NLS::Render::Settings::EOperation depthFail, NLS::Render::Settings::EOperation bothPass) override;
		void SetCullFace(NLS::Render::Settings::ECullFace cullFace) override;
		void SetDepthWriting(bool enable) override;
		void SetColorWriting(bool enableRed, bool enableGreen, bool enableBlue, bool enableAlpha) override;
		void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
		void BindGraphicsPipeline(const NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc, const NLS::Render::Resources::BindingSetInstance* bindingSet) override;
		std::shared_ptr<NLS::Render::RHI::IRHITexture> CreateTextureResource(NLS::Render::RHI::TextureDimension dimension = NLS::Render::RHI::TextureDimension::Texture2D) override;
		uint32_t CreateTexture() override;
		void DestroyTexture(uint32_t textureId) override;
		void BindTexture(NLS::Render::RHI::TextureDimension dimension, uint32_t textureId) override;
		void ActivateTexture(uint32_t slot) override;
		void SetupTexture(const NLS::Render::RHI::TextureDesc& desc, const void* data) override;
		void GenerateTextureMipmap(NLS::Render::RHI::TextureDimension dimension) override;
		uint32_t CreateFramebuffer() override;
		void DestroyFramebuffer(uint32_t framebufferId) override;
		void BindFramebuffer(uint32_t framebufferId) override;
		void AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex) override;
		void AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId) override;
		void SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount) override;
		void BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height) override;
		std::shared_ptr<NLS::Render::RHI::IRHIBuffer> CreateBufferResource(NLS::Render::RHI::BufferType type) override;
		uint32_t CreateBuffer() override;
		void DestroyBuffer(uint32_t bufferId) override;
		void BindBuffer(NLS::Render::RHI::BufferType type, uint32_t bufferId) override;
		void BindBufferBase(NLS::Render::RHI::BufferType type, uint32_t bindingPoint, uint32_t bufferId) override;
		void SetBufferData(NLS::Render::RHI::BufferType type, size_t size, const void* data, NLS::Render::RHI::BufferUsage usage) override;
		void SetBufferSubData(NLS::Render::RHI::BufferType type, size_t offset, size_t size, const void* data) override;
		void* GetUITextureHandle(uint32_t textureId) const override;
		void ReleaseUITextureHandles() override;
		bool PrepareUIRender() override;
		std::string GetVendor() override;
		std::string GetHardware() override;
		std::string GetVersion() override;
		std::string GetShadingLanguageVersion() override;
		NLS::Render::RHI::RHIDeviceCapabilities GetCapabilities() const override;
		NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override;
		bool IsBackendReady() const override;
		bool CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc) override;
		void DestroySwapchain() override;
		void ResizeSwapchain(uint32_t width, uint32_t height) override;
		void PresentSwapchain() override;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
