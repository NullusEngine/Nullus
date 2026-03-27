#include "Rendering/RHI/Backends/OpenGL/OpenGLRenderDevice.h"

#include <functional>
#include <memory>
#include <sstream>
#include <string_view>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "Debug/Logger.h"
#include "Rendering/RHI/Backends/OpenGL/OpenGLShaderProgramAPI.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/Resources/BindingSetInstance.h"

namespace
{
	using OpenGLShaderProgramAPI = NLS::Render::Backend::OpenGLShaderProgramAPI;

	class OpenGLTextureResource final : public NLS::Render::RHI::IRHITexture
	{
	public:
		OpenGLTextureResource(uint32_t id, NLS::Render::RHI::TextureDesc desc, std::function<void(uint32_t)> destroy)
			: m_id(id), m_desc(desc), m_destroy(std::move(destroy))
		{
		}

		~OpenGLTextureResource() override
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

	class OpenGLBufferResource final : public NLS::Render::RHI::IRHIBuffer
	{
	public:
		OpenGLBufferResource(uint32_t id, NLS::Render::RHI::BufferType type, std::function<void(uint32_t)> destroy)
			: m_id(id), m_type(type), m_destroy(std::move(destroy))
		{
		}

		~OpenGLBufferResource() override
		{
			if (m_destroy && m_id != 0)
				m_destroy(m_id);
		}

		NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Buffer; }
		uint32_t GetResourceId() const override { return m_id; }
		NLS::Render::RHI::BufferType GetBufferType() const override { return m_type; }
		size_t GetSize() const override { return m_size; }
		void SetSize(size_t size) { m_size = size; }

	private:
		uint32_t m_id = 0;
		NLS::Render::RHI::BufferType m_type = NLS::Render::RHI::BufferType::Uniform;
		size_t m_size = 0;
		std::function<void(uint32_t)> m_destroy;
	};

	GLenum ToGLTextureTarget(NLS::Render::RHI::TextureDimension dimension)
	{
		switch (dimension)
		{
		case NLS::Render::RHI::TextureDimension::TextureCube:
			return GL_TEXTURE_CUBE_MAP;
		case NLS::Render::RHI::TextureDimension::Texture2D:
		default:
			return GL_TEXTURE_2D;
		}
	}

	GLenum ToGLTextureFormat(NLS::Render::RHI::TextureFormat format)
	{
		switch (format)
		{
		case NLS::Render::RHI::TextureFormat::RGB8: return GL_RGB;
		case NLS::Render::RHI::TextureFormat::RGBA8: return GL_RGBA;
		case NLS::Render::RHI::TextureFormat::RGBA16F: return GL_RGBA;
		case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return GL_DEPTH_STENCIL;
		default: return GL_RGBA;
		}
	}

	GLenum ToGLTextureInternalFormat(NLS::Render::RHI::TextureFormat format)
	{
		switch (format)
		{
		case NLS::Render::RHI::TextureFormat::RGB8: return GL_RGB8;
		case NLS::Render::RHI::TextureFormat::RGBA8: return GL_RGBA8;
		case NLS::Render::RHI::TextureFormat::RGBA16F: return GL_RGBA16F;
		case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return GL_DEPTH24_STENCIL8;
		default: return GL_RGBA8;
		}
	}

	GLenum ToGLTextureDataType(NLS::Render::RHI::TextureFormat format)
	{
		switch (format)
		{
		case NLS::Render::RHI::TextureFormat::RGBA16F: return GL_FLOAT;
		case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return GL_UNSIGNED_INT_24_8;
		case NLS::Render::RHI::TextureFormat::RGB8:
		case NLS::Render::RHI::TextureFormat::RGBA8:
		default:
			return GL_UNSIGNED_BYTE;
		}
	}

	GLenum ToGLTextureFilter(NLS::Render::RHI::TextureFilter filter)
	{
		switch (filter)
		{
		case NLS::Render::RHI::TextureFilter::Linear: return GL_LINEAR;
		case NLS::Render::RHI::TextureFilter::Nearest:
		default:
			return GL_NEAREST;
		}
	}

	GLenum ToGLTextureWrap(NLS::Render::RHI::TextureWrap wrap)
	{
		switch (wrap)
		{
		case NLS::Render::RHI::TextureWrap::Repeat: return GL_REPEAT;
		case NLS::Render::RHI::TextureWrap::ClampToEdge:
		default:
			return GL_CLAMP_TO_EDGE;
		}
	}

	GLenum ToGLBufferTarget(NLS::Render::RHI::BufferType type)
	{
		switch (type)
		{
		case NLS::Render::RHI::BufferType::Vertex: return GL_ARRAY_BUFFER;
		case NLS::Render::RHI::BufferType::Index: return GL_ELEMENT_ARRAY_BUFFER;
		case NLS::Render::RHI::BufferType::Uniform: return GL_UNIFORM_BUFFER;
		case NLS::Render::RHI::BufferType::ShaderStorage:
		default:
			return GL_SHADER_STORAGE_BUFFER;
		}
	}

	GLenum ToGLBufferUsage(NLS::Render::RHI::BufferUsage usage)
	{
		switch (usage)
		{
		case NLS::Render::RHI::BufferUsage::StaticDraw: return GL_STATIC_DRAW;
		case NLS::Render::RHI::BufferUsage::StreamDraw: return GL_STREAM_DRAW;
		case NLS::Render::RHI::BufferUsage::DynamicDraw:
		default:
			return GL_DYNAMIC_DRAW;
		}
	}

	uint64_t HashBytes(const std::vector<uint8_t>& bytes)
	{
		uint64_t hash = 1469598103934665603ull;
		for (const auto byte : bytes)
		{
			hash ^= static_cast<uint64_t>(byte);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	std::string BuildOpenGLProgramCacheKey(const NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc)
	{
		std::string key;
		for (const auto& stage : pipelineDesc.shaderStages)
		{
			if (stage.targetPlatform != NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL)
				continue;

			key += std::to_string(static_cast<int>(stage.stage));
			key += ':';
			key += std::to_string(HashBytes(stage.bytecode));
			key += ':';
			key += std::to_string(stage.bytecode.size());
			key += '|';
		}
		return key;
	}

	uint32_t CompileOpenGLShader(uint32_t shaderType, std::string_view source)
	{
		const uint32_t shader = OpenGLShaderProgramAPI::CreateShader(shaderType);
		const std::string sourceString(source);
		const char* sourcePointer = sourceString.c_str();
		OpenGLShaderProgramAPI::SetShaderSource(shader, sourcePointer);
		OpenGLShaderProgramAPI::CompileShader(shader);

		if (OpenGLShaderProgramAPI::GetShaderCompileStatus(shader) == GL_FALSE)
		{
			GLsizei maxLength = static_cast<GLsizei>(OpenGLShaderProgramAPI::GetShaderInfoLogLength(shader));
			std::string errorLog(maxLength, ' ');
			OpenGLShaderProgramAPI::GetShaderInfoLog(shader, maxLength, &maxLength, errorLog.data());
			NLS_LOG_ERROR(std::string("[OpenGL] Failed to compile generated GLSL shader:\n") + errorLog);
			OpenGLShaderProgramAPI::DeleteShader(shader);
			return 0;
		}

		return shader;
	}

	uint32_t CreateOpenGLProgramFromPipeline(const NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc)
	{
		std::string_view vertexSource;
		std::string_view fragmentSource;

		for (const auto& stage : pipelineDesc.shaderStages)
		{
			if (stage.targetPlatform != NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL || stage.bytecode.empty())
				continue;

			const auto source = std::string_view(reinterpret_cast<const char*>(stage.bytecode.data()), stage.bytecode.size());
			if (stage.stage == NLS::Render::RHI::ShaderStage::Vertex)
				vertexSource = source;
			else if (stage.stage == NLS::Render::RHI::ShaderStage::Fragment)
				fragmentSource = source;
		}

		if (vertexSource.empty() || fragmentSource.empty())
			return 0;

		const uint32_t vertexShader = CompileOpenGLShader(GL_VERTEX_SHADER, vertexSource);
		const uint32_t fragmentShader = CompileOpenGLShader(GL_FRAGMENT_SHADER, fragmentSource);
		if (vertexShader == 0 || fragmentShader == 0)
		{
			if (vertexShader != 0)
				OpenGLShaderProgramAPI::DeleteShader(vertexShader);
			if (fragmentShader != 0)
				OpenGLShaderProgramAPI::DeleteShader(fragmentShader);
			return 0;
		}

		const uint32_t program = OpenGLShaderProgramAPI::CreateProgram();
		OpenGLShaderProgramAPI::AttachShader(program, vertexShader);
		OpenGLShaderProgramAPI::AttachShader(program, fragmentShader);
		OpenGLShaderProgramAPI::LinkProgram(program);
		OpenGLShaderProgramAPI::DeleteShader(vertexShader);
		OpenGLShaderProgramAPI::DeleteShader(fragmentShader);

		if (OpenGLShaderProgramAPI::GetLinkStatus(program) == GL_FALSE)
		{
			GLsizei maxLength = static_cast<GLsizei>(OpenGLShaderProgramAPI::GetProgramInfoLogLength(program));
			std::string errorLog(maxLength, ' ');
			OpenGLShaderProgramAPI::GetProgramInfoLog(program, maxLength, &maxLength, errorLog.data());
			NLS_LOG_ERROR(std::string("[OpenGL] Failed to link generated GLSL program:\n") + errorLog);
			OpenGLShaderProgramAPI::DeleteProgram(program);
			return 0;
		}

		return program;
	}

	void ConfigureOpenGLMeshVertexLayout(uint32_t vertexArray)
	{
		glBindVertexArray(vertexArray);

		constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 14u);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 3u));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 5u));
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 8u));
		glEnableVertexAttribArray(4);
		glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 11u));
	}

	void LogOpenGLDrawDebugOnce(bool& hasLogged)
	{
		if (hasLogged)
			return;

		GLint currentProgram = 0;
		GLint currentVertexArray = 0;
		GLint arrayBuffer = 0;
		GLint elementArrayBuffer = 0;
		GLint attrib0Enabled = 0;
		GLint attrib0Size = 0;
		GLint attrib0Stride = 0;
		GLint attrib0Type = 0;
		GLint attrib0Normalized = 0;
		GLvoid* attrib0Pointer = nullptr;

		glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &currentVertexArray);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementArrayBuffer);
		glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib0Enabled);
		glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attrib0Size);
		glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attrib0Stride);
		glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attrib0Type);
		glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attrib0Normalized);
		glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attrib0Pointer);

		std::ostringstream stream;
		stream << "[OpenGLDrawDebug] program=" << currentProgram
			<< " vao=" << currentVertexArray
			<< " arrayBuffer=" << arrayBuffer
			<< " elementArrayBuffer=" << elementArrayBuffer
			<< " attrib0Enabled=" << attrib0Enabled
			<< " attrib0Size=" << attrib0Size
			<< " attrib0Stride=" << attrib0Stride
			<< " attrib0Type=" << attrib0Type
			<< " attrib0Normalized=" << attrib0Normalized
			<< " attrib0Pointer=" << attrib0Pointer;

		if (arrayBuffer != 0)
		{
			glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
			float preview[14] = {};
			glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(preview), preview);
			stream << " firstVertex=[";
			for (size_t i = 0; i < std::size(preview); ++i)
			{
				if (i > 0)
					stream << ',';
				stream << preview[i];
			}
			stream << "]";
		}

		NLS_LOG_INFO(stream.str());
		hasLogged = true;
	}
}

namespace NLS::Render::Backend
{
	OpenGLRenderDevice::~OpenGLRenderDevice()
	{
		if (m_defaultVertexArray != 0)
			glDeleteVertexArrays(1, &m_defaultVertexArray);

		for (const auto& [_, program] : m_programCache)
		{
			if (program != 0)
				OpenGLShaderProgramAPI::DeleteProgram(program);
		}
	}

	std::optional<NLS::Render::Data::PipelineState> OpenGLRenderDevice::Init(const NLS::Render::Settings::DriverSettings& settings)
	{
		auto initialState = m_api.Init(settings.debugMode);
		m_backendReady = initialState.has_value();

		if (m_backendReady)
		{
			GLint maxTextureSize = 0;
			GLint maxColorAttachments = 0;
			glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
			glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxColorAttachments);

			m_capabilities.backendReady = true;
			m_capabilities.supportsGraphics = true;
			m_capabilities.supportsCompute = GLAD_GL_VERSION_4_3 != 0;
			m_capabilities.supportsSwapchain = true;
			m_capabilities.supportsFramebufferBlit = true;
			m_capabilities.supportsDepthBlit = true;
			m_capabilities.supportsCurrentSceneRenderer = true;
			m_capabilities.supportsOffscreenFramebuffers = true;
			m_capabilities.supportsFramebufferReadback = true;
			m_capabilities.supportsUITextureHandles = true;
			m_capabilities.supportsCubemaps = true;
			m_capabilities.supportsMultiRenderTargets = maxColorAttachments > 1;
			m_capabilities.maxTextureDimension2D = static_cast<uint32_t>(maxTextureSize);
			m_capabilities.maxColorAttachments = static_cast<uint32_t>(maxColorAttachments);

			if (m_defaultVertexArray == 0)
				glGenVertexArrays(1, &m_defaultVertexArray);
			glBindVertexArray(m_defaultVertexArray);
		}

		return initialState;
	}

	void OpenGLRenderDevice::Clear(bool colorBuffer, bool depthBuffer, bool stencilBuffer) { m_api.Clear(colorBuffer, depthBuffer, stencilBuffer); }
	void OpenGLRenderDevice::ReadPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, NLS::Render::Settings::EPixelDataFormat format, NLS::Render::Settings::EPixelDataType type, void* data) { m_api.ReadPixels(x, y, width, height, format, type, data); }
	void OpenGLRenderDevice::DrawElements(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount)
	{
		if (m_defaultVertexArray != 0)
			ConfigureOpenGLMeshVertexLayout(m_defaultVertexArray);
		LogOpenGLDrawDebugOnce(m_hasLoggedDrawDebug);
		m_api.DrawElements(primitiveMode, indexCount);
	}
	void OpenGLRenderDevice::DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t instances)
	{
		if (m_defaultVertexArray != 0)
			ConfigureOpenGLMeshVertexLayout(m_defaultVertexArray);
		LogOpenGLDrawDebugOnce(m_hasLoggedDrawDebug);
		m_api.DrawElementsInstanced(primitiveMode, indexCount, instances);
	}
	void OpenGLRenderDevice::DrawArrays(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount)
	{
		if (m_defaultVertexArray != 0)
			ConfigureOpenGLMeshVertexLayout(m_defaultVertexArray);
		LogOpenGLDrawDebugOnce(m_hasLoggedDrawDebug);
		m_api.DrawArrays(primitiveMode, vertexCount);
	}
	void OpenGLRenderDevice::DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t instances)
	{
		if (m_defaultVertexArray != 0)
			ConfigureOpenGLMeshVertexLayout(m_defaultVertexArray);
		LogOpenGLDrawDebugOnce(m_hasLoggedDrawDebug);
		m_api.DrawArraysInstanced(primitiveMode, vertexCount, instances);
	}
	void OpenGLRenderDevice::SetClearColor(float red, float green, float blue, float alpha) { m_api.SetClearColor(red, green, blue, alpha); }
	void OpenGLRenderDevice::SetRasterizationLinesWidth(float width) { m_api.SetRasterizationLinesWidth(width); }
	void OpenGLRenderDevice::SetRasterizationMode(NLS::Render::Settings::ERasterizationMode mode) { m_api.SetRasterizationMode(mode); }
	void OpenGLRenderDevice::SetCapability(NLS::Render::Settings::ERenderingCapability capability, bool value) { m_api.SetCapability(capability, value); }
	bool OpenGLRenderDevice::GetCapability(NLS::Render::Settings::ERenderingCapability capability) { return m_api.GetCapability(capability); }
	void OpenGLRenderDevice::SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm, int32_t reference, uint32_t mask) { m_api.SetStencilAlgorithm(algorithm, reference, mask); }
	void OpenGLRenderDevice::SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm algorithm) { m_api.SetDepthAlgorithm(algorithm); }
	void OpenGLRenderDevice::SetStencilMask(uint32_t mask) { m_api.SetStencilMask(mask); }
	void OpenGLRenderDevice::SetStencilOperations(NLS::Render::Settings::EOperation stencilFail, NLS::Render::Settings::EOperation depthFail, NLS::Render::Settings::EOperation bothPass) { m_api.SetStencilOperations(stencilFail, depthFail, bothPass); }
	void OpenGLRenderDevice::SetCullFace(NLS::Render::Settings::ECullFace cullFace) { m_api.SetCullFace(cullFace); }
	void OpenGLRenderDevice::SetDepthWriting(bool enable) { m_api.SetDepthWriting(enable); }
	void OpenGLRenderDevice::SetColorWriting(bool enableRed, bool enableGreen, bool enableBlue, bool enableAlpha) { m_api.SetColorWriting(enableRed, enableGreen, enableBlue, enableAlpha); }
	void OpenGLRenderDevice::SetViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height) { m_api.SetViewport(x, y, width, height); }
	void OpenGLRenderDevice::BindGraphicsPipeline(const NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc, const NLS::Render::Resources::BindingSetInstance* bindingSet)
	{
		const auto programKey = BuildOpenGLProgramCacheKey(pipelineDesc);
		uint32_t programHandle = 0;
		if (!programKey.empty())
		{
			auto found = m_programCache.find(programKey);
			if (found == m_programCache.end())
				found = m_programCache.emplace(programKey, CreateOpenGLProgramFromPipeline(pipelineDesc)).first;
			programHandle = found->second;
		}

		if (programHandle != 0)
			OpenGLShaderProgramAPI::UseProgram(programHandle);

		if (bindingSet == nullptr || pipelineDesc.reflection == nullptr)
			return;

		for (const auto& constantBuffer : pipelineDesc.reflection->constantBuffers)
		{
			if (const auto* entry = bindingSet->Find(constantBuffer.name); entry != nullptr && entry->bufferResource != nullptr)
			{
				m_api.BindBufferBase(
					GL_UNIFORM_BUFFER,
					NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(constantBuffer.bindingSpace, constantBuffer.bindingIndex),
					entry->bufferResource->GetResourceId());
			}
		}

		for (const auto& entry : bindingSet->Entries())
		{
			if (entry.kind != NLS::Render::Resources::ShaderResourceKind::SampledTexture || entry.textureResource == nullptr)
				continue;

			const auto slot = NLS::Render::RHI::BindingPointMap::GetTextureBindingPoint(entry.bindingSpace, entry.bindingIndex);
			m_api.ActivateTexture(slot);
			m_api.BindTexture(ToGLTextureTarget(entry.textureResource->GetDimension()), entry.textureResource->GetResourceId());
		}
	}
	std::shared_ptr<NLS::Render::RHI::IRHITexture> OpenGLRenderDevice::CreateTextureResource(NLS::Render::RHI::TextureDimension dimension)
	{
		NLS::Render::RHI::TextureDesc desc{};
		desc.dimension = dimension;
		return std::make_shared<OpenGLTextureResource>(CreateTexture(), desc, [this](uint32_t id) { DestroyTexture(id); });
	}
	uint32_t OpenGLRenderDevice::CreateTexture() { return m_api.CreateTexture(); }
	void OpenGLRenderDevice::DestroyTexture(uint32_t textureId) { m_api.DestroyTexture(textureId); }
	void OpenGLRenderDevice::BindTexture(NLS::Render::RHI::TextureDimension dimension, uint32_t textureId) { m_api.BindTexture(ToGLTextureTarget(dimension), textureId); }
	void OpenGLRenderDevice::ActivateTexture(uint32_t slot) { m_api.ActivateTexture(slot); }

	void OpenGLRenderDevice::SetupTexture(const NLS::Render::RHI::TextureDesc& desc, const void* data)
	{
		const auto target = ToGLTextureTarget(desc.dimension);
		m_api.SetTextureParameteri(target, GL_TEXTURE_MIN_FILTER, static_cast<int32_t>(ToGLTextureFilter(desc.minFilter)));
		m_api.SetTextureParameteri(target, GL_TEXTURE_MAG_FILTER, static_cast<int32_t>(ToGLTextureFilter(desc.magFilter)));
		m_api.SetTextureParameteri(target, GL_TEXTURE_WRAP_S, static_cast<int32_t>(ToGLTextureWrap(desc.wrapS)));
		m_api.SetTextureParameteri(target, GL_TEXTURE_WRAP_T, static_cast<int32_t>(ToGLTextureWrap(desc.wrapT)));
		if (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube)
		{
			m_api.SetTextureParameteri(target, GL_TEXTURE_WRAP_R, static_cast<int32_t>(ToGLTextureWrap(desc.wrapT)));
			const auto bytesPerFace = static_cast<size_t>(desc.width) * static_cast<size_t>(desc.height) * NLS::Render::RHI::GetTextureFormatBytesPerPixel(desc.format);
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (uint32_t face = 0; face < NLS::Render::RHI::GetTextureLayerCount(desc.dimension); ++face)
			{
				const void* faceData = bytes != nullptr ? bytes + bytesPerFace * face : nullptr;
				m_api.SetTextureImage2D(
					GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
					0,
					static_cast<int32_t>(ToGLTextureInternalFormat(desc.format)),
					desc.width,
					desc.height,
					ToGLTextureFormat(desc.format),
					ToGLTextureDataType(desc.format),
					faceData);
			}
			return;
		}

		m_api.SetTextureImage2D(target, 0, static_cast<int32_t>(ToGLTextureInternalFormat(desc.format)), desc.width, desc.height, ToGLTextureFormat(desc.format), ToGLTextureDataType(desc.format), data);
	}

	void OpenGLRenderDevice::GenerateTextureMipmap(NLS::Render::RHI::TextureDimension dimension) { m_api.GenerateTextureMipmap(ToGLTextureTarget(dimension)); }
	uint32_t OpenGLRenderDevice::CreateFramebuffer() { return m_api.CreateFramebuffer(); }
	void OpenGLRenderDevice::DestroyFramebuffer(uint32_t framebufferId) { m_api.DestroyFramebuffer(framebufferId); }
	void OpenGLRenderDevice::BindFramebuffer(uint32_t framebufferId) { m_api.BindFramebuffer(framebufferId); }
	void OpenGLRenderDevice::AttachFramebufferColorTexture(uint32_t framebufferId, uint32_t textureId, uint32_t attachmentIndex) { m_api.AttachFramebufferColorTexture(framebufferId, textureId, attachmentIndex); }
	void OpenGLRenderDevice::AttachFramebufferDepthStencilTexture(uint32_t framebufferId, uint32_t textureId) { m_api.AttachFramebufferDepthStencilTexture(framebufferId, textureId); }
	void OpenGLRenderDevice::SetFramebufferDrawBufferCount(uint32_t framebufferId, uint32_t colorAttachmentCount) { m_api.SetFramebufferDrawBufferCount(framebufferId, colorAttachmentCount); }
	void OpenGLRenderDevice::BlitDepth(uint32_t sourceFramebufferId, uint32_t destinationFramebufferId, uint32_t width, uint32_t height) { m_api.BlitDepth(sourceFramebufferId, destinationFramebufferId, width, height); }
	std::shared_ptr<NLS::Render::RHI::IRHIBuffer> OpenGLRenderDevice::CreateBufferResource(NLS::Render::RHI::BufferType type)
	{
		return std::make_shared<OpenGLBufferResource>(CreateBuffer(), type, [this](uint32_t id) { DestroyBuffer(id); });
	}
	uint32_t OpenGLRenderDevice::CreateBuffer() { return m_api.CreateBuffer(); }
	void OpenGLRenderDevice::DestroyBuffer(uint32_t bufferId) { m_api.DestroyBuffer(bufferId); }
	void OpenGLRenderDevice::BindBuffer(NLS::Render::RHI::BufferType type, uint32_t bufferId) { m_api.BindBuffer(ToGLBufferTarget(type), bufferId); }
	void OpenGLRenderDevice::BindBufferBase(NLS::Render::RHI::BufferType type, uint32_t bindingPoint, uint32_t bufferId) { m_api.BindBufferBase(ToGLBufferTarget(type), bindingPoint, bufferId); }
	void OpenGLRenderDevice::SetBufferData(NLS::Render::RHI::BufferType type, size_t size, const void* data, NLS::Render::RHI::BufferUsage usage) { m_api.SetBufferData(ToGLBufferTarget(type), size, data, ToGLBufferUsage(usage)); }
	void OpenGLRenderDevice::SetBufferSubData(NLS::Render::RHI::BufferType type, size_t offset, size_t size, const void* data) { m_api.SetBufferSubData(ToGLBufferTarget(type), offset, size, data); }
	void* OpenGLRenderDevice::GetUITextureHandle(uint32_t textureId) const
	{
		if (textureId == 0 || textureId == static_cast<uint32_t>(-1))
			return nullptr;

		return reinterpret_cast<void*>(static_cast<intptr_t>(textureId));
	}
	void OpenGLRenderDevice::ReleaseUITextureHandles() {}
	bool OpenGLRenderDevice::PrepareUIRender() { return true; }
	std::string OpenGLRenderDevice::GetVendor() { return m_api.GetVendor(); }
	std::string OpenGLRenderDevice::GetHardware() { return m_api.GetHardware(); }
	std::string OpenGLRenderDevice::GetVersion() { return m_api.GetVersion(); }
	std::string OpenGLRenderDevice::GetShadingLanguageVersion() { return m_api.GetShadingLanguageVersion(); }
	NLS::Render::RHI::RHIDeviceCapabilities OpenGLRenderDevice::GetCapabilities() const { return m_capabilities; }
	NLS::Render::RHI::NativeRenderDeviceInfo OpenGLRenderDevice::GetNativeDeviceInfo() const
	{
		NLS::Render::RHI::NativeRenderDeviceInfo info{};
		info.backend = NLS::Render::RHI::NativeBackendType::OpenGL;
		info.platformWindow = m_swapchainWindow;
		return info;
	}
	bool OpenGLRenderDevice::IsBackendReady() const { return m_backendReady; }
	bool OpenGLRenderDevice::CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc)
	{
		m_swapchainWindow = static_cast<GLFWwindow*>(desc.platformWindow);
		return m_backendReady && m_swapchainWindow != nullptr;
	}
	void OpenGLRenderDevice::DestroySwapchain() { m_swapchainWindow = nullptr; }
	void OpenGLRenderDevice::ResizeSwapchain(uint32_t, uint32_t) {}
	void OpenGLRenderDevice::PresentSwapchain()
	{
		if (m_swapchainWindow)
			glfwSwapBuffers(m_swapchainWindow);
	}
}
