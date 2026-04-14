#pragma once

#include <optional>
#include <string>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/EOperation.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/Settings/ERasterizationMode.h"
#include "Rendering/Settings/ERenderingCapability.h"

namespace NLS::Render::Backend
{
	class OpenGLAPI final
	{
	public:
		std::optional<NLS::Render::Data::PipelineState> Init(bool debug);

		void Clear(bool colorBuffer, bool depthBuffer, bool stencilBuffer);
		void ReadPixels(
			uint32_t x,
			uint32_t y,
			uint32_t width,
			uint32_t height,
			NLS::Render::Settings::EPixelDataFormat format,
			NLS::Render::Settings::EPixelDataType type,
			void* data);

		void DrawElements(
			NLS::Render::Settings::EPrimitiveMode primitiveMode,
			uint32_t indexCount,
			NLS::Render::RHI::IndexType indexType);
		void DrawElementsInstanced(
			NLS::Render::Settings::EPrimitiveMode primitiveMode,
			uint32_t indexCount,
			uint32_t instances,
			NLS::Render::RHI::IndexType indexType);
		void DrawArrays(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount);
		void DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t instances);

		void SetClearColor(float red, float green, float blue, float alpha);
		void SetRasterizationLinesWidth(float width);
		void SetRasterizationMode(NLS::Render::Settings::ERasterizationMode rasterizationMode);
		void SetCapability(NLS::Render::Settings::ERenderingCapability capability, bool value);
		bool GetCapability(NLS::Render::Settings::ERenderingCapability capability);
		void SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm, int32_t reference, uint32_t mask);
		void SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm);
		void SetStencilMask(uint32_t mask);
		void SetStencilOperations(
			NLS::Render::Settings::EOperation stencilFail,
			NLS::Render::Settings::EOperation depthFail,
			NLS::Render::Settings::EOperation bothPass);
		void SetCullFace(NLS::Render::Settings::ECullFace cullFace);
		void SetDepthWriting(bool enable);
		void SetColorWriting(bool enableRed, bool enableGreen, bool enableBlue, bool enableAlpha);
		void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

		uint32_t CreateTexture();
		void DestroyTexture(uint32_t textureId);
		void BindTexture(uint32_t target, uint32_t textureId);
		void ActivateTexture(uint32_t slot);
		void SetTextureImage2D(uint32_t target, int32_t level, int32_t internalFormat, uint32_t width, uint32_t height, uint32_t format, uint32_t type, const void* data);
		void SetTextureParameteri(uint32_t target, uint32_t parameter, int32_t value);
		void GenerateTextureMipmap(uint32_t target);

		uint32_t CreateRenderbuffer();
		void DestroyRenderbuffer(uint32_t renderbufferId);
		void BindRenderbuffer(uint32_t renderbufferId);
		void SetRenderbufferStorage(uint32_t internalFormat, uint32_t width, uint32_t height);

		uint32_t CreateFramebuffer();
		void DestroyFramebuffer(uint32_t framebufferId);
		void BindFramebuffer(uint32_t framebufferId);
		void AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex);
		void AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId);
		void SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount);
		void BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height);

		uint32_t CreateBuffer();
		void DestroyBuffer(uint32_t bufferId);
		void BindBuffer(uint32_t target, uint32_t bufferId);
		void BindBufferBase(uint32_t target, uint32_t bindingPoint, uint32_t bufferId);
		void SetBufferData(uint32_t target, size_t size, const void* data, uint32_t usage);
		void SetBufferSubData(uint32_t target, size_t offset, size_t size, const void* data);
		void SetUniformBlockBinding(uint32_t programId, uint32_t uniformBlockLocation, uint32_t bindingPoint);
		uint32_t GetUniformBlockIndex(uint32_t programId, const std::string& name);

		std::string GetVendor();
		std::string GetHardware();
		std::string GetVersion();
		std::string GetShadingLanguageVersion();
	};
}
