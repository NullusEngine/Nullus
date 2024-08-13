#include "Rendering/HAL/GraphicsAPI.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <Debug/Logger.h>
#include <Debug/Assertion.h>
#include "Rendering/Utils/Conversions.h"

constexpr GLenum ToGLEnum(NLS::Render::Settings::ERasterizationMode value)
{
	return GL_POINT + static_cast<GLint>(value);
}

constexpr GLenum ToGLEnum(NLS::Render::Settings::EComparaisonAlgorithm value)
{
	return GL_NEVER + static_cast<GLint>(value);
}

constexpr GLenum ToGLEnum(NLS::Render::Settings::ECullFace value)
{
	switch (value)
	{
	case NLS::Render::Settings::ECullFace::FRONT: return GL_FRONT;
	case NLS::Render::Settings::ECullFace::BACK: return GL_BACK;
	case NLS::Render::Settings::ECullFace::FRONT_AND_BACK: return GL_FRONT_AND_BACK;
	}

	static_assert(true);
	return {};
}

constexpr GLenum ToGLEnum(NLS::Render::Settings::EOperation value)
{
	switch (value)
	{
	case NLS::Render::Settings::EOperation::KEEP: return GL_KEEP;
	case NLS::Render::Settings::EOperation::ZERO: return GL_ZERO;
	case NLS::Render::Settings::EOperation::REPLACE: return GL_REPLACE;
	case NLS::Render::Settings::EOperation::INCREMENT: return GL_INCR;
	case NLS::Render::Settings::EOperation::INCREMENT_WRAP: return GL_INCR_WRAP;
	case NLS::Render::Settings::EOperation::DECREMENT: return GL_DECR;
	case NLS::Render::Settings::EOperation::DECREMENT_WRAP: return GL_DECR_WRAP;
	case NLS::Render::Settings::EOperation::INVERT: return GL_INVERT;
	}

	static_assert(true);
	return {};
}

constexpr GLenum ToGLEnum(NLS::Render::Settings::ERenderingCapability value)
{
	switch (value)
	{
	case NLS::Render::Settings::ERenderingCapability::BLEND: return GL_BLEND;
	case NLS::Render::Settings::ERenderingCapability::CULL_FACE: return GL_CULL_FACE;
	case NLS::Render::Settings::ERenderingCapability::DEPTH_TEST: return GL_DEPTH_TEST;
	case NLS::Render::Settings::ERenderingCapability::DITHER: return GL_DITHER;
	case NLS::Render::Settings::ERenderingCapability::POLYGON_OFFSET_FILL: return GL_POLYGON_OFFSET_FILL;
	case NLS::Render::Settings::ERenderingCapability::SAMPLE_ALPHA_TO_COVERAGE: return GL_SAMPLE_ALPHA_TO_COVERAGE;
	case NLS::Render::Settings::ERenderingCapability::SAMPLE_COVERAGE: return GL_SAMPLE_COVERAGE;
	case NLS::Render::Settings::ERenderingCapability::SCISSOR_TEST: return GL_SCISSOR_TEST;
	case NLS::Render::Settings::ERenderingCapability::STENCIL_TEST: return GL_STENCIL_TEST;
	case NLS::Render::Settings::ERenderingCapability::MULTISAMPLE: return GL_MULTISAMPLE;
	}

	static_assert(true);
	return {};
}

constexpr GLenum ToGLEnum(NLS::Render::Settings::EPrimitiveMode value)
{
	switch (value)
	{
	case NLS::Render::Settings::EPrimitiveMode::POINTS: return GL_POINTS;
	case NLS::Render::Settings::EPrimitiveMode::LINES: return GL_LINES;
	case NLS::Render::Settings::EPrimitiveMode::LINE_LOOP: return GL_LINE_LOOP;
	case NLS::Render::Settings::EPrimitiveMode::LINE_STRIP: return GL_LINE_STRIP;
	case NLS::Render::Settings::EPrimitiveMode::TRIANGLES: return GL_TRIANGLES;
	case NLS::Render::Settings::EPrimitiveMode::TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
	case NLS::Render::Settings::EPrimitiveMode::TRIANGLE_FAN: return GL_TRIANGLE_FAN;
	case NLS::Render::Settings::EPrimitiveMode::LINES_ADJACENCY: return GL_LINES_ADJACENCY;
	case NLS::Render::Settings::EPrimitiveMode::LINE_STRIP_ADJACENCY: return GL_LINE_STRIP_ADJACENCY;
	case NLS::Render::Settings::EPrimitiveMode::PATCHES: return GL_PATCHES;
	}

	static_assert(true);
	return {};
}

void GLDebugMessageCallback(uint32_t source, uint32_t type, uint32_t id, uint32_t severity, int32_t length, const char* message, const void* userParam)
{
	// ignore non-significant error/warning codes
	if (id == 131169 || id == 131185 || id == 131218 || id == 131204) return;

	std::string output;

	output += "OpenGL Debug Message:\n";
	output += "Debug message (" + std::to_string(id) + "): " + message + "\n";

	switch (source)
	{
	case GL_DEBUG_SOURCE_API:				output += "Source: API";				break;
	case GL_DEBUG_SOURCE_WINDOW_SYSTEM:		output += "Source: Window System";		break;
	case GL_DEBUG_SOURCE_SHADER_COMPILER:	output += "Source: Shader Compiler";	break;
	case GL_DEBUG_SOURCE_THIRD_PARTY:		output += "Source: Third Party";		break;
	case GL_DEBUG_SOURCE_APPLICATION:		output += "Source: Application";		break;
	case GL_DEBUG_SOURCE_OTHER:				output += "Source: Other";				break;
	}

	output += "\n";

	switch (type)
	{
	case GL_DEBUG_TYPE_ERROR:               output += "Type: Error";				break;
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: output += "Type: Deprecated Behaviour"; break;
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:  output += "Type: Undefined Behaviour";	break;
	case GL_DEBUG_TYPE_PORTABILITY:         output += "Type: Portability";			break;
	case GL_DEBUG_TYPE_PERFORMANCE:         output += "Type: Performance";			break;
	case GL_DEBUG_TYPE_MARKER:              output += "Type: Marker";				break;
	case GL_DEBUG_TYPE_PUSH_GROUP:          output += "Type: Push Group";			break;
	case GL_DEBUG_TYPE_POP_GROUP:           output += "Type: Pop Group";			break;
	case GL_DEBUG_TYPE_OTHER:               output += "Type: Other";				break;
	}

	output += "\n";

	switch (severity)
	{
	case GL_DEBUG_SEVERITY_HIGH:			output += "Severity: High";				break;
	case GL_DEBUG_SEVERITY_MEDIUM:			output += "Severity: Medium";			break;
	case GL_DEBUG_SEVERITY_LOW:				output += "Severity: Low";				break;
	case GL_DEBUG_SEVERITY_NOTIFICATION:	output += "Severity: Notification";		break;
	}

	switch (severity)
	{
	case GL_DEBUG_SEVERITY_HIGH:			NLS_LOG_ERROR(output);	break;
	case GL_DEBUG_SEVERITY_MEDIUM:			NLS_LOG_WARNING(output);	break;
	case GL_DEBUG_SEVERITY_LOW:				NLS_LOG_INFO(output);		break;
	case GL_DEBUG_SEVERITY_NOTIFICATION:	NLS_LOG_INFO(output);		break;
	}
}

bool GetBool(uint32_t p_parameter)
{
	GLboolean result;
	glGetBooleanv(p_parameter, &result);
	return static_cast<bool>(result);
}

bool GetBool(uint32_t p_parameter, uint32_t p_index)
{
	GLboolean result;
	glGetBooleani_v(p_parameter, p_index, &result);
	return static_cast<bool>(result);
}

int GetInt(uint32_t p_parameter)
{
	GLint result;
	glGetIntegerv(p_parameter, &result);
	GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        return -1; 
    }
	return static_cast<int>(result);
}

int GetInt(uint32_t p_parameter, uint32_t p_index)
{
	GLint result;
	glGetIntegeri_v(p_parameter, p_index, &result);
	return static_cast<int>(result);
}

float GetFloat(uint32_t p_parameter)
{
	GLfloat result;
	glGetFloatv(p_parameter, &result);
	return static_cast<float>(result);
}

float GetFloat(uint32_t p_parameter, uint32_t p_index)
{
	GLfloat result;
	glGetFloati_v(p_parameter, p_index, &result);
	return static_cast<float>(result);
}

double GetDouble(uint32_t p_parameter)
{
	GLdouble result;
	glGetDoublev(p_parameter, &result);
	return static_cast<double>(result);
}

double GetDouble(uint32_t p_parameter, uint32_t p_index)
{
	GLdouble result;
	glGetDoublei_v(p_parameter, p_index, &result);
	return static_cast<double>(result);
}

int64_t GetInt64(uint32_t p_parameter)
{
	GLint64 result;
	glGetInteger64v(p_parameter, &result);
	return static_cast<int64_t>(result);
}

int64_t GetInt64(uint32_t p_parameter, uint32_t p_index)
{
	GLint64 result;
	glGetInteger64i_v(p_parameter, p_index, &result);
	return static_cast<int64_t>(result);
}

std::string GetString(uint32_t p_parameter)
{
	const GLubyte* result = glGetString(p_parameter);
	return result ? reinterpret_cast<const char*>(result) : std::string();
}

std::string GetString(uint32_t p_parameter, uint32_t p_index)
{
	const GLubyte* result = glGetStringi(p_parameter, p_index);
	return result ? reinterpret_cast<const char*>(result) : std::string();
}

template<class T>
T GetEnum(GLenum e);

template<>
NLS::Render::Settings::ERasterizationMode GetEnum(GLenum e)
{
	auto glEnumValue = GetInt(e);
	return static_cast<NLS::Render::Settings::ERasterizationMode>(glEnumValue - GL_POINT);
}

template<>
NLS::Render::Settings::EComparaisonAlgorithm GetEnum(GLenum e)
{
	auto glEnumValue = GetInt(e);
	return static_cast<NLS::Render::Settings::EComparaisonAlgorithm>(glEnumValue - GL_NEVER);
}

template<>
NLS::Render::Settings::ECullFace GetEnum(GLenum e)
{
	auto glEnumValue = GetInt(e);
	switch (glEnumValue)
	{
		case GL_FRONT: return NLS::Render::Settings::ECullFace::FRONT;
		case GL_BACK: return NLS::Render::Settings::ECullFace::BACK;
		case GL_FRONT_AND_BACK: return NLS::Render::Settings::ECullFace::FRONT_AND_BACK;
	}

	NLS_ASSERT(false, "");
	return {};
}

template<>
NLS::Render::Settings::EOperation GetEnum(GLenum e)
{
	auto glEnumValue = GetInt(e);
	switch (glEnumValue)
	{
	case GL_KEEP: return NLS::Render::Settings::EOperation::KEEP;
	case GL_ZERO: return NLS::Render::Settings::EOperation::ZERO;
	case GL_REPLACE: return NLS::Render::Settings::EOperation::REPLACE;
	case GL_INCR: return NLS::Render::Settings::EOperation::INCREMENT;
	case GL_INCR_WRAP: return NLS::Render::Settings::EOperation::INCREMENT_WRAP;
	case GL_DECR: return NLS::Render::Settings::EOperation::DECREMENT;
	case GL_DECR_WRAP: return NLS::Render::Settings::EOperation::DECREMENT_WRAP;
	case GL_INVERT: return NLS::Render::Settings::EOperation::INVERT;
	}

	NLS_ASSERT(false, "");
	return {};
}

template<>
NLS::Render::Settings::ERenderingCapability GetEnum(GLenum e)
{
	auto glEnumValue = GetInt(e);
	switch (glEnumValue)
	{
	case GL_BLEND: return NLS::Render::Settings::ERenderingCapability::BLEND;
	case GL_CULL_FACE: return NLS::Render::Settings::ERenderingCapability::CULL_FACE;
	case GL_DEPTH_TEST: return NLS::Render::Settings::ERenderingCapability::DEPTH_TEST;
	case GL_DITHER: return NLS::Render::Settings::ERenderingCapability::DITHER;
	case GL_POLYGON_OFFSET_FILL: return NLS::Render::Settings::ERenderingCapability::POLYGON_OFFSET_FILL;
	case GL_SAMPLE_ALPHA_TO_COVERAGE: return NLS::Render::Settings::ERenderingCapability::SAMPLE_ALPHA_TO_COVERAGE;
	case GL_SAMPLE_COVERAGE: return NLS::Render::Settings::ERenderingCapability::SAMPLE_COVERAGE;
	case GL_SCISSOR_TEST: return NLS::Render::Settings::ERenderingCapability::SCISSOR_TEST;
	case GL_STENCIL_TEST: return NLS::Render::Settings::ERenderingCapability::STENCIL_TEST;
	case GL_MULTISAMPLE: return NLS::Render::Settings::ERenderingCapability::MULTISAMPLE;
	}

	NLS_ASSERT(false, "");
	return {};
}

/**
* Very expensive! Call it once, and make sure you always keep track of state changes
*/
NLS::Render::Data::PipelineState RetrieveOpenGLPipelineState()
{
	using namespace NLS::Render::Settings;

	NLS::Render::Data::PipelineState pso;

	// Rasterization
	pso.rasterizationMode = GetEnum<ERasterizationMode>(GL_POLYGON_MODE);
	pso.lineWidthPow2 = NLS::Render::Utils::Conversions::FloatToPow2(GetFloat(GL_LINE_WIDTH));

	// Color write mask
	GLboolean colorWriteMask[4];
	glGetBooleanv(GL_COLOR_WRITEMASK, colorWriteMask);
	pso.colorWriting.r = colorWriteMask[0];
	pso.colorWriting.g = colorWriteMask[1];
	pso.colorWriting.b = colorWriteMask[2];
	pso.colorWriting.a = colorWriteMask[3];

	// Capability
	pso.depthWriting = GetBool(GL_DEPTH_WRITEMASK);
	pso.blending = GetBool(GL_BLEND);
	pso.culling = GetBool(GL_CULL_FACE);
	pso.dither = GetBool(GL_DITHER);
	pso.polygonOffsetFill = GetBool(GL_POLYGON_OFFSET_FILL);
	pso.sampleAlphaToCoverage = GetBool(GL_SAMPLE_ALPHA_TO_COVERAGE);
	pso.depthTest = GetBool(GL_DEPTH_TEST);
	pso.scissorTest = GetBool(GL_SCISSOR_TEST);
	pso.stencilTest = GetBool(GL_STENCIL_TEST);
	pso.multisample = GetBool(GL_MULTISAMPLE);

	// Stencil
	pso.stencilFuncOp = GetEnum<EComparaisonAlgorithm>(GL_STENCIL_FUNC);
	pso.stencilFuncRef = GetInt(GL_STENCIL_REF);
	pso.stencilFuncMask = static_cast<uint32_t>(GetInt(GL_STENCIL_VALUE_MASK));

	pso.stencilWriteMask = static_cast<uint32_t>(GetInt(GL_STENCIL_WRITEMASK));

	pso.stencilOpFail = GetEnum<EOperation>(GL_STENCIL_FAIL);
	pso.depthOpFail = GetEnum<EOperation>(GL_STENCIL_PASS_DEPTH_FAIL);
	pso.bothOpFail = GetEnum<EOperation>(GL_STENCIL_PASS_DEPTH_PASS);

	// Depth
	pso.depthFunc = GetEnum<EComparaisonAlgorithm>(GL_DEPTH_FUNC);

	// Culling
	pso.cullFace = GetEnum<ECullFace>(GL_CULL_FACE_MODE);

	return pso;
}

namespace NLS::Render::HAL
{
	template<>
	std::optional<Data::PipelineState> OpenGL::Init(bool debug)
	{
		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			std::string message = "Failed to initialize GLAD";
			NLS_LOG_ERROR(message);
			return std::nullopt;
		}

		if (debug)
		{
			glEnable(GL_DEBUG_OUTPUT);
			glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
			glDebugMessageCallback(GLDebugMessageCallback, nullptr);
		}

		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glCullFace(GL_BACK);

		return RetrieveOpenGLPipelineState();
	}
	template<>
	void OpenGL::Clear(bool p_colorBuffer, bool p_depthBuffer, bool p_stencilBuffer)
	{
		GLbitfield clearMask = 0;

		if (p_colorBuffer) clearMask |= GL_COLOR_BUFFER_BIT;
		if (p_depthBuffer) clearMask |= GL_DEPTH_BUFFER_BIT;
		if (p_stencilBuffer) clearMask |= GL_STENCIL_BUFFER_BIT;

		if (clearMask != 0)
		{
			glClear(clearMask);
		}
	}
	template<>
	void OpenGL::ReadPixels(
		uint32_t p_x,
		uint32_t p_y,
		uint32_t p_width,
		uint32_t p_height,
		Settings::EPixelDataFormat p_format,
		Settings::EPixelDataType p_type,
		void* p_data
	)
	{
		glReadPixels(p_x, p_y, p_width, p_height, static_cast<GLenum>(p_format), static_cast<GLenum>(p_type), p_data);
	}
	template<>
	void OpenGL::DrawElements(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_indexCount)
	{
		glDrawElements(ToGLEnum(p_primitiveMode), p_indexCount, GL_UNSIGNED_INT, nullptr);
	}
	template<>
	void OpenGL::DrawElementsInstanced(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_indexCount, uint32_t p_instances)
	{
		glDrawElementsInstanced(ToGLEnum(p_primitiveMode), p_indexCount, GL_UNSIGNED_INT, nullptr, p_instances);
	}
	template<>
	void OpenGL::DrawArrays(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_vertexCount)
	{
		glDrawArrays(ToGLEnum(p_primitiveMode), 0, p_vertexCount);
	}
	template<>
	void OpenGL::DrawArraysInstanced(Settings::EPrimitiveMode p_primitiveMode, uint32_t p_vertexCount, uint32_t p_instances)
	{
		glDrawArraysInstanced(ToGLEnum(p_primitiveMode), 0, p_vertexCount, p_instances);
	}
	template<>
	void OpenGL::SetClearColor(float p_red, float p_green, float p_blue, float p_alpha)
	{
		glClearColor(p_red, p_green, p_blue, p_alpha);
	}
	template<>
	void OpenGL::SetRasterizationLinesWidth(float p_width)
	{
		glLineWidth(p_width);
	}
	template<>
	void OpenGL::SetRasterizationMode(Settings::ERasterizationMode p_rasterizationMode)
	{
		glPolygonMode(GL_FRONT_AND_BACK, ToGLEnum(p_rasterizationMode));
	}
	template<>
	void OpenGL::SetCapability(Settings::ERenderingCapability p_capability, bool p_value)
	{
		(p_value ? glEnable : glDisable)(ToGLEnum(p_capability));
	}
	template<>
	bool OpenGL::GetCapability(Settings::ERenderingCapability p_capability)
	{
		return glIsEnabled(ToGLEnum(p_capability));
	}
	template<>
	void OpenGL::SetStencilAlgorithm(Settings::EComparaisonAlgorithm p_algorithm, int32_t p_reference, uint32_t p_mask)
	{
		glStencilFunc(ToGLEnum(p_algorithm), p_reference, p_mask);
	}
	template<>
	void OpenGL::SetDepthAlgorithm(Settings::EComparaisonAlgorithm p_algorithm)
	{
		glDepthFunc(ToGLEnum(p_algorithm));
	}
	template<>
	void OpenGL::SetStencilMask(uint32_t p_mask)
	{
		glStencilMask(p_mask);
	}
	template<>
	void OpenGL::SetStencilOperations(Settings::EOperation p_stencilFail, Settings::EOperation p_depthFail, Settings::EOperation p_bothPass)
	{
		glStencilOp(ToGLEnum(p_stencilFail), ToGLEnum(p_depthFail), ToGLEnum(p_bothPass));
	}
	template<>
	void OpenGL::SetCullFace(Settings::ECullFace p_cullFace)
	{
		glCullFace(ToGLEnum(p_cullFace));
	}
	template<>
	void OpenGL::SetDepthWriting(bool p_enable)
	{
		glDepthMask(p_enable);
	}
	template<>
	void OpenGL::SetColorWriting(bool p_enableRed, bool p_enableGreen, bool p_enableBlue, bool p_enableAlpha)
	{
		glColorMask(p_enableRed, p_enableGreen, p_enableBlue, p_enableAlpha);
	}
	template<>
	void OpenGL::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		glViewport(x, y, width, height);
	}
	template<>
	std::string OpenGL::GetVendor()
	{
		return GetString(GL_VENDOR);
	}
	template<>
	std::string OpenGL::GetHardware()
	{
		return GetString(GL_RENDERER);
	}
	template<>
	std::string OpenGL::GetVersion()
	{
		return GetString(GL_VERSION);
	}
	template<>
	std::string OpenGL::GetShadingLanguageVersion()
	{
		return GetString(GL_SHADING_LANGUAGE_VERSION);
	}
}
