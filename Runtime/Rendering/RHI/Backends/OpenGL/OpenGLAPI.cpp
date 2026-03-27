#include "Rendering/RHI/Backends/OpenGL/OpenGLAPI.h"

#include <cstring>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "Debug/Assertion.h"
#include "Debug/Logger.h"
#include "Rendering/Utils/Conversions.h"

namespace
{
	constexpr GLenum ToGLEnum(NLS::Render::Settings::ERasterizationMode value) { return GL_POINT + static_cast<GLint>(value); }
	constexpr GLenum ToGLEnum(NLS::Render::Settings::EComparaisonAlgorithm value) { return GL_NEVER + static_cast<GLint>(value); }

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

	constexpr GLenum ToGLEnum(NLS::Render::Settings::EPixelDataFormat value)
	{
		switch (value)
		{
		case NLS::Render::Settings::EPixelDataFormat::RED: return GL_RED;
		case NLS::Render::Settings::EPixelDataFormat::GREEN: return GL_GREEN;
		case NLS::Render::Settings::EPixelDataFormat::BLUE: return GL_BLUE;
		case NLS::Render::Settings::EPixelDataFormat::ALPHA: return GL_ALPHA;
		case NLS::Render::Settings::EPixelDataFormat::RGB: return GL_RGB;
		case NLS::Render::Settings::EPixelDataFormat::BGR: return GL_BGR;
		case NLS::Render::Settings::EPixelDataFormat::RGBA: return GL_RGBA;
		case NLS::Render::Settings::EPixelDataFormat::BGRA: return GL_BGRA;
		case NLS::Render::Settings::EPixelDataFormat::DEPTH_COMPONENT: return GL_DEPTH_COMPONENT;
		case NLS::Render::Settings::EPixelDataFormat::STENCIL_INDEX: return GL_STENCIL_INDEX;
		default: return GL_RGBA;
		}
	}

	constexpr GLenum ToGLEnum(NLS::Render::Settings::EPixelDataType value)
	{
		switch (value)
		{
		case NLS::Render::Settings::EPixelDataType::BYTE: return GL_BYTE;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE: return GL_UNSIGNED_BYTE;
		case NLS::Render::Settings::EPixelDataType::SHORT: return GL_SHORT;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_SHORT: return GL_UNSIGNED_SHORT;
		case NLS::Render::Settings::EPixelDataType::INT: return GL_INT;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_INT: return GL_UNSIGNED_INT;
		case NLS::Render::Settings::EPixelDataType::FLOAT: return GL_FLOAT;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE_3_3_2: return GL_UNSIGNED_BYTE_3_3_2;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE_2_3_3_REV: return GL_UNSIGNED_BYTE_2_3_3_REV;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_SHORT_5_6_5: return GL_UNSIGNED_SHORT_5_6_5;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_SHORT_5_6_5_REV: return GL_UNSIGNED_SHORT_5_6_5_REV;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_SHORT_4_4_4_4: return GL_UNSIGNED_SHORT_4_4_4_4;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_SHORT_4_4_4_4_REV: return GL_UNSIGNED_SHORT_4_4_4_4_REV;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_SHORT_5_5_5_1: return GL_UNSIGNED_SHORT_5_5_5_1;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_SHORT_1_5_5_5_REV: return GL_UNSIGNED_SHORT_1_5_5_5_REV;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_INT_8_8_8_8: return GL_UNSIGNED_INT_8_8_8_8;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_INT_8_8_8_8_REV: return GL_UNSIGNED_INT_8_8_8_8_REV;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_INT_10_10_10_2: return GL_UNSIGNED_INT_10_10_10_2;
		case NLS::Render::Settings::EPixelDataType::UNSIGNED_INT_2_10_10_10_REV: return GL_UNSIGNED_INT_2_10_10_10_REV;
		default: return GL_UNSIGNED_BYTE;
		}
	}

	void GLDebugMessageCallback(uint32_t, uint32_t, uint32_t id, uint32_t severity, int32_t, const char* message, const void*)
	{
		if (id == 131169 || id == 131185 || id == 131218 || id == 131204)
			return;
		if (message != nullptr && std::strstr(message, "<texture> is not the name of an existing texture.") != nullptr)
			return;

		std::string output = "OpenGL Debug Message:\nDebug message (" + std::to_string(id) + "): ";
		output += message != nullptr ? message : "";
		output += "\n";

		switch (severity)
		{
		case GL_DEBUG_SEVERITY_HIGH: NLS_LOG_ERROR(output); break;
		case GL_DEBUG_SEVERITY_MEDIUM: NLS_LOG_WARNING(output); break;
		default: NLS_LOG_INFO(output); break;
		}
	}

	bool GetBool(uint32_t parameter)
	{
		GLboolean result;
		glGetBooleanv(parameter, &result);
		return static_cast<bool>(result);
	}

	int GetInt(uint32_t parameter)
	{
		GLint result;
		glGetIntegerv(parameter, &result);
		return static_cast<int>(result);
	}

	float GetFloat(uint32_t parameter)
	{
		GLfloat result;
		glGetFloatv(parameter, &result);
		return static_cast<float>(result);
	}

	std::string GetString(uint32_t parameter)
	{
		const auto* result = glGetString(parameter);
		return result != nullptr ? reinterpret_cast<const char*>(result) : std::string{};
	}

	template<class T>
	T GetEnum(GLenum e);

	template<>
	NLS::Render::Settings::ERasterizationMode GetEnum(GLenum e) { return static_cast<NLS::Render::Settings::ERasterizationMode>(GetInt(e) - GL_POINT); }

	template<>
	NLS::Render::Settings::EComparaisonAlgorithm GetEnum(GLenum e) { return static_cast<NLS::Render::Settings::EComparaisonAlgorithm>(GetInt(e) - GL_NEVER); }

	template<>
	NLS::Render::Settings::ECullFace GetEnum(GLenum e)
	{
		switch (GetInt(e))
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
		switch (GetInt(e))
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

	NLS::Render::Data::PipelineState RetrieveOpenGLPipelineState()
	{
		using namespace NLS::Render::Settings;

		NLS::Render::Data::PipelineState pso;
		pso.rasterizationMode = GetEnum<ERasterizationMode>(GL_POLYGON_MODE);
		pso.lineWidthPow2 = NLS::Render::Utils::Conversions::FloatToPow2(GetFloat(GL_LINE_WIDTH));

		GLboolean colorWriteMask[4];
		glGetBooleanv(GL_COLOR_WRITEMASK, colorWriteMask);
		pso.colorWriting.r = colorWriteMask[0];
		pso.colorWriting.g = colorWriteMask[1];
		pso.colorWriting.b = colorWriteMask[2];
		pso.colorWriting.a = colorWriteMask[3];

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
		pso.stencilFuncOp = GetEnum<EComparaisonAlgorithm>(GL_STENCIL_FUNC);
		pso.stencilFuncRef = GetInt(GL_STENCIL_REF);
		pso.stencilFuncMask = static_cast<uint32_t>(GetInt(GL_STENCIL_VALUE_MASK));
		pso.stencilWriteMask = static_cast<uint32_t>(GetInt(GL_STENCIL_WRITEMASK));
		pso.stencilOpFail = GetEnum<EOperation>(GL_STENCIL_FAIL);
		pso.depthOpFail = GetEnum<EOperation>(GL_STENCIL_PASS_DEPTH_FAIL);
		pso.bothOpFail = GetEnum<EOperation>(GL_STENCIL_PASS_DEPTH_PASS);
		pso.depthFunc = GetEnum<EComparaisonAlgorithm>(GL_DEPTH_FUNC);
		pso.cullFace = GetEnum<ECullFace>(GL_CULL_FACE_MODE);
		return pso;
	}
}

namespace NLS::Render::Backend
{
	std::optional<NLS::Render::Data::PipelineState> OpenGLAPI::Init(bool debug)
	{
		if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		{
			NLS_LOG_ERROR("Failed to initialize GLAD");
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

	void OpenGLAPI::Clear(bool colorBuffer, bool depthBuffer, bool stencilBuffer)
	{
		GLbitfield clearMask = 0;
		if (colorBuffer) clearMask |= GL_COLOR_BUFFER_BIT;
		if (depthBuffer) clearMask |= GL_DEPTH_BUFFER_BIT;
		if (stencilBuffer) clearMask |= GL_STENCIL_BUFFER_BIT;
		if (clearMask != 0) glClear(clearMask);
	}

	void OpenGLAPI::ReadPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, NLS::Render::Settings::EPixelDataFormat format, NLS::Render::Settings::EPixelDataType type, void* data) { glReadPixels(x, y, width, height, ToGLEnum(format), ToGLEnum(type), data); }
	void OpenGLAPI::DrawElements(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount) { glDrawElements(ToGLEnum(primitiveMode), indexCount, GL_UNSIGNED_INT, nullptr); }
	void OpenGLAPI::DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t instances) { glDrawElementsInstanced(ToGLEnum(primitiveMode), indexCount, GL_UNSIGNED_INT, nullptr, instances); }
	void OpenGLAPI::DrawArrays(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount) { glDrawArrays(ToGLEnum(primitiveMode), 0, vertexCount); }
	void OpenGLAPI::DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t instances) { glDrawArraysInstanced(ToGLEnum(primitiveMode), 0, vertexCount, instances); }
	void OpenGLAPI::SetClearColor(float red, float green, float blue, float alpha) { glClearColor(red, green, blue, alpha); }
	void OpenGLAPI::SetRasterizationLinesWidth(float width) { glLineWidth(width); }
	void OpenGLAPI::SetRasterizationMode(NLS::Render::Settings::ERasterizationMode rasterizationMode) { glPolygonMode(GL_FRONT_AND_BACK, ToGLEnum(rasterizationMode)); }
	void OpenGLAPI::SetCapability(NLS::Render::Settings::ERenderingCapability capability, bool value) { (value ? glEnable : glDisable)(ToGLEnum(capability)); }
	bool OpenGLAPI::GetCapability(NLS::Render::Settings::ERenderingCapability capability) { return glIsEnabled(ToGLEnum(capability)); }
	void OpenGLAPI::SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm, int32_t reference, uint32_t mask) { glStencilFunc(ToGLEnum(algorithm), reference, mask); }
	void OpenGLAPI::SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm) { glDepthFunc(ToGLEnum(algorithm)); }
	void OpenGLAPI::SetStencilMask(uint32_t mask) { glStencilMask(mask); }
	void OpenGLAPI::SetStencilOperations(NLS::Render::Settings::EOperation stencilFail, NLS::Render::Settings::EOperation depthFail, NLS::Render::Settings::EOperation bothPass) { glStencilOp(ToGLEnum(stencilFail), ToGLEnum(depthFail), ToGLEnum(bothPass)); }
	void OpenGLAPI::SetCullFace(NLS::Render::Settings::ECullFace cullFace) { glCullFace(ToGLEnum(cullFace)); }
	void OpenGLAPI::SetDepthWriting(bool enable) { glDepthMask(enable); }
	void OpenGLAPI::SetColorWriting(bool enableRed, bool enableGreen, bool enableBlue, bool enableAlpha) { glColorMask(enableRed, enableGreen, enableBlue, enableAlpha); }
	void OpenGLAPI::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) { glViewport(x, y, width, height); }
	uint32_t OpenGLAPI::CreateTexture() { GLuint id = 0; glGenTextures(1, &id); return id; }
	void OpenGLAPI::DestroyTexture(uint32_t textureId) { const GLuint id = textureId; glDeleteTextures(1, &id); }
	void OpenGLAPI::BindTexture(uint32_t target, uint32_t textureId) { glBindTexture(static_cast<GLenum>(target), textureId); }
	void OpenGLAPI::ActivateTexture(uint32_t slot) { glActiveTexture(GL_TEXTURE0 + slot); }
	void OpenGLAPI::SetTextureImage2D(uint32_t target, int32_t level, int32_t internalFormat, uint32_t width, uint32_t height, uint32_t format, uint32_t type, const void* data) { glTexImage2D(static_cast<GLenum>(target), level, internalFormat, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, static_cast<GLenum>(format), static_cast<GLenum>(type), data); }
	void OpenGLAPI::SetTextureParameteri(uint32_t target, uint32_t parameter, int32_t value) { glTexParameteri(static_cast<GLenum>(target), static_cast<GLenum>(parameter), value); }
	void OpenGLAPI::GenerateTextureMipmap(uint32_t target) { glGenerateMipmap(static_cast<GLenum>(target)); }
	uint32_t OpenGLAPI::CreateRenderbuffer() { GLuint id = 0; glGenRenderbuffers(1, &id); return id; }
	void OpenGLAPI::DestroyRenderbuffer(uint32_t renderbufferId) { const GLuint id = renderbufferId; glDeleteRenderbuffers(1, &id); }
	void OpenGLAPI::BindRenderbuffer(uint32_t renderbufferId) { glBindRenderbuffer(GL_RENDERBUFFER, renderbufferId); }
	void OpenGLAPI::SetRenderbufferStorage(uint32_t internalFormat, uint32_t width, uint32_t height) { glRenderbufferStorage(GL_RENDERBUFFER, static_cast<GLenum>(internalFormat), static_cast<GLsizei>(width), static_cast<GLsizei>(height)); }
	uint32_t OpenGLAPI::CreateFramebuffer() { GLuint id = 0; glGenFramebuffers(1, &id); return id; }
	void OpenGLAPI::DestroyFramebuffer(uint32_t framebufferId) { const GLuint id = framebufferId; glDeleteFramebuffers(1, &id); }
	void OpenGLAPI::BindFramebuffer(uint32_t framebufferId) { glBindFramebuffer(GL_FRAMEBUFFER, framebufferId); }
	void OpenGLAPI::AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex) { glBindFramebuffer(GL_FRAMEBUFFER, framebufferId); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attachmentIndex, GL_TEXTURE_2D, textureId, 0); }
	void OpenGLAPI::AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId) { glBindFramebuffer(GL_FRAMEBUFFER, framebufferId); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, textureId, 0); }
	void OpenGLAPI::SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount) { glBindFramebuffer(GL_FRAMEBUFFER, framebufferId); std::vector<GLenum> attachments; attachments.reserve(colorAttachmentCount); for (uint32_t i = 0; i < colorAttachmentCount; ++i) attachments.push_back(GL_COLOR_ATTACHMENT0 + i); glDrawBuffers(static_cast<GLsizei>(attachments.size()), attachments.data()); }
	void OpenGLAPI::BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height) { glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebufferId); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destinationFramebufferId); glBlitFramebuffer(0, 0, static_cast<GLint>(width), static_cast<GLint>(height), 0, 0, static_cast<GLint>(width), static_cast<GLint>(height), GL_DEPTH_BUFFER_BIT, GL_NEAREST); glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); }
	uint32_t OpenGLAPI::CreateBuffer() { GLuint id = 0; glGenBuffers(1, &id); return id; }
	void OpenGLAPI::DestroyBuffer(uint32_t bufferId) { const GLuint id = bufferId; glDeleteBuffers(1, &id); }
	void OpenGLAPI::BindBuffer(uint32_t target, uint32_t bufferId) { glBindBuffer(static_cast<GLenum>(target), bufferId); }
	void OpenGLAPI::BindBufferBase(uint32_t target, uint32_t bindingPoint, uint32_t bufferId) { glBindBufferBase(static_cast<GLenum>(target), bindingPoint, bufferId); }
	void OpenGLAPI::SetBufferData(uint32_t target, size_t size, const void* data, uint32_t usage) { glBufferData(static_cast<GLenum>(target), static_cast<GLsizeiptr>(size), data, static_cast<GLenum>(usage)); }
	void OpenGLAPI::SetBufferSubData(uint32_t target, size_t offset, size_t size, const void* data) { glBufferSubData(static_cast<GLenum>(target), static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data); }
	void OpenGLAPI::SetUniformBlockBinding(uint32_t programId, uint32_t uniformBlockLocation, uint32_t bindingPoint) { glUniformBlockBinding(programId, uniformBlockLocation, bindingPoint); }
	uint32_t OpenGLAPI::GetUniformBlockIndex(uint32_t programId, const std::string& name) { return glGetUniformBlockIndex(programId, name.c_str()); }
	std::string OpenGLAPI::GetVendor() { return GetString(GL_VENDOR); }
	std::string OpenGLAPI::GetHardware() { return GetString(GL_RENDERER); }
	std::string OpenGLAPI::GetVersion() { return GetString(GL_VERSION); }
	std::string OpenGLAPI::GetShadingLanguageVersion() { return GetString(GL_SHADING_LANGUAGE_VERSION); }
}
