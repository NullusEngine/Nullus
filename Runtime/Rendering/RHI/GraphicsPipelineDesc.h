#pragma once

#include "Rendering/RHI/RHITypes.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "Rendering/Settings/ECullFace.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "RenderDef.h"

#include <vector>

namespace NLS::Render::Resources
{
	struct ShaderReflection;
}

namespace NLS::Render::RHI
{
	struct NLS_RENDER_API DepthStencilStateDesc
	{
		bool depthTest = true;
		bool depthWrite = true;
		NLS::Render::Settings::EComparaisonAlgorithm depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS;
	};

	struct NLS_RENDER_API RasterStateDesc
	{
		bool culling = true;
		NLS::Render::Settings::ECullFace cullFace = NLS::Render::Settings::ECullFace::BACK;
	};

	struct NLS_RENDER_API BlendStateDesc
	{
		bool enabled = false;
		bool colorWrite = true;
	};

	struct NLS_RENDER_API ShaderStageDesc
	{
		ShaderStage stage = ShaderStage::Vertex;
		ShaderCompiler::ShaderTargetPlatform targetPlatform = ShaderCompiler::ShaderTargetPlatform::Unknown;
		std::string entryPoint = "main";
		std::vector<uint8_t> bytecode;
	};

	struct NLS_RENDER_API PipelineLayoutDesc
	{
		uint32_t uniformBufferBindingCount = 0;
		uint32_t sampledTextureBindingCount = 0;
		uint32_t samplerBindingCount = 0;
		uint32_t storageBufferBindingCount = 0;
	};

	struct NLS_RENDER_API AttachmentLayoutDesc
	{
		std::vector<TextureFormat> colorAttachmentFormats;
		TextureFormat depthAttachmentFormat = TextureFormat::Depth24Stencil8;
		uint32_t sampleCount = 1;
		bool hasDepthAttachment = true;
	};

	struct NLS_RENDER_API GraphicsPipelineDesc
	{
		std::vector<ShaderStageDesc> shaderStages;
		const NLS::Render::Resources::ShaderReflection* reflection = nullptr;
		PipelineLayoutDesc layout;
		AttachmentLayoutDesc attachmentLayout;
		RasterStateDesc rasterState;
		DepthStencilStateDesc depthStencilState;
		BlendStateDesc blendState;
		NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
		int gpuInstances = 1;
	};
}
