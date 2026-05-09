#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/RenderDef.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/ShaderCompiler/ShaderAsset.h"

namespace NLS::Render::ShaderCompiler
{
	struct NLS_RENDER_API ShaderReflectionInput
	{
		ShaderCompilationInput input;
		ShaderCompilationOutput compiledOutput;
	};

	struct NLS_RENDER_API ShaderCompilerToolchainIdentity
	{
		std::string compilerPath;
		std::string compilerVersion;
		std::string argumentSchemaVersion;
	};

	struct NLS_RENDER_API ShaderArtifactStagingPlan
	{
		std::string finalPath;
		std::string temporaryPath;
		std::string lockPath;
	};

	enum class NLS_RENDER_API ShaderProcessStatus : uint8_t
	{
		Succeeded,
		Failed,
		FailedToStart,
		TimedOut,
		Cancelled
	};

	struct NLS_RENDER_API ShaderProcessOptions
	{
		uint32_t timeoutMilliseconds = 60000u;
		const std::atomic_bool* cancellationFlag = nullptr;
	};

	struct NLS_RENDER_API ShaderProcessResult
	{
		ShaderProcessStatus status = ShaderProcessStatus::Failed;
		int exitCode = -1;
		std::string output;
		std::string diagnostics;
		std::string commandLine;
	};

	NLS_RENDER_API std::string BuildShaderCompilationCacheKey(
		const ShaderCompilationInput& input,
		std::string_view sourceDependencyFingerprint,
		const ShaderCompilerToolchainIdentity& toolchain);

	NLS_RENDER_API ShaderArtifactStagingPlan BuildShaderArtifactStagingPlan(
		std::string_view finalPath,
		std::string_view purpose);

	NLS_RENDER_API bool WriteShaderArtifactTextAtomic(
		std::string_view finalPath,
		std::string_view content,
		std::string* diagnostics = nullptr);

	NLS_RENDER_API ShaderProcessResult ExecuteShaderCompilerProcess(
		std::string_view executable,
		const std::vector<std::string>& arguments,
		const ShaderProcessOptions& options = {});

	class NLS_RENDER_API IShaderCompilerBackend
	{
	public:
		virtual ~IShaderCompilerBackend() = default;

		virtual ShaderCompilationOutput Compile(const ShaderCompilationInput& input) = 0;
		virtual Resources::ShaderReflection Reflect(const ShaderCompilationInput& input) = 0;
		virtual Resources::ShaderReflection Reflect(const ShaderCompilationInput& input, const ShaderCompilationOutput& compiledOutput) = 0;
		virtual const char* GetBackendName() const = 0;
	};

	class NLS_RENDER_API ShaderCompiler
	{
	public:
		ShaderCompiler();
		explicit ShaderCompiler(std::unique_ptr<IShaderCompilerBackend> backend);

		void SetBackend(std::unique_ptr<IShaderCompilerBackend> backend);
		const IShaderCompilerBackend* GetBackend() const;

		ShaderCompilationOutput Compile(const ShaderCompilationInput& input) const;
		std::vector<ShaderCompilationOutput> CompileBatch(const std::vector<ShaderCompilationInput>& inputs) const;
		ShaderCompilationOutput Compile(ShaderAsset& asset, const ShaderVariantKey& variantKey, const ShaderCompileOptions& options) const;
		Resources::ShaderReflection Reflect(const ShaderCompilationInput& input) const;
		Resources::ShaderReflection Reflect(const ShaderCompilationInput& input, const ShaderCompilationOutput& compiledOutput) const;
		std::vector<Resources::ShaderReflection> ReflectBatch(const std::vector<ShaderReflectionInput>& inputs) const;

	private:
		std::unique_ptr<IShaderCompilerBackend> m_backend;
	};
}
