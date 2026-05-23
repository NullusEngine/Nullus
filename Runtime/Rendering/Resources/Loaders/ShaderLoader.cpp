#include <algorithm>
#include <cctype>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

#include <Debug/Logger.h>

#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/ShaderType.h"
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
	using ShaderReflectionInput = NLS::Render::ShaderCompiler::ShaderReflectionInput;
	using ShaderSourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage;
	using ShaderStage = NLS::Render::ShaderCompiler::ShaderStage;
	using ShaderTargetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform;
	using ShaderReflection = NLS::Render::Resources::ShaderReflection;
	using ShaderPropertyDesc = NLS::Render::Resources::ShaderPropertyDesc;
	using ShaderConstantBufferDesc = NLS::Render::Resources::ShaderConstantBufferDesc;

	constexpr const char* kAppDirectoryName = "App";
	constexpr const char* kAssetsDirectoryName = "Assets";
	constexpr const char* kEngineDirectoryName = "Engine";
	constexpr const char* kEditorDirectoryName = "Editor";
	constexpr const char* kLibraryDirectoryName = "Library";
	constexpr const char* kShaderCacheDirectoryName = "ShaderCache";
	constexpr const char* kShaderCacheDatabaseFileName = "ShaderCache.tsv";

	std::string ToLowerAscii(std::string value)
	{
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](const unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return value;
	}

	bool PathNameEquals(const std::filesystem::path& path, const char* expected)
	{
		return ToLowerAscii(path.filename().string()) == ToLowerAscii(expected);
	}

	std::string MakeShaderCacheDatabasePath(const std::filesystem::path& projectRoot)
	{
		return (projectRoot /
			kLibraryDirectoryName /
			kShaderCacheDirectoryName /
			kShaderCacheDatabaseFileName).string();
	}

	bool IsBuiltInAppAssetsPath(
		const std::filesystem::path& assetsRoot,
		const std::filesystem::path& sourcePath)
	{
		if (!PathNameEquals(assetsRoot.parent_path(), kAppDirectoryName))
			return false;

		const auto relativeSourcePath = sourcePath.lexically_relative(assetsRoot);
		if (relativeSourcePath.empty())
			return false;

		const auto firstAssetSegment = relativeSourcePath.begin();
		return firstAssetSegment != relativeSourcePath.end() &&
			(PathNameEquals(*firstAssetSegment, kEngineDirectoryName) ||
			 PathNameEquals(*firstAssetSegment, kEditorDirectoryName));
	}

	std::string GetShaderCacheDatabasePath(const std::string& sourcePath)
	{
		std::filesystem::path path(sourcePath);
		if (path.is_relative())
			path = std::filesystem::absolute(path);

		std::error_code error;
		path = std::filesystem::weakly_canonical(path, error);
		if (error)
			path = std::filesystem::absolute(std::filesystem::path(sourcePath));
		path = path.lexically_normal();

		for (auto probe = path.parent_path(); !probe.empty(); probe = probe.parent_path())
		{
			if (PathNameEquals(probe, kAssetsDirectoryName))
			{
				const auto assetRoot = probe.parent_path();
				if (IsBuiltInAppAssetsPath(probe, path))
					return {};

				return MakeShaderCacheDatabasePath(assetRoot);
			}

			if (probe == probe.parent_path())
				break;
		}
		return {};
	}

	std::string GetConfiguredProjectShaderCacheDatabasePath(const std::string& projectAssetsPath)
	{
		if (projectAssetsPath.empty())
			return {};

		std::filesystem::path assetsPath(projectAssetsPath);
		if (assetsPath.is_relative())
			assetsPath = std::filesystem::absolute(assetsPath);

		std::error_code error;
		assetsPath = std::filesystem::weakly_canonical(assetsPath, error);
		if (error)
			assetsPath = std::filesystem::absolute(std::filesystem::path(projectAssetsPath));
		assetsPath = assetsPath.lexically_normal();
		while (!assetsPath.empty() && assetsPath.filename().empty())
		{
			const auto parent = assetsPath.parent_path();
			if (parent == assetsPath)
				break;
			assetsPath = parent;
		}

		const auto projectRoot = PathNameEquals(assetsPath, kAssetsDirectoryName) ? assetsPath.parent_path() : assetsPath;
		return MakeShaderCacheDatabasePath(projectRoot);
	}

	std::vector<uint8_t> ReadBinaryFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return {};

		return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	std::string ReadTextFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return {};

		return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	bool HasEntryPointToken(const std::string& source, std::string_view entryPoint)
	{
		return !source.empty() && source.find(std::string(entryPoint)) != std::string::npos;
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

	std::optional<NLS::Render::Settings::EGraphicsBackend> ResolveActiveGraphicsBackend()
	{
		if (const auto backend = NLS::Render::Context::TryGetLocatedActiveGraphicsBackend();
			backend.has_value() && backend.value() != NLS::Render::Settings::EGraphicsBackend::NONE)
		{
			return backend.value();
		}

		return std::nullopt;
	}

	std::vector<NLS::Render::Resources::ShaderParameterStruct> BuildRegisteredShaderParameterStructs(const std::string& shaderPath)
	{
		const auto shaderTypes = NLS::Render::Resources::GetShaderTypeRegistry().FindBySourcePath(shaderPath);
		const auto materialStageType = std::find_if(
			shaderTypes.begin(),
			shaderTypes.end(),
			[](const NLS::Render::Resources::ShaderType* shaderType)
			{
				return shaderType != nullptr &&
					shaderType->GetKind() == NLS::Render::Resources::ShaderTypeKind::Material &&
					shaderType->GetStage() == NLS::Render::ShaderCompiler::ShaderStage::Pixel;
			});
		if (materialStageType != shaderTypes.end())
			return (*materialStageType)->GetRootParameterStructs();

		if (!shaderTypes.empty())
			return shaderTypes.front()->GetRootParameterStructs();

		return {};
	}

	NLS::Render::Assets::ShaderArtifactStage MakeSpirvArtifactStage(
		const ShaderCompilationInput& input,
		ShaderCompilationOutput output)
	{
		NLS::Render::Assets::ShaderArtifactStage stage;
		stage.stage = input.stage;
		stage.targetPlatform = ShaderTargetPlatform::SPIRV;
		stage.entryPoint = input.options.entryPoint;
		stage.targetProfile = input.options.targetProfile;
		stage.output = std::move(output);
		return stage;
	}

	NLS::Render::Assets::ShaderArtifact BuildGlslRuntimeFallbackArtifact(
		const std::string& sourcePath,
		const ShaderCompilationInput& vertexSpirvInput,
		const ShaderCompilationOutput& vertexSpirvResult,
		const ShaderCompilationInput& pixelSpirvInput,
		const ShaderCompilationOutput& pixelSpirvResult,
		const ShaderCompilationInput& computeSpirvInput,
		const ShaderCompilationOutput& computeSpirvResult)
	{
		NLS::Render::Assets::ShaderArtifact artifact;
		artifact.sourcePath = sourcePath;
		artifact.subAssetKey = "runtime:" + std::filesystem::path(sourcePath).stem().generic_string();
		artifact.targetPlatform = "runtime";

		if (HasUsableCompilationResult(vertexSpirvResult))
			artifact.stages.push_back(MakeSpirvArtifactStage(vertexSpirvInput, vertexSpirvResult));
		if (HasUsableCompilationResult(pixelSpirvResult))
			artifact.stages.push_back(MakeSpirvArtifactStage(pixelSpirvInput, pixelSpirvResult));
		if (HasUsableCompilationResult(computeSpirvResult))
			artifact.stages.push_back(MakeSpirvArtifactStage(computeSpirvInput, computeSpirvResult));

		NLS::Render::Assets::AppendGlslShaderArtifactStages(artifact);
		return artifact;
	}

	bool HasGlslArtifactStage(
		const NLS::Render::Assets::ShaderArtifact& artifact,
		ShaderStage stage)
	{
		return std::any_of(
			artifact.stages.begin(),
			artifact.stages.end(),
			[stage](const NLS::Render::Assets::ShaderArtifactStage& candidate)
			{
				return candidate.stage == stage &&
					candidate.targetPlatform == ShaderTargetPlatform::GLSL &&
					candidate.output.status == ShaderCompilationStatus::Succeeded &&
					!candidate.output.bytecode.empty();
			});
	}
}

namespace NLS::Render::Resources::Loaders
{
std::string ShaderLoader::__FILE_TRACE;
namespace
{
	std::string g_defaultProjectAssetsPath;
}

Shader* ShaderLoader::Create(const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;
	auto extension = std::filesystem::path(p_filePath).extension().string();
	std::transform(
		extension.begin(),
		extension.end(),
		extension.begin(),
		[](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
	if (extension == ".nshader")
		return CreateImportedShaderArtifact(p_filePath);
	return CreateHLSLShaderAsset(p_filePath, ResolveProjectAssetsPath({}));
}

Shader* ShaderLoader::Create(const std::string& p_filePath, const std::string& p_projectAssetsPath)
{
	__FILE_TRACE = p_filePath;
	auto extension = std::filesystem::path(p_filePath).extension().string();
	std::transform(
		extension.begin(),
		extension.end(),
		extension.begin(),
		[](const unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
	if (extension == ".nshader")
		return CreateImportedShaderArtifact(p_filePath);
	return CreateHLSLShaderAsset(p_filePath, ResolveProjectAssetsPath(p_projectAssetsPath));
}

std::string ShaderLoader::GetCacheDatabasePath(
	const std::string& p_filePath,
	const std::string& p_projectAssetsPath)
{
	const auto configuredProjectPath = GetConfiguredProjectShaderCacheDatabasePath(
		ResolveProjectAssetsPath(p_projectAssetsPath));
	if (!configuredProjectPath.empty())
		return configuredProjectPath;
	return GetShaderCacheDatabasePath(p_filePath);
}

void ShaderLoader::CopyRuntimeData(Shader& p_destination, const Shader& p_source)
{
	p_destination.SetReflection(p_source.GetReflection());
	p_destination.SetParameterStructs(p_source.GetParameterStructs());
	p_destination.ClearCompiledArtifacts();
	for (const auto* artifact : {
		p_source.FindCompiledArtifact(ShaderStage::Vertex, ShaderTargetPlatform::DXIL),
		p_source.FindCompiledArtifact(ShaderStage::Pixel, ShaderTargetPlatform::DXIL),
		p_source.FindCompiledArtifact(ShaderStage::Compute, ShaderTargetPlatform::DXIL),
		p_source.FindCompiledArtifact(ShaderStage::Vertex, ShaderTargetPlatform::SPIRV),
		p_source.FindCompiledArtifact(ShaderStage::Pixel, ShaderTargetPlatform::SPIRV),
		p_source.FindCompiledArtifact(ShaderStage::Compute, ShaderTargetPlatform::SPIRV),
		p_source.FindCompiledArtifact(ShaderStage::Vertex, ShaderTargetPlatform::GLSL),
		p_source.FindCompiledArtifact(ShaderStage::Pixel, ShaderTargetPlatform::GLSL),
		p_source.FindCompiledArtifact(ShaderStage::Compute, ShaderTargetPlatform::GLSL) })
	{
		if (artifact != nullptr)
			p_destination.SetCompiledArtifact(*artifact);
	}
}

void ShaderLoader::Recompile(Shader& p_shader, const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;

	if (Shader* refreshed = Create(p_filePath, ResolveProjectAssetsPath({})); refreshed != nullptr)
	{
		CopyRuntimeData(p_shader, *refreshed);
		delete refreshed;
		NLS_LOG_INFO("[COMPILE] \"" + __FILE_TRACE + "\": shader refreshed.");
	}
	else
	{
		NLS_LOG_ERROR("[COMPILE] \"" + __FILE_TRACE + "\": Failed to refresh shader.");
	}
}

void ShaderLoader::Recompile(Shader& p_shader, const std::string& p_filePath, const std::string& p_projectAssetsPath)
{
	__FILE_TRACE = p_filePath;

	if (Shader* refreshed = Create(p_filePath, ResolveProjectAssetsPath(p_projectAssetsPath)); refreshed != nullptr)
	{
		CopyRuntimeData(p_shader, *refreshed);
		delete refreshed;
		NLS_LOG_INFO("[COMPILE] \"" + __FILE_TRACE + "\": shader refreshed.");
	}
	else
	{
		NLS_LOG_ERROR("[COMPILE] \"" + __FILE_TRACE + "\": Failed to refresh shader.");
	}
}

void ShaderLoader::SetDefaultProjectAssetsPath(const std::string& p_projectAssetsPath)
{
	g_defaultProjectAssetsPath = p_projectAssetsPath;
}

const std::string& ShaderLoader::ResolveProjectAssetsPath(const std::string& p_projectAssetsPath)
{
	return p_projectAssetsPath.empty() ? g_defaultProjectAssetsPath : p_projectAssetsPath;
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

Shader* ShaderLoader::CreateImportedShaderArtifact(const std::string& p_filePath)
{
	const auto artifact = NLS::Render::Assets::LoadShaderArtifact(p_filePath);
	if (!artifact.has_value())
	{
		NLS_LOG_ERROR("[SHADER ARTIFACT] \"" + p_filePath + "\": imported shader artifact could not be read.");
		return nullptr;
	}

	auto* shader = new Shader(p_filePath, ShaderSourceLanguage::HLSL);
	shader->SetReflection(artifact->reflection);
	for (const auto& stage : artifact->stages)
	{
		if (stage.output.status != ShaderCompilationStatus::Succeeded || stage.output.bytecode.empty())
			continue;

		shader->SetCompiledArtifact({
			stage.stage,
			stage.targetPlatform,
			stage.entryPoint,
			stage.targetProfile,
			stage.output
		});
	}

	shader->SetParameterStructs(BuildRegisteredShaderParameterStructs(artifact->sourcePath));
	return shader;
}

Shader* ShaderLoader::CreateHLSLShaderAsset(
	const std::string& p_filePath,
	const std::string& p_projectAssetsPath)
{
	NLS::Render::ShaderCompiler::ShaderCompiler compiler;
	compiler.SetCacheDatabasePath(GetCacheDatabasePath(p_filePath, p_projectAssetsPath));
	const auto sourceText = ReadTextFile(p_filePath);
	if (sourceText.empty())
	{
		NLS_LOG_ERROR("[HLSL] \"" + p_filePath + "\": shader source could not be read or is empty.");
	}
	const bool hasVertexEntryPoint = HasEntryPointToken(sourceText, "VSMain");
	const bool hasPixelEntryPoint = HasEntryPointToken(sourceText, "PSMain");
	const bool hasComputeEntryPoint = HasEntryPointToken(sourceText, "CSMain");
    const auto activeBackend = ResolveActiveGraphicsBackend();
    const bool compileAllRuntimeBackends = !activeBackend.has_value();
    const bool compileDxil = compileAllRuntimeBackends ||
        activeBackend.value_or(NLS::Render::Settings::EGraphicsBackend::NONE) == NLS::Render::Settings::EGraphicsBackend::DX12;
    const bool compileSpirv = compileAllRuntimeBackends ||
        activeBackend.value_or(NLS::Render::Settings::EGraphicsBackend::NONE) == NLS::Render::Settings::EGraphicsBackend::VULKAN ||
        activeBackend.value_or(NLS::Render::Settings::EGraphicsBackend::NONE) == NLS::Render::Settings::EGraphicsBackend::OPENGL;
    const bool compileGlsl = compileAllRuntimeBackends ||
        activeBackend.value_or(NLS::Render::Settings::EGraphicsBackend::NONE) == NLS::Render::Settings::EGraphicsBackend::OPENGL;
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

	ShaderCompileOptions computeDxilOptions = pixelDxilOptions;
	computeDxilOptions.entryPoint = "CSMain";
	computeDxilOptions.targetProfile = "cs_6_0";

	ShaderCompileOptions computeSpirvOptions = computeDxilOptions;
	computeSpirvOptions.targetPlatform = ShaderTargetPlatform::SPIRV;

	const ShaderCompilationInput vertexDxilInput{ p_filePath, ShaderStage::Vertex, vertexDxilOptions };
	const ShaderCompilationInput pixelDxilInput{ p_filePath, ShaderStage::Pixel, pixelDxilOptions };
	const ShaderCompilationInput computeDxilInput{ p_filePath, ShaderStage::Compute, computeDxilOptions };
	const ShaderCompilationInput vertexSpirvInput{ p_filePath, ShaderStage::Vertex, vertexSpirvOptions };
	const ShaderCompilationInput pixelSpirvInput{ p_filePath, ShaderStage::Pixel, pixelSpirvOptions };
	const ShaderCompilationInput computeSpirvInput{ p_filePath, ShaderStage::Compute, computeSpirvOptions };

    ShaderCompilationOutput vertexDxilResult;
    ShaderCompilationOutput pixelDxilResult;
    ShaderCompilationOutput computeDxilResult;
    ShaderCompilationOutput vertexSpirvResult;
    ShaderCompilationOutput pixelSpirvResult;
    ShaderCompilationOutput computeSpirvResult;
	std::vector<ShaderCompilationInput> compileInputs;
	std::vector<ShaderCompilationOutput*> compileOutputs;
	auto queueCompile = [&](const bool shouldCompile, const ShaderCompilationInput& input, ShaderCompilationOutput& output)
	{
		if (!shouldCompile)
			return;

		compileInputs.push_back(input);
		compileOutputs.push_back(&output);
	};
	queueCompile(compileDxil && hasVertexEntryPoint, vertexDxilInput, vertexDxilResult);
	queueCompile(compileDxil && hasPixelEntryPoint, pixelDxilInput, pixelDxilResult);
	queueCompile(compileDxil && hasComputeEntryPoint, computeDxilInput, computeDxilResult);
	queueCompile(compileSpirv && hasVertexEntryPoint, vertexSpirvInput, vertexSpirvResult);
	queueCompile(compileSpirv && hasPixelEntryPoint, pixelSpirvInput, pixelSpirvResult);
	queueCompile(compileSpirv && hasComputeEntryPoint, computeSpirvInput, computeSpirvResult);

	const auto compileResults = compiler.CompileBatch(compileInputs);
	for (size_t index = 0u; index < compileResults.size() && index < compileOutputs.size(); ++index)
		*compileOutputs[index] = compileResults[index];

	NormalizeCompilationResult(vertexDxilResult);
	NormalizeCompilationResult(pixelDxilResult);
	NormalizeCompilationResult(computeDxilResult);
	NormalizeCompilationResult(vertexSpirvResult);
	NormalizeCompilationResult(pixelSpirvResult);
	NormalizeCompilationResult(computeSpirvResult);
	ShaderReflection reflection;

	std::vector<ShaderReflectionInput> dxilReflectionInputs;
	if (HasUsableCompilationResult(vertexDxilResult))
		dxilReflectionInputs.push_back({ vertexDxilInput, vertexDxilResult });
	if (HasUsableCompilationResult(pixelDxilResult))
		dxilReflectionInputs.push_back({ pixelDxilInput, pixelDxilResult });
	if (HasUsableCompilationResult(computeDxilResult))
		dxilReflectionInputs.push_back({ computeDxilInput, computeDxilResult });
	for (const auto& reflectedStage : compiler.ReflectBatch(dxilReflectionInputs))
		MergeReflection(reflection, reflectedStage);

	if (reflection.constantBuffers.empty())
	{
		std::vector<ShaderReflectionInput> spirvReflectionInputs;
		if (HasUsableCompilationResult(vertexSpirvResult))
			spirvReflectionInputs.push_back({ vertexSpirvInput, vertexSpirvResult });
		if (HasUsableCompilationResult(pixelSpirvResult))
			spirvReflectionInputs.push_back({ pixelSpirvInput, pixelSpirvResult });
		if (HasUsableCompilationResult(computeSpirvResult))
			spirvReflectionInputs.push_back({ computeSpirvInput, computeSpirvResult });
		for (const auto& reflectedStage : compiler.ReflectBatch(spirvReflectionInputs))
			MergeReflection(reflection, reflectedStage);
	}
	const bool hasAnySpirvResult =
		HasUsableCompilationResult(vertexSpirvResult) ||
		HasUsableCompilationResult(pixelSpirvResult) ||
		HasUsableCompilationResult(computeSpirvResult);
	const auto glslFallbackArtifact = compileGlsl && hasAnySpirvResult
		? BuildGlslRuntimeFallbackArtifact(
			p_filePath,
			vertexSpirvInput,
			vertexSpirvResult,
			pixelSpirvInput,
			pixelSpirvResult,
			computeSpirvInput,
			computeSpirvResult)
		: NLS::Render::Assets::ShaderArtifact {};

	auto* shader = new Shader(p_filePath, ShaderSourceLanguage::HLSL);
	shader->SetReflection(std::move(reflection));
	shader->SetParameterStructs(BuildRegisteredShaderParameterStructs(p_filePath));
	if (HasUsableCompilationResult(vertexDxilResult))
		shader->SetCompiledArtifact({ ShaderStage::Vertex, ShaderTargetPlatform::DXIL, vertexDxilOptions.entryPoint, vertexDxilOptions.targetProfile, vertexDxilResult });
	if (HasUsableCompilationResult(pixelDxilResult))
		shader->SetCompiledArtifact({ ShaderStage::Pixel, ShaderTargetPlatform::DXIL, pixelDxilOptions.entryPoint, pixelDxilOptions.targetProfile, pixelDxilResult });
	if (HasUsableCompilationResult(computeDxilResult))
		shader->SetCompiledArtifact({ ShaderStage::Compute, ShaderTargetPlatform::DXIL, computeDxilOptions.entryPoint, computeDxilOptions.targetProfile, computeDxilResult });
	if (HasUsableCompilationResult(vertexSpirvResult))
		shader->SetCompiledArtifact({ ShaderStage::Vertex, ShaderTargetPlatform::SPIRV, vertexSpirvOptions.entryPoint, vertexSpirvOptions.targetProfile, vertexSpirvResult });
	if (HasUsableCompilationResult(pixelSpirvResult))
		shader->SetCompiledArtifact({ ShaderStage::Pixel, ShaderTargetPlatform::SPIRV, pixelSpirvOptions.entryPoint, pixelSpirvOptions.targetProfile, pixelSpirvResult });
	if (HasUsableCompilationResult(computeSpirvResult))
		shader->SetCompiledArtifact({ ShaderStage::Compute, ShaderTargetPlatform::SPIRV, computeSpirvOptions.entryPoint, computeSpirvOptions.targetProfile, computeSpirvResult });
	for (const auto& stage : glslFallbackArtifact.stages)
	{
		if (stage.targetPlatform != ShaderTargetPlatform::GLSL ||
			stage.output.status != ShaderCompilationStatus::Succeeded ||
			stage.output.bytecode.empty())
		{
			continue;
		}

		shader->SetCompiledArtifact({
			stage.stage,
			stage.targetPlatform,
			stage.entryPoint,
			stage.targetProfile,
			stage.output
		});
	}

	if (compileDxil && hasVertexEntryPoint && !HasUsableCompilationResult(vertexDxilResult))
		NLS_LOG_ERROR("[HLSL][DXIL][VS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(vertexDxilResult.status)) + " artifact=\"" + vertexDxilResult.artifactPath + "\":\n" + vertexDxilResult.diagnostics);
	if (compileDxil && hasPixelEntryPoint && !HasUsableCompilationResult(pixelDxilResult))
		NLS_LOG_ERROR("[HLSL][DXIL][PS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(pixelDxilResult.status)) + " artifact=\"" + pixelDxilResult.artifactPath + "\":\n" + pixelDxilResult.diagnostics);
	if (compileDxil && hasComputeEntryPoint && !HasUsableCompilationResult(computeDxilResult))
		NLS_LOG_ERROR("[HLSL][DXIL][CS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(computeDxilResult.status)) + " artifact=\"" + computeDxilResult.artifactPath + "\":\n" + computeDxilResult.diagnostics);
	if (compileSpirv && hasVertexEntryPoint && !HasUsableCompilationResult(vertexSpirvResult))
		NLS_LOG_ERROR("[HLSL][SPIR-V][VS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(vertexSpirvResult.status)) + " artifact=\"" + vertexSpirvResult.artifactPath + "\":\n" + vertexSpirvResult.diagnostics);
	if (compileSpirv && hasPixelEntryPoint && !HasUsableCompilationResult(pixelSpirvResult))
		NLS_LOG_ERROR("[HLSL][SPIR-V][PS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(pixelSpirvResult.status)) + " artifact=\"" + pixelSpirvResult.artifactPath + "\":\n" + pixelSpirvResult.diagnostics);
	if (compileSpirv && hasComputeEntryPoint && !HasUsableCompilationResult(computeSpirvResult))
		NLS_LOG_ERROR("[HLSL][SPIR-V][CS] \"" + __FILE_TRACE + "\" status=" + std::to_string(static_cast<int>(computeSpirvResult.status)) + " artifact=\"" + computeSpirvResult.artifactPath + "\":\n" + computeSpirvResult.diagnostics);
	if (compileGlsl &&
        ((HasUsableCompilationResult(vertexSpirvResult) && !HasGlslArtifactStage(glslFallbackArtifact, ShaderStage::Vertex)) ||
		 (HasUsableCompilationResult(pixelSpirvResult) && !HasGlslArtifactStage(glslFallbackArtifact, ShaderStage::Pixel)) ||
		 (HasUsableCompilationResult(computeSpirvResult) && !HasGlslArtifactStage(glslFallbackArtifact, ShaderStage::Compute))))
	{
		NLS_LOG_ERROR("[HLSL][OpenGL] \"" + __FILE_TRACE + "\": Failed to generate GLSL artifacts from SPIR-V.");
	}

	return shader;
}
}
