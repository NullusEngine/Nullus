#include <algorithm>
#include <cctype>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

#include <Debug/Logger.h>

#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/ShaderReflectionMerge.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/ShaderType.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/ShaderCompiler/ShaderAsset.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"

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

	struct PreparedShaderSource
	{
		std::string compileFilePath;
		std::string sourceText;
		std::string vertexEntry = "VSMain";
		std::string pixelEntry = "PSMain";
		std::string computeEntry = "CSMain";
		std::optional<NLS::Render::ShaderLab::ShaderLabPassState> shaderLabPassState;
		std::vector<std::string> includeDirectories;
	};

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

	uint64_t StableFnv1a64(std::string_view value)
	{
		uint64_t hash = 14695981039346656037ull;
		for (const unsigned char character : value)
		{
			hash ^= static_cast<uint64_t>(character);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	std::string ToFixedHex16(const uint64_t value)
	{
		std::ostringstream output;
		output << std::hex << std::nouppercase;
		output.width(16);
		output.fill('0');
		output << value;
		return output.str();
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

	bool IsNativeShaderArtifactFile(const std::string& path)
	{
		return NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
			path,
			NLS::Core::Assets::ArtifactType::Shader,
			1u,
			0u,
			16u * 1024u).has_value();
	}

	void AddConfiguredBuiltInShaderRoots(
		std::vector<std::filesystem::path>& roots,
		const std::string& projectAssetsPath)
	{
		if (projectAssetsPath.empty())
			return;

		std::filesystem::path assetsPath(projectAssetsPath);
		if (assetsPath.is_relative())
			assetsPath = std::filesystem::absolute(assetsPath);

		std::error_code error;
		assetsPath = std::filesystem::weakly_canonical(assetsPath, error);
		if (error)
			assetsPath = std::filesystem::absolute(std::filesystem::path(projectAssetsPath)).lexically_normal();

		while (!assetsPath.empty() && assetsPath.filename().empty())
		{
			const auto parent = assetsPath.parent_path();
			if (parent == assetsPath)
				break;
			assetsPath = parent;
		}

		const auto projectRoot = PathNameEquals(assetsPath, kAssetsDirectoryName) ? assetsPath.parent_path() : assetsPath;
		const auto workspaceRoot = projectRoot.parent_path();
		if (workspaceRoot.empty())
			return;

		roots.emplace_back(workspaceRoot / kAppDirectoryName / kAssetsDirectoryName / kEngineDirectoryName / "Shaders");
		roots.emplace_back(workspaceRoot / kAppDirectoryName / kAssetsDirectoryName / kEditorDirectoryName / "Shaders");
	}

	bool IsAllowedBuiltInHlslSourcePath(
		const std::string& path,
		const std::string& projectAssetsPath = {})
	{
		const auto normalizedRequest = ToLowerAscii(std::filesystem::path(path).generic_string());
		if (normalizedRequest.rfind(":shaders/", 0u) == 0u)
			return true;

		auto canonicalPath = [](std::filesystem::path value)
		{
			std::error_code error;
			value = std::filesystem::weakly_canonical(value, error);
			if (error)
				value = std::filesystem::absolute(value).lexically_normal();
			return ToLowerAscii(value.generic_string());
		};
		auto isInsideOrEqual = [](const std::string& candidate, const std::string& root)
		{
			return candidate == root ||
				(candidate.size() > root.size() &&
					candidate.rfind(root, 0u) == 0u &&
					(candidate[root.size()] == '/' || candidate[root.size()] == '\\'));
		};

		const auto normalized = canonicalPath(path);
		std::vector<std::filesystem::path> roots =
		{
			std::filesystem::path(kAppDirectoryName) / kAssetsDirectoryName / kEngineDirectoryName / "Shaders",
			std::filesystem::path(kAppDirectoryName) / kAssetsDirectoryName / kEditorDirectoryName / "Shaders",
			std::filesystem::path("EngineAssets") / "Shaders"
		};
		AddConfiguredBuiltInShaderRoots(roots, projectAssetsPath);
		for (auto root : roots)
		{
			if (root.is_relative())
				root = std::filesystem::absolute(root);
			if (isInsideOrEqual(normalized, canonicalPath(root)))
				return true;
		}
		return false;
	}

	bool HasEntryPointToken(const std::string& source, std::string_view entryPoint)
	{
		return !source.empty() && source.find(std::string(entryPoint)) != std::string::npos;
	}

	PreparedShaderSource PrepareShaderSource(
		const std::string& filePath,
		const std::string& projectAssetsPath)
	{
		PreparedShaderSource prepared;
		prepared.compileFilePath = filePath;
		prepared.sourceText = ReadTextFile(filePath);

		const auto sourcePath = std::filesystem::path(filePath);
		const auto sourceParent = sourcePath.parent_path();
		if (!sourceParent.empty())
		{
			prepared.includeDirectories.push_back(sourceParent.string());
			if (!sourceParent.parent_path().empty())
				prepared.includeDirectories.push_back(sourceParent.parent_path().string());
		}

		return prepared;
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
	if (IsNativeShaderArtifactFile(p_filePath))
		return CreateImportedShaderArtifact(p_filePath);
	const auto extension = ToLowerAscii(std::filesystem::path(p_filePath).extension().generic_string());
	if (extension == ".shader" || extension == ".hlsl")
	{
		NLS_LOG_ERROR("[SHADER] \"" + p_filePath + "\": runtime source loading is disabled; load the imported artifact listed in ArtifactDB/manifest.");
		return nullptr;
	}
	NLS_LOG_ERROR("[SHADER] \"" + p_filePath + "\": unsupported runtime shader payload; expected a native imported shader artifact.");
	return nullptr;
}

Shader* ShaderLoader::Create(const std::string& p_filePath, const std::string& p_projectAssetsPath)
{
	__FILE_TRACE = p_filePath;
	if (IsNativeShaderArtifactFile(p_filePath))
		return CreateImportedShaderArtifact(p_filePath);
	const auto extension = ToLowerAscii(std::filesystem::path(p_filePath).extension().generic_string());
	if (extension == ".shader" || extension == ".hlsl")
	{
		NLS_LOG_ERROR("[SHADER] \"" + p_filePath + "\": runtime source loading is disabled; load the imported artifact listed in ArtifactDB/manifest.");
		return nullptr;
	}
	NLS_LOG_ERROR("[SHADER] \"" + p_filePath + "\": unsupported runtime shader payload; expected a native imported shader artifact.");
	return nullptr;
}

Shader* ShaderLoader::CreateBuiltInHlsl(const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;
	if (!IsAllowedBuiltInHlslSourcePath(p_filePath))
	{
		NLS_LOG_ERROR("[SHADER] \"" + p_filePath + "\": built-in HLSL loading only accepts engine/editor shader library paths; project shader sources must be imported artifacts.");
		return nullptr;
	}
	return CreateHLSLShaderAsset(p_filePath, ResolveProjectAssetsPath({}));
}

Shader* ShaderLoader::CreateBuiltInHlsl(const std::string& p_filePath, const std::string& p_projectAssetsPath)
{
	__FILE_TRACE = p_filePath;
	const auto resolvedProjectAssetsPath = ResolveProjectAssetsPath(p_projectAssetsPath);
	if (!IsAllowedBuiltInHlslSourcePath(p_filePath, resolvedProjectAssetsPath))
	{
		NLS_LOG_ERROR("[SHADER] \"" + p_filePath + "\": built-in HLSL loading only accepts engine/editor shader library paths; project shader sources must be imported artifacts.");
		return nullptr;
	}
	return CreateHLSLShaderAsset(p_filePath, resolvedProjectAssetsPath);
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
	p_destination.ReplaceRuntimeData(p_source.GetRuntimeDataSnapshot());
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

void ShaderLoader::RecompileBuiltInHlsl(Shader& p_shader, const std::string& p_filePath)
{
	__FILE_TRACE = p_filePath;

	if (Shader* refreshed = CreateBuiltInHlsl(p_filePath, ResolveProjectAssetsPath({})); refreshed != nullptr)
	{
		CopyRuntimeData(p_shader, *refreshed);
		delete refreshed;
		NLS_LOG_INFO("[COMPILE] \"" + __FILE_TRACE + "\": built-in shader refreshed.");
	}
	else
	{
		NLS_LOG_ERROR("[COMPILE] \"" + __FILE_TRACE + "\": Failed to refresh built-in shader.");
	}
}

void ShaderLoader::RecompileBuiltInHlsl(Shader& p_shader, const std::string& p_filePath, const std::string& p_projectAssetsPath)
{
	__FILE_TRACE = p_filePath;

	if (Shader* refreshed = CreateBuiltInHlsl(p_filePath, ResolveProjectAssetsPath(p_projectAssetsPath)); refreshed != nullptr)
	{
		CopyRuntimeData(p_shader, *refreshed);
		delete refreshed;
		NLS_LOG_INFO("[COMPILE] \"" + __FILE_TRACE + "\": built-in shader refreshed.");
	}
	else
	{
		NLS_LOG_ERROR("[COMPILE] \"" + __FILE_TRACE + "\": Failed to refresh built-in shader.");
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
		NLS::Render::Resources::Material::ClearShaderReferencesFromLiveMaterials(p_shader);
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

	std::vector<ShaderCompiledArtifact> compiledArtifacts;
	compiledArtifacts.reserve(artifact->stages.size());
	for (const auto& stage : artifact->stages)
	{
		if (stage.output.status != ShaderCompilationStatus::Succeeded || stage.output.bytecode.empty())
			continue;

		compiledArtifacts.push_back({
			stage.stage,
			stage.targetPlatform,
			stage.entryPoint,
			stage.targetProfile,
			stage.output,
			stage.keywordHash
		});
	}

	auto* shader = new Shader(p_filePath, ShaderSourceLanguage::HLSL);
	shader->SetRuntimeData(
		artifact->reflection,
		BuildRegisteredShaderParameterStructs(artifact->sourcePath),
		std::move(compiledArtifacts),
		artifact->sourcePath,
		artifact->subAssetKey,
		artifact->shaderLabLightMode,
		artifact->shaderLabPassState);
	return shader;
}

Shader* ShaderLoader::CreateHLSLShaderAsset(
	const std::string& p_filePath,
	const std::string& p_projectAssetsPath)
{
	NLS::Render::ShaderCompiler::ShaderCompiler compiler;
	compiler.SetCacheDatabasePath(GetCacheDatabasePath(p_filePath, p_projectAssetsPath));
	const auto prepared = PrepareShaderSource(p_filePath, p_projectAssetsPath);
	const auto& sourceText = prepared.sourceText;
	if (sourceText.empty())
	{
		NLS_LOG_ERROR("[HLSL] \"" + p_filePath + "\": shader source could not be read or is empty.");
	}
	const bool hasVertexEntryPoint = !prepared.vertexEntry.empty() && HasEntryPointToken(sourceText, prepared.vertexEntry);
	const bool hasPixelEntryPoint = !prepared.pixelEntry.empty() && HasEntryPointToken(sourceText, prepared.pixelEntry);
	const bool hasComputeEntryPoint = !prepared.computeEntry.empty() && HasEntryPointToken(sourceText, prepared.computeEntry);
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
	vertexDxilOptions.entryPoint = prepared.vertexEntry;
	vertexDxilOptions.targetProfile = "vs_6_0";
	vertexDxilOptions.includeDirectories = prepared.includeDirectories;

	ShaderCompileOptions pixelDxilOptions;
	pixelDxilOptions.sourceLanguage = ShaderSourceLanguage::HLSL;
	pixelDxilOptions.targetPlatform = ShaderTargetPlatform::DXIL;
	pixelDxilOptions.entryPoint = prepared.pixelEntry;
	pixelDxilOptions.targetProfile = "ps_6_0";
	pixelDxilOptions.includeDirectories = vertexDxilOptions.includeDirectories;

	ShaderCompileOptions vertexSpirvOptions = vertexDxilOptions;
	vertexSpirvOptions.targetPlatform = ShaderTargetPlatform::SPIRV;

	ShaderCompileOptions pixelSpirvOptions = pixelDxilOptions;
	pixelSpirvOptions.targetPlatform = ShaderTargetPlatform::SPIRV;

	ShaderCompileOptions computeDxilOptions = pixelDxilOptions;
	computeDxilOptions.entryPoint = prepared.computeEntry;
	computeDxilOptions.targetProfile = "cs_6_0";

	ShaderCompileOptions computeSpirvOptions = computeDxilOptions;
	computeSpirvOptions.targetPlatform = ShaderTargetPlatform::SPIRV;

	const ShaderCompilationInput vertexDxilInput{ prepared.compileFilePath, ShaderStage::Vertex, vertexDxilOptions };
	const ShaderCompilationInput pixelDxilInput{ prepared.compileFilePath, ShaderStage::Pixel, pixelDxilOptions };
	const ShaderCompilationInput computeDxilInput{ prepared.compileFilePath, ShaderStage::Compute, computeDxilOptions };
	const ShaderCompilationInput vertexSpirvInput{ prepared.compileFilePath, ShaderStage::Vertex, vertexSpirvOptions };
	const ShaderCompilationInput pixelSpirvInput{ prepared.compileFilePath, ShaderStage::Pixel, pixelSpirvOptions };
	const ShaderCompilationInput computeSpirvInput{ prepared.compileFilePath, ShaderStage::Compute, computeSpirvOptions };

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
	std::vector<ShaderReflectionInput> dxilReflectionInputs;
	if (HasUsableCompilationResult(vertexDxilResult))
		dxilReflectionInputs.push_back({ vertexDxilInput, vertexDxilResult });
	if (HasUsableCompilationResult(pixelDxilResult))
		dxilReflectionInputs.push_back({ pixelDxilInput, pixelDxilResult });
	if (HasUsableCompilationResult(computeDxilResult))
		dxilReflectionInputs.push_back({ computeDxilInput, computeDxilResult });

	std::vector<ShaderReflectionInput> spirvReflectionInputs;
	if (HasUsableCompilationResult(vertexSpirvResult))
		spirvReflectionInputs.push_back({ vertexSpirvInput, vertexSpirvResult });
	if (HasUsableCompilationResult(pixelSpirvResult))
		spirvReflectionInputs.push_back({ pixelSpirvInput, pixelSpirvResult });
	if (HasUsableCompilationResult(computeSpirvResult))
		spirvReflectionInputs.push_back({ computeSpirvInput, computeSpirvResult });

	ShaderReflection reflection;
	const auto dxilReflections = compiler.ReflectBatch(dxilReflectionInputs);
	const auto spirvReflections = compiler.ReflectBatch(spirvReflectionInputs);
	std::string reflectionDiagnostic;
	if (!NLS::Render::Resources::TryMergePreferredShaderReflectionOrFallback(
		dxilReflections,
		spirvReflections,
		reflection,
		&reflectionDiagnostic))
	{
		NLS_LOG_ERROR("[SHADER REFLECTION] \"" + p_filePath + "\": " + reflectionDiagnostic);
		reflection = {};
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

	std::vector<ShaderCompiledArtifact> compiledArtifacts;
	compiledArtifacts.reserve(9u);
	if (HasUsableCompilationResult(vertexDxilResult))
		compiledArtifacts.push_back({ ShaderStage::Vertex, ShaderTargetPlatform::DXIL, vertexDxilOptions.entryPoint, vertexDxilOptions.targetProfile, vertexDxilResult, 0u });
	if (HasUsableCompilationResult(pixelDxilResult))
		compiledArtifacts.push_back({ ShaderStage::Pixel, ShaderTargetPlatform::DXIL, pixelDxilOptions.entryPoint, pixelDxilOptions.targetProfile, pixelDxilResult, 0u });
	if (HasUsableCompilationResult(computeDxilResult))
		compiledArtifacts.push_back({ ShaderStage::Compute, ShaderTargetPlatform::DXIL, computeDxilOptions.entryPoint, computeDxilOptions.targetProfile, computeDxilResult, 0u });
	if (HasUsableCompilationResult(vertexSpirvResult))
		compiledArtifacts.push_back({ ShaderStage::Vertex, ShaderTargetPlatform::SPIRV, vertexSpirvOptions.entryPoint, vertexSpirvOptions.targetProfile, vertexSpirvResult, 0u });
	if (HasUsableCompilationResult(pixelSpirvResult))
		compiledArtifacts.push_back({ ShaderStage::Pixel, ShaderTargetPlatform::SPIRV, pixelSpirvOptions.entryPoint, pixelSpirvOptions.targetProfile, pixelSpirvResult, 0u });
	if (HasUsableCompilationResult(computeSpirvResult))
		compiledArtifacts.push_back({ ShaderStage::Compute, ShaderTargetPlatform::SPIRV, computeSpirvOptions.entryPoint, computeSpirvOptions.targetProfile, computeSpirvResult, 0u });
	for (const auto& stage : glslFallbackArtifact.stages)
	{
		if (stage.targetPlatform != ShaderTargetPlatform::GLSL ||
			stage.output.status != ShaderCompilationStatus::Succeeded ||
			stage.output.bytecode.empty())
		{
			continue;
		}

		compiledArtifacts.push_back({
			stage.stage,
			stage.targetPlatform,
			stage.entryPoint,
			stage.targetProfile,
			stage.output,
			0u
		});
	}
	auto* shader = new Shader(p_filePath, ShaderSourceLanguage::HLSL);
	shader->SetRuntimeData(
		std::move(reflection),
		BuildRegisteredShaderParameterStructs(p_filePath),
		std::move(compiledArtifacts),
		{},
		{},
		{},
		prepared.shaderLabPassState);

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
