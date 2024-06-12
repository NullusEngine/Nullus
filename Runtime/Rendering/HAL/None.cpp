#include "Rendering/HAL/GraphicsAPI.h"

/**
* Although this is intended as a Null implementation, certain components of the engine rely on OpenGL,
* such as resource creation, binding, and the user interface. Consequently, GLEW must be initialized to
* support these functionalities. This implementation exclusively initializes GLEW without making any
* additional calls.
*/

namespace NLS::Rendering::HAL
{
	template<>
	std::optional<NLS::Rendering::Data::PipelineState> None::Init(bool debug)
	{
		return NLS::Rendering::Data::PipelineState{};
	}
	template<>
	void None::Clear(bool p_colorBuffer, bool p_depthBuffer, bool p_stencilBuffer)
	{}
	template<>
	void None::ReadPixels(
		uint32_t p_x,
		uint32_t p_y,
		uint32_t p_width,
		uint32_t p_height,
		Settings::EPixelDataFormat p_format,
		Settings::EPixelDataType p_type,
		void* p_data
	)
	{}
	template<>
	void None::DrawElements(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_indexCount)
	{}
	template<>
	void None::DrawElementsInstanced(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_indexCount, uint32_t p_instances)
	{}
	template<>
	void None::DrawArrays(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_vertexCount)
	{}
	template<>
	void None::DrawArraysInstanced(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_vertexCount, uint32_t p_instances)
	{}
	template<>
	void None::SetClearColor(float p_red, float p_green, float p_blue, float p_alpha)
	{}
	template<>
	void None::SetRasterizationLinesWidth(float p_width)
	{}
	template<>
	void None::SetRasterizationMode(Settings::ERasterizationMode p_rasterizationMode)
	{}
	template<>
	void None::SetCapability(Settings::ERenderingCapability p_capability, bool p_value)
	{}
	template<>
	bool None::GetCapability(Settings::ERenderingCapability p_capability)
	{
		return false;
	}
	template<>
	void None::SetStencilAlgorithm(Settings::EComparaisonAlgorithm p_algorithm, int32_t p_reference, uint32_t p_mask)
	{}
	template<>
	void None::SetDepthAlgorithm(Settings::EComparaisonAlgorithm p_algorithm)
	{}
	template<>
	void None::SetStencilMask(uint32_t p_mask)
	{}
	template<>
	void None::SetStencilOperations(Settings::EOperation p_stencilFail, Settings::EOperation p_depthFail, Settings::EOperation p_bothPass)
	{}
	template<>
	void None::SetCullFace(Settings::ECullFace p_cullFace)
	{}
	template<>
	void None::SetDepthWriting(bool p_enable)
	{}
	template<>
	void None::SetColorWriting(bool p_enableRed, bool p_enableGreen, bool p_enableBlue, bool p_enableAlpha)
	{}
	template<>
	void None::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{}
	template<>
	std::string None::GetVendor()
	{
		return "None";
	}
	template<>
	std::string None::GetHardware()
	{
		return "None";
	}
	template<>
	std::string None::GetVersion()
	{
		return "None";
	}
	template<>
	std::string None::GetShadingLanguageVersion()
	{
		return "None";
	}
}
