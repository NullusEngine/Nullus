#pragma once

#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/ERenderingCapability.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/Settings/ERasterizationMode.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/EOperation.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/ECullingOptions.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Data/PipelineState.h"

namespace NLS::Rendering::HAL
{
	template<Settings::EGraphicsBackend Backend>
	class GraphicsAPI final
	{
	public:
		std::optional<NLS::Rendering::Data::PipelineState> Init(bool debug);

		void Clear(bool p_colorBuffer, bool p_depthBuffer, bool p_stencilBuffer);
		void ReadPixels(
			uint32_t p_x,
			uint32_t p_y,
			uint32_t p_width,
			uint32_t p_height,
			NLS::Rendering::Settings::EPixelDataFormat p_format,
			NLS::Rendering::Settings::EPixelDataType p_type,
			void* p_data
		);

		void DrawElements(NLS::Rendering::Settings::EPrimitiveMode p_primitiveMode, uint32_t p_indexCount);
		void DrawElementsInstanced(NLS::Rendering::Settings::EPrimitiveMode p_primitiveMode, uint32_t p_indexCount, uint32_t p_instances);
		void DrawArrays(NLS::Rendering::Settings::EPrimitiveMode p_primitiveMode, uint32_t p_vertexCount);
		void DrawArraysInstanced(NLS::Rendering::Settings::EPrimitiveMode p_primitiveMode, uint32_t p_vertexCount, uint32_t p_instances);

		void SetClearColor(float p_red, float p_green, float p_blue, float p_alpha);
		void SetRasterizationLinesWidth(float p_width);
		void SetRasterizationMode(NLS::Rendering::Settings::ERasterizationMode p_rasterizationMode);
		void SetCapability(NLS::Rendering::Settings::ERenderingCapability p_capability, bool p_value);
		bool GetCapability(NLS::Rendering::Settings::ERenderingCapability p_capability);
		void SetStencilAlgorithm(NLS::Rendering::Settings::EComparaisonAlgorithm p_algorithm, int32_t p_reference, uint32_t p_mask);
		void SetDepthAlgorithm(NLS::Rendering::Settings::EComparaisonAlgorithm p_algorithm);
		void SetStencilMask(uint32_t p_mask);

		void SetStencilOperations(
			NLS::Rendering::Settings::EOperation p_stencilFail,
			NLS::Rendering::Settings::EOperation p_depthFail,
			NLS::Rendering::Settings::EOperation p_bothPass
		);

		void SetCullFace(NLS::Rendering::Settings::ECullFace p_cullFace);
		void SetDepthWriting(bool p_enable);
		void SetColorWriting(bool p_enableRed, bool p_enableGreen, bool p_enableBlue, bool p_enableAlpha);
		void SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

		std::string GetVendor();
		std::string GetHardware();
		std::string GetVersion();
		std::string GetShadingLanguageVersion();
	};

	using None = NLS::Rendering::HAL::GraphicsAPI<NLS::Rendering::Settings::EGraphicsBackend::NONE>;
	using OpenGL = NLS::Rendering::HAL::GraphicsAPI<NLS::Rendering::Settings::EGraphicsBackend::OPENGL>;
	using Vulkan = NLS::Rendering::HAL::GraphicsAPI<NLS::Rendering::Settings::EGraphicsBackend::VULKAN>;
}
