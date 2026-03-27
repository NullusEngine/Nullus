#pragma once

#include <memory>
#include <string>

#include "Rendering/RenderDef.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/ShaderCompiler/ShaderAsset.h"

namespace NLS::Render::ShaderCompiler
{
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
		ShaderCompilationOutput Compile(ShaderAsset& asset, const ShaderVariantKey& variantKey, const ShaderCompileOptions& options) const;
		Resources::ShaderReflection Reflect(const ShaderCompilationInput& input) const;
		Resources::ShaderReflection Reflect(const ShaderCompilationInput& input, const ShaderCompilationOutput& compiledOutput) const;

	private:
		std::unique_ptr<IShaderCompilerBackend> m_backend;
	};
}
