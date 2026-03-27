#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/IRHIResource.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/ERenderingCapability.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/Settings/ERasterizationMode.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/EOperation.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/RHI/GraphicsPipelineDesc.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Resources
{
	class BindingSetInstance;
}

namespace NLS::Render::RHI
{
	class NLS_RENDER_API IRenderDevice
	{
	public:
		virtual ~IRenderDevice() = default;

		virtual std::optional<NLS::Render::Data::PipelineState> Init(const NLS::Render::Settings::DriverSettings& settings) = 0;

		virtual void Clear(bool colorBuffer, bool depthBuffer, bool stencilBuffer) = 0;
		virtual void ReadPixels(
			uint32_t x,
			uint32_t y,
			uint32_t width,
			uint32_t height,
			NLS::Render::Settings::EPixelDataFormat format,
			NLS::Render::Settings::EPixelDataType type,
			void* data) = 0;

		virtual void DrawElements(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount) = 0;
		virtual void DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t instances) = 0;
		virtual void DrawArrays(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount) = 0;
		virtual void DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t instances) = 0;

		virtual void SetClearColor(float red, float green, float blue, float alpha) = 0;
		virtual void SetRasterizationLinesWidth(float width) = 0;
		virtual void SetRasterizationMode(NLS::Render::Settings::ERasterizationMode mode) = 0;
		virtual void SetCapability(NLS::Render::Settings::ERenderingCapability capability, bool value) = 0;
		virtual bool GetCapability(NLS::Render::Settings::ERenderingCapability capability) = 0;
		virtual void SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm, int32_t reference, uint32_t mask) = 0;
		virtual void SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm) = 0;
		virtual void SetStencilMask(uint32_t mask) = 0;
		virtual void SetStencilOperations(
			NLS::Render::Settings::EOperation stencilFail,
			NLS::Render::Settings::EOperation depthFail,
			NLS::Render::Settings::EOperation bothPass) = 0;
		virtual void SetCullFace(NLS::Render::Settings::ECullFace cullFace) = 0;
		virtual void SetDepthWriting(bool enable) = 0;
		virtual void SetColorWriting(bool enableRed, bool enableGreen, bool enableBlue, bool enableAlpha) = 0;
		virtual void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) = 0;
		virtual void BindGraphicsPipeline(const GraphicsPipelineDesc& pipelineDesc, const NLS::Render::Resources::BindingSetInstance* bindingSet) = 0;

		virtual std::shared_ptr<IRHITexture> CreateTextureResource(TextureDimension dimension = TextureDimension::Texture2D) = 0;
		virtual uint32_t CreateTexture() = 0;
		virtual void DestroyTexture(uint32_t textureId) = 0;
		virtual void BindTexture(TextureDimension dimension, uint32_t textureId) = 0;
		virtual void ActivateTexture(uint32_t slot) = 0;
		virtual void SetupTexture(const TextureDesc& desc, const void* data) = 0;
		virtual void GenerateTextureMipmap(TextureDimension dimension) = 0;

		virtual uint32_t CreateFramebuffer() = 0;
		virtual uint32_t CreateFramebuffer(const FramebufferDesc& desc)
		{
			const auto framebufferId = CreateFramebuffer();
			ConfigureFramebuffer(framebufferId, desc);
			return framebufferId;
		}
		virtual void DestroyFramebuffer(uint32_t framebufferId) = 0;
		virtual void BindFramebuffer(uint32_t framebufferId) = 0;
		virtual void ConfigureFramebuffer(uint32_t framebufferId, const FramebufferDesc& desc)
		{
			for (uint32_t attachmentIndex = 0; attachmentIndex < desc.colorAttachments.size(); ++attachmentIndex)
			{
				AttachFramebufferColorTexture(
					framebufferId,
					desc.colorAttachments[attachmentIndex].textureId,
					attachmentIndex);
			}

			if (desc.depthStencilTextureId != 0)
				AttachFramebufferDepthStencilTexture(framebufferId, desc.depthStencilTextureId);

			SetFramebufferDrawBufferCount(
				framebufferId,
				desc.drawBufferCount != 0 ? desc.drawBufferCount : static_cast<uint32_t>(desc.colorAttachments.size()));
		}
		virtual void AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex) = 0;
		virtual void AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId) = 0;
		virtual void SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount) = 0;
		virtual void BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height) = 0;

		virtual std::shared_ptr<IRHIBuffer> CreateBufferResource(BufferType type) = 0;
		virtual uint32_t CreateBuffer() = 0;
		virtual void DestroyBuffer(uint32_t bufferId) = 0;
		virtual void BindBuffer(BufferType type, uint32_t bufferId) = 0;
		virtual void BindBufferBase(BufferType type, uint32_t bindingPoint, uint32_t bufferId) = 0;
		virtual void SetBufferData(BufferType type, size_t size, const void* data, BufferUsage usage) = 0;
		virtual void SetBufferSubData(BufferType type, size_t offset, size_t size, const void* data) = 0;
		virtual void* GetUITextureHandle(uint32_t textureId) const = 0;
		virtual void ReleaseUITextureHandles() = 0;
		virtual bool PrepareUIRender() = 0;

		virtual std::string GetVendor() = 0;
		virtual std::string GetHardware() = 0;
		virtual std::string GetVersion() = 0;
		virtual std::string GetShadingLanguageVersion() = 0;
		virtual RHIDeviceCapabilities GetCapabilities() const = 0;
		virtual NativeRenderDeviceInfo GetNativeDeviceInfo() const = 0;
		virtual bool IsBackendReady() const = 0;
		virtual bool CreateSwapchain(const SwapchainDesc& desc) = 0;
		virtual void DestroySwapchain() = 0;
		virtual void ResizeSwapchain(uint32_t width, uint32_t height) = 0;
		virtual void PresentSwapchain() = 0;
	};

}
