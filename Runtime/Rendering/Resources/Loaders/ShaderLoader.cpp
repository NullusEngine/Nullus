#include <sstream>
#include <cstring>
#include <exception>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>

#include <spirv_glsl.hpp>
#include <spirv_cross_util.hpp>

#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/ShaderCompiler/ShaderAsset.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

namespace
{
	using ShaderAsset = NLS::Render::ShaderCompiler::ShaderAsset;
	using ShaderCompileOptions = NLS::Render::ShaderCompiler::ShaderCompileOptions;
	using ShaderCompilationOutput = NLS::Render::ShaderCompiler::ShaderCompilationOutput;
	using ShaderCompilationStatus = NLS::Render::ShaderCompiler::ShaderCompilationStatus;
	using ShaderCompilationInput = NLS::Render::ShaderCompiler::ShaderCompilationInput;
	using ShaderSourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage;
	using ShaderStage = NLS::Render::ShaderCompiler::ShaderStage;
	using ShaderTargetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform;
	using ShaderReflection = NLS::Render::Resources::ShaderReflection;
	using ShaderPropertyDesc = NLS::Render::Resources::ShaderPropertyDesc;
	using ShaderConstantBufferDesc = NLS::Render::Resources::ShaderConstantBufferDesc;
	using Driver = NLS::Render::Context::Driver;

	uint32_t GetOpenGLTextureBindingPoint(uint32_t bindingSpace, uint32_t bindingIndex)
	{
		return bindingSpace * 16u + bindingIndex;
	}

	uint32_t GetOpenGLUniformBufferBindingPoint(uint32_t bindingSpace, uint32_t bindingIndex)
	{
		return 8u + bindingSpace * 4u + bindingIndex;
	}

	std::vector<uint32_t> ToSpirvWords(const std::vector<uint8_t>& bytecode)
	{
		if (bytecode.size() % sizeof(uint32_t) != 0)
			return {};

		std::vector<uint32_t> words(bytecode.size() / sizeof(uint32_t));
		std::memcpy(words.data(), bytecode.data(), bytecode.size());
		return words;
	}

	std::string GetGLSLCachePath(const std::string& spirvArtifactPath, ShaderStage stage)
	{
		auto path = std::filesystem::path(spirvArtifactPath);
		path.replace_extension(stage == ShaderStage::Vertex ? ".vert.glsl" : ".frag.glsl");
		return path.string();
	}

	bool WriteTextFile(const std::string& path, const std::string& content)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
			return false;

		stream << content;
		return true;
	}

	std::vector<uint8_t> ReadBinaryFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return {};

		return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	void NormalizeCompilationResult(ShaderCompilationOutput& output)
	{
		if (output.artifactPath.empty() || !std::filesystem::exists(output.artifactPath))
			return;

		if (output.bytecode.empty())
			output.bytecode = ReadBinaryFile(output.artifactPath);

		if (!output.bytecode.empty())
		{
			const bool wasRecovered = output.status != ShaderCompilationStatus::Succeeded;
			output.status = ShaderCompilationStatus::Succeeded;
			if (wasRecovered && output.diagnostics.empty())
				output.diagnostics = "Recovered shader compilation result from generated artifact.";
		}
	}

	bool HasUsableCompilationResult(const ShaderCompilationOutput& output)
	{
		return !output.bytecode.empty() &&
			!output.artifactPath.empty() &&
			std::filesystem::exists(output.artifactPath);
	}

	const char* ShaderStageLabel(ShaderStage stage)
	{
		switch (stage)
		{
		case ShaderStage::Vertex: return "VS";
		case ShaderStage::Pixel: return "PS";
		case ShaderStage::Compute: return "CS";
		default: return "Unknown";
		}
	}

	std::string CrossCompileSpirvToGLSL(const ShaderCompilationOutput& spirvOutput, ShaderStage stage)
	{
		const auto spirv = ToSpirvWords(spirvOutput.bytecode);
		if (spirv.empty())
			return {};

		try
		{
			spirv_cross::CompilerGLSL compiler(spirv);
			auto options = compiler.get_common_options();
			options.version = 430;
			options.es = false;
			options.vulkan_semantics = false;
			options.separate_shader_objects = false;
			options.enable_420pack_extension = true;
			options.vertex.flip_vert_y = false;
			options.vertex.fixup_clipspace = false;
			compiler.set_common_options(options);

			auto resources = compiler.get_shader_resources();
			compiler.build_combined_image_samplers();
			spirv_cross_util::inherit_combined_sampler_bindings(compiler);

			for (const auto& resource : resources.uniform_buffers)
			{
				const auto descriptorSet = compiler.has_decoration(resource.id, spv::DecorationDescriptorSet)
					? compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)
					: 0u;
				const auto binding = compiler.has_decoration(resource.id, spv::DecorationBinding)
					? compiler.get_decoration(resource.id, spv::DecorationBinding)
					: 0u;
				compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
				compiler.set_decoration(resource.id, spv::DecorationBinding, GetOpenGLUniformBufferBindingPoint(descriptorSet, binding));
			}

			for (const auto& resource : resources.sampled_images)
			{
				const auto descriptorSet = compiler.has_decoration(resource.id, spv::DecorationDescriptorSet)
					? compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)
					: 0u;
				const auto binding = compiler.has_decoration(resource.id, spv::DecorationBinding)
					? compiler.get_decoration(resource.id, spv::DecorationBinding)
					: 0u;
				compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
				compiler.set_decoration(resource.id, spv::DecorationBinding, GetOpenGLTextureBindingPoint(descriptorSet, binding));
			}

			for (const auto& combined : compiler.get_combined_image_samplers())
			{
				const auto descriptorSet = compiler.has_decoration(combined.combined_id, spv::DecorationDescriptorSet)
					? compiler.get_decoration(combined.combined_id, spv::DecorationDescriptorSet)
					: (compiler.has_decoration(combined.image_id, spv::DecorationDescriptorSet)
						? compiler.get_decoration(combined.image_id, spv::DecorationDescriptorSet)
						: 0u);
				const auto binding = compiler.has_decoration(combined.combined_id, spv::DecorationBinding)
					? compiler.get_decoration(combined.combined_id, spv::DecorationBinding)
					: (compiler.has_decoration(combined.image_id, spv::DecorationBinding)
						? compiler.get_decoration(combined.image_id, spv::DecorationBinding)
						: 0u);

				compiler.unset_decoration(combined.combined_id, spv::DecorationDescriptorSet);
				compiler.set_decoration(combined.combined_id, spv::DecorationBinding, GetOpenGLTextureBindingPoint(descriptorSet, binding));
				compiler.set_name(combined.combined_id, compiler.get_name(combined.image_id));
			}

			const auto glsl = compiler.compile();
			WriteTextFile(GetGLSLCachePath(spirvOutput.artifactPath, stage), glsl);
			return glsl;
		}
		catch (const spirv_cross::CompilerError& error)
		{
			NLS_LOG_ERROR("[HLSL][OpenGL][" + std::string(ShaderStageLabel(stage)) + "] Cross-compilation failed for artifact \"" + spirvOutput.artifactPath + "\":\n" + error.what());
		}
		catch (const std::exception& error)
		{
			NLS_LOG_ERROR("[HLSL][OpenGL][" + std::string(ShaderStageLabel(stage)) + "] Unexpected exception while generating GLSL for artifact \"" + spirvOutput.artifactPath + "\":\n" + error.what());
		}
		catch (...)
		{
			NLS_LOG_ERROR("[HLSL][OpenGL][" + std::string(ShaderStageLabel(stage)) + "] Unknown exception while generating GLSL for artifact \"" + spirvOutput.artifactPath + "\".");
		}

		return {};
	}

	void MergeReflection(ShaderReflection& destination, const ShaderReflection& source)
	{
		auto upsertProperty = [&destination](const ShaderPropertyDesc& property)
		{
			const auto found = std::find_if(destination.properties.begin(), destination.properties.end(), [&property](const ShaderPropertyDesc& existing)
			{
				return existing.name == property.name
					&& existing.kind == property.kind
					&& existing.bindingSpace == property.bindingSpace
					&& existing.bindingIndex == property.bindingIndex
					&& existing.parentConstantBuffer == property.parentConstantBuffer;
			});

			if (found == destination.properties.end())
				destination.properties.push_back(property);
		};

		auto upsertConstantBuffer = [&destination](const ShaderConstantBufferDesc& buffer)
		{
			const auto found = std::find_if(destination.constantBuffers.begin(), destination.constantBuffers.end(), [&buffer](const ShaderConstantBufferDesc& existing)
			{
				return existing.name == buffer.name
					&& existing.bindingSpace == buffer.bindingSpace
					&& existing.bindingIndex == buffer.bindingIndex;
			});

			if (found == destination.constantBuffers.end())
				destination.constantBuffers.push_back(buffer);
		};

		for (const auto& property : source.properties)
			upsertProperty(property);

		for (const auto& buffer : source.constantBuffers)
			upsertConstantBuffer(buffer);
	}

    NLS::Render::Settings::EGraphicsBackend ResolveActiveGraphicsBackend()
    {
        if (NLS::Core::ServiceLocator::Contains<Driver>())
        {
            const auto backend = NLS::Core::ServiceLocator::Get<Driver>().GetNativeDeviceInfo().backend;
            switch (backend)
            {
            case NLS::Render::RHI::NativeBackendType::DX12: return NLS::Render::Settings::EGraphicsBackend::DX12;
            case NLS::Render::RHI::NativeBackendType::Vulkan: return NLS::Render::Settings::EGraphicsBackend::VULKAN;
            case NLS::Render::RHI::NativeBackendType::OpenGL: return NLS::Render::Settings::EGraphicsBackend::OPENGL;
            case NLS::Render::RHI::NativeBackendType::Metal: return NLS::Render::Settings::EGraphicsBackend::METAL;
            case NLS::Render::RHI::NativeBackendType::None:
            default:
                break;
            }
        }

        if (const auto backend = NLS::Render::Settings::TryReadGraphicsBackendFromEnvironment("NLS_GRAPHICS_BACKEND"); backend.has_value())
            return backend.value();

        return NLS::Render::Settings::GetPlatformDefaultGraphicsBackend();
    }
}

namespace NLS::Render::Resources::Loaders
{
std::string ShaderLoader::__FILE_TRACE;

Shader* ShaderLoader::Create(const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;
	return CreateHLSLShaderAsset(p_filePath);
}

void ShaderLoader::Recompile(Shader& p_shader, const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;

	if (Shader* refreshed = CreateHLSLShaderAsset(p_filePath); refreshed != nullptr)
	{
		p_shader.SetReflection(refreshed->GetReflection());
		p_shader.ClearCompiledArtifacts();
		for (const auto* artifact : {
			refreshed->FindCompiledArtifact(ShaderStage::Vertex, ShaderTargetPlatform::DXIL),
			refreshed->FindCompiledArtifact(ShaderStage::Pixel, ShaderTargetPlatform::DXIL),
			refreshed->FindCompiledArtifact(ShaderStage::Vertex, ShaderTargetPlatform::SPIRV),
			refreshed->FindCompiledArtifact(ShaderStage::Pixel, ShaderTargetPlatform::SPIRV),
            refreshed->FindCompiledArtifact(ShaderStage::Vertex, ShaderTargetPlatform::GLSL),
            refreshed->FindCompiledArtifact(ShaderStage::Pixel, ShaderTargetPlatform::GLSL) })
		{
			if (artifact != nullptr)
				p_shader.SetCompiledArtifact(*artifact);
		}
		delete refreshed;
		NLS_LOG_INFO("[COMPILE] \"" + __FILE_TRACE + "\": HLSL shader refreshed.");
	}
	else
	{
		NLS_LOG_ERROR("[COMPILE] \"" + __FILE_TRACE + "\": Failed to refresh HLSL shader.");
	}
}

bool ShaderLoader::Destroy(Shader*& p_shader)
{
	if (p_shader)
	{
		delete p_shader;
		p_shader = nullptr;
		return true;
	}

	return false;
}

Shader* ShaderLoader::CreateHLSLShaderAsset(const std::string& p_filePath)
{
	NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    const auto activeBackend = ResolveActiveGraphicsBackend();
    const bool compileDxil = activeBackend == NLS::Render::Settings::EGraphicsBackend::DX12 ||
        activeBackend == NLS::Render::Settings::EGraphicsBackend::NONE;
    const bool compileSpirv = activeBackend == NLS::Render::Settings::EGraphicsBackend::VULKAN ||
        activeBackend == NLS::Render::Settings::EGraphicsBackend::OPENGL ||
        activeBackend == NLS::Render::Settings::EGraphicsBackend::NONE;
    const bool compileGlsl = activeBackend == NLS::Render::Settings::EGraphicsBackend::OPENGL;
	ShaderCompileOptions vertexDxilOptions;
	vertexDxilOptions.sourceLanguage = ShaderSourceLanguage::HLSL;
	vertexDxilOptions.targetPlatform = ShaderTargetPlatform::DXIL;
	vertexDxilOptions.entryPoint = "VSMain";
	vertexDxilOptions.targetProfile = "vs_6_0";
	vertexDxilOptions.includeDirectories.push_back(std::filesystem::path(p_filePath).parent_path().string());

	ShaderCompileOptions pixelDxilOptions;
	pixelDxilOptions.sourceLanguage = ShaderSourceLanguage::HLSL;
	pixelDxilOptions.targetPlatform = ShaderTargetPlatform::DXIL;
	pixelDxilOptions.entryPoint = "PSMain";
	pixelDxilOptions.targetProfile = "ps_6_0";
	pixelDxilOptions.includeDirectories = vertexDxilOptions.includeDirectories;

	ShaderCompileOptions vertexSpirvOptions = vertexDxilOptions;
	vertexSpirvOptions.targetPlatform = ShaderTargetPlatform::SPIRV;

	ShaderCompileOptions pixelSpirvOptions = pixelDxilOptions;
	pixelSpirvOptions.targetPlatform = ShaderTargetPlatform::SPIRV;

	const ShaderCompilationInput vertexDxilInput{ p_filePath, ShaderStage::Vertex, vertexDxilOptions };
	const ShaderCompilationInput pixelDxilInput{ p_filePath, ShaderStage::Pixel, pixelDxilOptions };
	const ShaderCompilationInput vertexSpirvInput{ p_filePath, ShaderStage::Vertex, vertexSpirvOptions };
	const ShaderCompilationInput pixelSpirvInput{ p_filePath, ShaderStage::Pixel, pixelSpirvOptions };

    ShaderCompilationOutput vertexDxilResult;
    ShaderCompilationOutput pixelDxilResult;
    ShaderCompilationOutput vertexSpirvResult;
    ShaderCompilationOutput pixelSpirvResult;
    if (compileDxil)
    {
	    vertexDxilResult = compiler.Compile(vertexDxilInput);
	    pixelDxilResult = compiler.Compile(pixelDxilInput);
    }
    if (compileSpirv)
    {
	    vertexSpirvResult = compiler.Compile(vertexSpirvInput);
	    pixelSpirvResult = compiler.Compile(pixelSpirvInput);
    }
	NormalizeCompilationResult(vertexDxilResult);
	NormalizeCompilationResult(pixelDxilResult);
	NormalizeCompilationResult(vertexSpirvResult);
	NormalizeCompilationResult(pixelSpirvResult);
	ShaderReflection reflection;

	if (HasUsableCompilationResult(vertexDxilResult))
		MergeReflection(reflection, compiler.Reflect(vertexDxilInput, vertexDxilResult));
	if (HasUsableCompilationResult(pixelDxilResult))
		MergeReflection(reflection, compiler.Reflect(pixelDxilInput, pixelDxilResult));
    if (reflection.constantBuffers.empty() && HasUsableCompilationResult(vertexSpirvResult))
        MergeReflection(reflection, compiler.Reflect(vertexSpirvInput, vertexSpirvResult));
    if (reflection.constantBuffers.empty() && HasUsableCompilationResult(pixelSpirvResult))
        MergeReflection(reflection, compiler.Reflect(pixelSpirvInput, pixelSpirvResult));
	const auto vertexGLSL = compileGlsl && HasUsableCompilationResult(vertexSpirvResult)
		? CrossCompileSpirvToGLSL(vertexSpirvResult, ShaderStage::Vertex)
		: std::string{};
	const auto pixelGLSL = compileGlsl && HasUsableCompilationResult(pixelSpirvResult)
		? CrossCompileSpirvToGLSL(pixelSpirvResult, ShaderStage::Pixel)
		: std::string{};

	auto* shader = new Shader(p_filePath, ShaderSourceLanguage::HLSL);
	shader->SetReflection(std::move(reflection));
	if (HasUsableCompilationResult(vertexDxilResult))
		shader->SetCompiledArtifact({ ShaderStage::Vertex, ShaderTargetPlatform::DXIL, vertexDxilOptions.entryPoint, vertexDxilOptions.targetProfile, vertexDxilResult });
	if (HasUsableCompilationResult(pixelDxilResult))
		shader->SetCompiledArtifact({ ShaderStage::Pixel, ShaderTargetPlatform::DXIL, pixelDxilOptions.entryPoint, pixelDxilOptions.targetProfile, pixelDxilResult });
	if (HasUsableCompilationResult(vertexSpirvResult))
		shader->SetCompiledArtifact({ ShaderStage::Vertex, ShaderTargetPlatform::SPIRV, vertexSpirvOptions.entryPoint, vertexSpirvOptions.targetProfile, vertexSpirvResult });
	if (HasUsableCompilationResult(pixelSpirvResult))
		shader->SetCompiledArtifact({ ShaderStage::Pixel, ShaderTargetPlatform::SPIRV, pixelSpirvOptions.entryPoint, pixelSpirvOptions.targetProfile, pixelSpirvResult });
	if (!vertexGLSL.empty())
	{
		ShaderCompilationOutput glslVertexOutput;
		glslVertexOutput.status = ShaderCompilationStatus::Succeeded;
		glslVertexOutput.bytecode.assign(vertexGLSL.begin(), vertexGLSL.end());
		glslVertexOutput.diagnostics = "Generated from SPIR-V.";
		shader->SetCompiledArtifact({ ShaderStage::Vertex, ShaderTargetPlatform::GLSL, "main", "glsl_430", std::move(glslVertexOutput) });
	}
	if (!pixelGLSL.empty())
	{
		ShaderCompilationOutput glslPixelOutput;
		glslPixelOutput.status = ShaderCompilationStatus::Succeeded;
		glslPixelOutput.bytecode.assign(pixelGLSL.begin(), pixelGLSL.end());
		glslPixelOutput.diagnostics = "Generated from SPIR-V.";
		shader->SetCompiledArtifact({ ShaderStage::Pixel, ShaderTargetPlatform::GLSL, "main", "glsl_430", std::move(glslPixelOutput) });
	}

	if (compileDxil && !HasUsableCompilationResult(vertexDxilResult))
		NLS_LOG_ERROR("[HLSL][DXIL][VS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(vertexDxilResult.status)) + " artifact=\"" + vertexDxilResult.artifactPath + "\":\n" + vertexDxilResult.diagnostics);
	if (compileDxil && !HasUsableCompilationResult(pixelDxilResult))
		NLS_LOG_ERROR("[HLSL][DXIL][PS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(pixelDxilResult.status)) + " artifact=\"" + pixelDxilResult.artifactPath + "\":\n" + pixelDxilResult.diagnostics);
	if (compileSpirv && !HasUsableCompilationResult(vertexSpirvResult))
		NLS_LOG_ERROR("[HLSL][SPIR-V][VS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(vertexSpirvResult.status)) + " artifact=\"" + vertexSpirvResult.artifactPath + "\":\n" + vertexSpirvResult.diagnostics);
	if (compileSpirv && !HasUsableCompilationResult(pixelSpirvResult))
		NLS_LOG_ERROR("[HLSL][SPIR-V][PS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(pixelSpirvResult.status)) + " artifact=\"" + pixelSpirvResult.artifactPath + "\":\n" + pixelSpirvResult.diagnostics);
	if (compileGlsl &&
        (HasUsableCompilationResult(vertexSpirvResult) && HasUsableCompilationResult(pixelSpirvResult)) &&
		(vertexGLSL.empty() || pixelGLSL.empty()))
	{
		NLS_LOG_ERROR("[HLSL][OpenGL] \"" + __FILE_TRACE + "\": Failed to generate GLSL artifacts from SPIR-V.");
	}

	return shader;
}
}
