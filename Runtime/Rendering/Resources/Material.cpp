#include "Rendering/Resources/Material.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Base/Image.h"
#include "Debug/Logger.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Math/Matrix4.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIPipelineStateUtils.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/MaterialResourceSet.h"
#include "Rendering/Resources/ShaderBindingLayoutUtils.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

namespace
{
	using ShaderSourceLanguage = NLS::Render::ShaderCompiler::ShaderSourceLanguage;
	using ShaderResourceKind = NLS::Render::Resources::ShaderResourceKind;
	using UniformType = NLS::Render::Resources::UniformType;
	constexpr std::string_view kIndexedObjectDataDiagnosticBindingName = "ObjectIndexConstants";

	void ClearNonIndexedObjectDataBindingDiagnostics(
		std::vector<NLS::Render::Resources::MaterialBindingDiagnostic>& diagnostics)
	{
		diagnostics.erase(
			std::remove_if(
				diagnostics.begin(),
				diagnostics.end(),
				[](const NLS::Render::Resources::MaterialBindingDiagnostic& diagnostic)
				{
					return diagnostic.bindingName != kIndexedObjectDataDiagnosticBindingName;
				}),
			diagnostics.end());
	}

	struct ExplicitShaderCacheKey
	{
		uint64_t deviceIdentity = 0u;
		NLS::Render::RHI::NativeBackendType backend = NLS::Render::RHI::NativeBackendType::None;
		uint64_t shaderInstanceId = 0u;
		uint64_t shaderGeneration = 0u;

		bool operator==(const ExplicitShaderCacheKey& rhs) const = default;
	};

	struct ExplicitShaderCacheKeyHash
	{
		size_t operator()(const ExplicitShaderCacheKey& key) const
		{
			size_t hash = std::hash<uint64_t>{}(key.deviceIdentity);
			auto combine = [&hash](const size_t value)
			{
				hash ^= value + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
			};
			combine(std::hash<uint32_t>{}(static_cast<uint32_t>(key.backend)));
			combine(std::hash<uint64_t>{}(key.shaderInstanceId));
			combine(std::hash<uint64_t>{}(key.shaderGeneration));
			return hash;
		}
	};

	struct IndexedObjectDataBindingDiagnosticSource
	{
		uint64_t shaderInstanceId = 0u;
		uint64_t shaderGeneration = 0u;
		std::string message;
	};

	void RefreshIndexedObjectDataBindingDiagnostics(
		std::vector<NLS::Render::Resources::MaterialBindingDiagnostic>& diagnostics,
		const std::vector<IndexedObjectDataBindingDiagnosticSource>& sources)
	{
		diagnostics.erase(
			std::remove_if(
				diagnostics.begin(),
				diagnostics.end(),
				[](const NLS::Render::Resources::MaterialBindingDiagnostic& diagnostic)
				{
					return diagnostic.bindingName == kIndexedObjectDataDiagnosticBindingName;
				}),
			diagnostics.end());
		for (const auto& source : sources)
		{
			diagnostics.push_back({
				NLS::Render::Resources::MaterialBindingDiagnosticSeverity::Error,
				std::string(kIndexedObjectDataDiagnosticBindingName),
				source.message
			});
		}
	}

	ExplicitShaderCacheKey BuildExplicitShaderCacheKey(
		const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device,
		const NLS::Render::Resources::Shader* shader)
	{
		return {
			device != nullptr ? device->GetCacheIdentity() : 0u,
			device != nullptr ? device->GetNativeDeviceInfo().backend : NLS::Render::RHI::NativeBackendType::None,
			shader != nullptr ? shader->GetInstanceId() : 0u,
			shader != nullptr ? shader->GetGeneration() : 0u
		};
	}

	uint64_t NextMaterialInstanceId()
	{
		static std::atomic<uint64_t> nextInstanceId { 1u };
		auto instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
		if (instanceId == 0u)
			instanceId = nextInstanceId.fetch_add(1u, std::memory_order_relaxed);
		return instanceId;
	}

	std::mutex& LiveMaterialRegistryMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	std::unordered_set<NLS::Render::Resources::Material*>& LiveMaterialRegistry()
	{
		static std::unordered_set<NLS::Render::Resources::Material*> materials;
		return materials;
	}

	void RegisterLiveMaterial(NLS::Render::Resources::Material* material)
	{
		if (material == nullptr)
			return;

		std::lock_guard lock(LiveMaterialRegistryMutex());
		LiveMaterialRegistry().insert(material);
	}

	void UnregisterLiveMaterial(NLS::Render::Resources::Material* material)
	{
		if (material == nullptr)
			return;

		std::lock_guard lock(LiveMaterialRegistryMutex());
		LiveMaterialRegistry().erase(material);
	}

	NLS::Render::RHI::SamplerDesc BuildDefaultSamplerDesc(const std::string& bindingName)
	{
		auto toLower = bindingName;
		std::transform(toLower.begin(), toLower.end(), toLower.begin(), [](unsigned char value)
		{
			return static_cast<char>(std::tolower(value));
		});

		NLS::Render::RHI::SamplerDesc desc{};
		desc.minFilter = toLower.find("point") != std::string::npos
			? NLS::Render::RHI::TextureFilter::Nearest
			: NLS::Render::RHI::TextureFilter::Linear;
		desc.magFilter = desc.minFilter;

		const auto wrapMode = toLower.find("clamp") != std::string::npos
			? NLS::Render::RHI::TextureWrap::ClampToEdge
			: NLS::Render::RHI::TextureWrap::Repeat;
		desc.wrapU = wrapMode;
		desc.wrapV = wrapMode;
		desc.wrapW = wrapMode;
		return desc;
	}

	std::shared_ptr<NLS::Render::RHI::RHIDevice> TryGetLocatedExplicitDevice()
	{
		auto* driver = NLS::Render::Context::TryGetLocatedDriver();
		return driver != nullptr
			? NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(*driver)
			: nullptr;
	}

	NLS::Render::RHI::NativeBackendType ResolveDeviceBackend(
		const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
	{
		if (device == nullptr || device->GetAdapter() == nullptr)
			return NLS::Render::RHI::NativeBackendType::None;
		return device->GetAdapter()->GetBackendType();
	}

	NLS::Render::Resources::Texture2D* CreateDefaultWhiteTexture2D()
	{
		NLS::Render::Assets::TextureArtifactData artifact;
		artifact.width = 1u;
		artifact.height = 1u;
		artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
		artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Linear;
		artifact.mips.push_back({
			0u,
			1u,
			1u,
			NLS::Render::RHI::CalculateTextureRowPitch(NLS::Render::RHI::TextureFormat::RGBA8, 1u),
			NLS::Render::RHI::CalculateTextureSlicePitch(NLS::Render::RHI::TextureFormat::RGBA8, 1u, 1u, 1u),
			std::vector<uint8_t> {255u, 255u, 255u, 255u}
		});

		auto* created = NLS::Render::Resources::Loaders::TextureLoader::CreateFromArtifact(
			artifact,
			NLS::Render::Settings::ETextureFilteringMode::NEAREST,
			NLS::Render::Settings::ETextureFilteringMode::NEAREST,
			false);
		if (created != nullptr)
			created->path = ":Generated/DefaultWhiteTexture";
		return created;
	}

	NLS::Render::Resources::Texture2D* GetDefaultWhiteTexture2D()
	{
		static NLS::Render::Resources::Texture2D* texture = nullptr;
		static uint64_t textureDeviceIdentity = 0u;
		static NLS::Render::RHI::NativeBackendType textureBackend = NLS::Render::RHI::NativeBackendType::None;

		const auto device = TryGetLocatedExplicitDevice();
		const auto deviceIdentity = device != nullptr ? device->GetCacheIdentity() : 0u;
		const auto backend = ResolveDeviceBackend(device);
		const bool needsRefresh =
			texture == nullptr ||
			textureDeviceIdentity != deviceIdentity ||
			textureBackend != backend ||
			(device != nullptr && texture->GetTextureHandle() == nullptr) ||
			(device == nullptr && texture->GetTextureHandle() != nullptr);
		if (needsRefresh)
		{
			NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture);
			texture = CreateDefaultWhiteTexture2D();
			textureDeviceIdentity = texture != nullptr ? deviceIdentity : 0u;
			textureBackend = texture != nullptr ? backend : NLS::Render::RHI::NativeBackendType::None;
		}
		return texture;
	}

	NLS::Render::Resources::TextureCube* CreateDefaultWhiteTextureCube()
	{
		auto* cubeMap = new NLS::Render::Resources::TextureCube();
		std::vector<uint8_t> whitePixel(4, 255);
		std::vector<NLS::Image> images;
		images.reserve(6);
		for (int i = 0; i < 6; ++i)
		{
			images.emplace_back(1, 1, 4);
			images.back().SetData(whitePixel.data());
		}
		std::vector<const NLS::Image*> imagePtrs;
		imagePtrs.reserve(6);
		for (auto& img : images)
			imagePtrs.push_back(&img);
		if (!cubeMap->SetTextureResource(imagePtrs))
		{
			delete cubeMap;
			return nullptr;
		}
		return cubeMap;
	}

	NLS::Render::Resources::TextureCube* GetDefaultWhiteTextureCube()
	{
		static NLS::Render::Resources::TextureCube* texture = nullptr;
		static uint64_t textureDeviceIdentity = 0u;
		static NLS::Render::RHI::NativeBackendType textureBackend = NLS::Render::RHI::NativeBackendType::None;

		const auto device = TryGetLocatedExplicitDevice();
		const auto deviceIdentity = device != nullptr ? device->GetCacheIdentity() : 0u;
		const auto backend = ResolveDeviceBackend(device);
		const bool needsRefresh =
			texture == nullptr ||
			textureDeviceIdentity != deviceIdentity ||
			textureBackend != backend ||
			(device != nullptr && texture->GetTextureHandle() == nullptr) ||
			(device == nullptr && texture->GetTextureHandle() != nullptr);
		if (needsRefresh)
		{
			delete texture;
			texture = CreateDefaultWhiteTextureCube();
			textureDeviceIdentity = texture != nullptr ? deviceIdentity : 0u;
			textureBackend = texture != nullptr ? backend : NLS::Render::RHI::NativeBackendType::None;
		}
		return texture;
	}

	std::any CreateDefaultMaterialValue(UniformType type)
	{
		switch (type)
		{
		case UniformType::UNIFORM_BOOL: return std::make_any<bool>(false);
		case UniformType::UNIFORM_INT: return std::make_any<int>(0);
		case UniformType::UNIFORM_FLOAT: return std::make_any<float>(0.0f);
		case UniformType::UNIFORM_FLOAT_VEC2: return std::make_any<NLS::Maths::Vector2>(NLS::Maths::Vector2::Zero);
		case UniformType::UNIFORM_FLOAT_VEC3: return std::make_any<NLS::Maths::Vector3>(NLS::Maths::Vector3::Zero);
		case UniformType::UNIFORM_FLOAT_VEC4: return std::make_any<NLS::Maths::Vector4>(NLS::Maths::Vector4::Zero);
		case UniformType::UNIFORM_FLOAT_MAT4: return std::make_any<NLS::Maths::Matrix4>(NLS::Maths::Matrix4::Identity);
		case UniformType::UNIFORM_SAMPLER_2D: return std::make_any<NLS::Render::Resources::Texture2D*>(nullptr);
		case UniformType::UNIFORM_SAMPLER_CUBE: return std::make_any<NLS::Render::Resources::TextureCube*>(nullptr);
		default: return {};
		}
	}

	bool ShouldExposeToMaterial(const NLS::Render::Resources::ShaderPropertyDesc& property)
	{
		return property.bindingSpace == NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace &&
			(property.kind == ShaderResourceKind::Value ||
			 property.kind == ShaderResourceKind::SampledTexture ||
			 property.kind == ShaderResourceKind::Sampler);
	}

	bool IsMaterialTextureBindingName(const std::string& name)
	{
		return name.size() >= 3u && name.rfind("Map") == name.size() - 3u;
	}

	std::string NormalizeShaderLabSourceIdentity(std::string sourcePath)
	{
		std::replace(sourcePath.begin(), sourcePath.end(), '\\', '/');
		return std::filesystem::path(sourcePath).lexically_normal().generic_string();
	}

	bool ShaderLabSourceIdentityMatches(const std::string& lhs, const std::string& rhs)
	{
		return NormalizeShaderLabSourceIdentity(lhs) == NormalizeShaderLabSourceIdentity(rhs);
	}

	NLS::Render::Resources::ResourceBindingLayout BuildMaterialBindingLayoutForShader(
		const NLS::Render::Resources::Shader* shader)
	{
		NLS::Render::Resources::ResourceBindingLayout layout;
		if (shader == nullptr)
			return layout;

		const auto reflection = shader->GetReflectionSnapshot();
		for (const auto& constantBuffer : reflection->constantBuffers)
		{
			if (constantBuffer.bindingSpace != NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace)
				continue;

			layout.bindings.push_back({
				constantBuffer.name,
				NLS::Render::Resources::ShaderResourceKind::UniformBuffer,
				NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex
			});
		}

		for (const auto& property : reflection->properties)
		{
			if (!ShouldExposeToMaterial(property) ||
				(property.kind != NLS::Render::Resources::ShaderResourceKind::SampledTexture &&
				 property.kind != NLS::Render::Resources::ShaderResourceKind::Sampler))
			{
				continue;
			}

			layout.bindings.push_back({
				property.name,
				property.kind,
				property.type,
				property.bindingSpace,
				property.bindingIndex
			});
		}

		return layout;
	}

	void FillMissingMaterialDefaultsFromShader(
		NLS::Render::Resources::MaterialParameterBlock& parameterBlock,
		const NLS::Render::Resources::Shader* shader)
	{
		if (shader == nullptr)
			return;

		const auto reflection = shader->GetReflectionSnapshot();
		for (const auto& property : reflection->properties)
		{
			if (!ShouldExposeToMaterial(property) || parameterBlock.Contains(property.name))
				continue;

			if (const auto uniformInfo = shader->GetUniformInfo(property.name); uniformInfo.has_value())
			{
				parameterBlock.Set(property.name, uniformInfo->defaultValue);
				continue;
			}

			if (const auto defaultValue = CreateDefaultMaterialValue(property.type); defaultValue.has_value())
				parameterBlock.Set(property.name, defaultValue);
		}
	}

	size_t ResolveRenderTargetCount(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc)
	{
		return std::max<size_t>(1u, desc.renderTargetLayout.colorFormats.size());
	}

	NLS::Render::RHI::RHIRenderTargetBlendStateDesc MakeNoWriteRenderTargetBlendState()
	{
		NLS::Render::RHI::RHIRenderTargetBlendStateDesc target;
		target.blendEnable = false;
		target.colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::None;
		return target;
	}

	void NormalizeRenderTargetBlendStateCount(
		NLS::Render::RHI::RHIGraphicsPipelineDesc& desc,
		const NLS::Render::RHI::RHIRenderTargetBlendStateDesc& fillTarget)
	{
		const auto renderTargetCount = ResolveRenderTargetCount(desc);
		if (desc.blendState.renderTargets.size() > renderTargetCount)
		{
			desc.blendState.renderTargets.resize(renderTargetCount);
			return;
		}

		while (desc.blendState.renderTargets.size() < renderTargetCount)
			desc.blendState.renderTargets.push_back(fillTarget);
	}

	void ApplyPipelineStateOverrides(
		NLS::Render::RHI::RHIGraphicsPipelineDesc& desc,
		const NLS::Render::Resources::MaterialPipelineStateOverrides& overrides)
	{
		const auto colorFormats = overrides.GetColorFormats();
			if (overrides.HasColorFormatsOverride())
			{
				desc.renderTargetLayout.colorFormats.assign(
					colorFormats.begin(),
					colorFormats.end());
				if (!overrides.HasColorSpacesOverride())
					desc.renderTargetLayout.colorSpaces.clear();
			const auto templateTarget = desc.blendState.renderTargets.empty()
				? NLS::Render::RHI::RHIRenderTargetBlendStateDesc{}
				: desc.blendState.renderTargets.front();
				desc.blendState.renderTargets.assign(ResolveRenderTargetCount(desc), templateTarget);
			}
			const auto colorSpaces = overrides.GetColorSpaces();
			if (overrides.HasColorSpacesOverride())
			{
				desc.renderTargetLayout.colorSpaces.assign(
					colorSpaces.begin(),
					colorSpaces.end());
			}
		if (overrides.colorWrite.has_value())
		{
			desc.blendState.colorWrite = *overrides.colorWrite;
			const auto writeMask = *overrides.colorWrite
				? NLS::Render::RHI::RHIColorWriteMask::All
				: NLS::Render::RHI::RHIColorWriteMask::None;
			if (desc.blendState.renderTargets.empty())
				desc.blendState.renderTargets.resize(ResolveRenderTargetCount(desc));
			for (auto& target : desc.blendState.renderTargets)
				target.colorWriteMask = writeMask;
		}
		if (overrides.blending.has_value())
		{
			desc.blendState.enabled = *overrides.blending;
			if (desc.blendState.renderTargets.empty())
				desc.blendState.renderTargets.resize(ResolveRenderTargetCount(desc));
			for (auto& target : desc.blendState.renderTargets)
				target.blendEnable = *overrides.blending;
		}
		if (overrides.HasRenderTargetBlendStatesOverride())
		{
			const auto renderTargetBlendStates = overrides.GetRenderTargetBlendStates();
			desc.blendState.independentBlendEnable = true;
			desc.blendState.renderTargets.assign(
				renderTargetBlendStates.begin(),
				renderTargetBlendStates.end());
			NormalizeRenderTargetBlendStateCount(desc, MakeNoWriteRenderTargetBlendState());
			desc.blendState.enabled = std::any_of(
				desc.blendState.renderTargets.begin(),
				desc.blendState.renderTargets.end(),
				[](const NLS::Render::RHI::RHIRenderTargetBlendStateDesc& target)
				{
					return target.blendEnable;
				});
			desc.blendState.colorWrite = std::any_of(
				desc.blendState.renderTargets.begin(),
				desc.blendState.renderTargets.end(),
				[](const NLS::Render::RHI::RHIRenderTargetBlendStateDesc& target)
				{
					return target.colorWriteMask != NLS::Render::RHI::RHIColorWriteMask::None;
				});
		}
		if (overrides.alphaToCoverage.has_value())
			desc.blendState.alphaToCoverageEnable = *overrides.alphaToCoverage;
		if (overrides.independentBlend.has_value())
			desc.blendState.independentBlendEnable = *overrides.independentBlend;
		if (overrides.depthWrite.has_value())
			desc.depthStencilState.depthWrite = *overrides.depthWrite;
		if (overrides.depthTest.has_value())
			desc.depthStencilState.depthTest = *overrides.depthTest;
		if (overrides.depthCompare.has_value())
			desc.depthStencilState.depthCompare = *overrides.depthCompare;
		if (overrides.stencilTest.has_value())
			desc.depthStencilState.stencilTest = *overrides.stencilTest;
		if (overrides.stencilWriteMask.has_value())
			desc.depthStencilState.stencilWriteMask = *overrides.stencilWriteMask;
		if (overrides.hasDepthAttachment.has_value())
			desc.renderTargetLayout.hasDepth = *overrides.hasDepthAttachment;
		if (overrides.depthFormat.has_value())
			desc.renderTargetLayout.depthFormat = *overrides.depthFormat;
		if (overrides.sampleCount.has_value())
			desc.renderTargetLayout.sampleCount = std::max(1u, *overrides.sampleCount);
		if (overrides.culling.has_value())
			desc.rasterState.cullEnabled = *overrides.culling;
		if (overrides.cullFace.has_value())
			desc.rasterState.cullFace = *overrides.cullFace;

		if (!desc.depthStencilState.depthTest &&
			!desc.depthStencilState.depthWrite &&
			!desc.depthStencilState.stencilTest)
		{
			desc.renderTargetLayout.hasDepth = false;
		}
	}

    bool ShouldLogMaterialBindingDiagnostics()
    {
        return NLS::Render::Settings::GetThreadDiagnosticsSettings().logMaterialBindings;
    }

    NLS::Render::RHI::NativeBackendType ResolveDeviceBackendType(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
    {
        if (device == nullptr)
            return NLS::Render::RHI::NativeBackendType::None;

        const auto& adapter = device->GetAdapter();
        return adapter != nullptr
            ? adapter->GetBackendType()
            : NLS::Render::RHI::NativeBackendType::None;
    }

    uint64_t ResolveDeviceCacheIdentity(
        const std::shared_ptr<NLS::Render::RHI::RHIDevice>& device)
    {
        return device != nullptr ? device->GetCacheIdentity() : 0u;
    }

	template<typename T>
	void CopyValueBytes(uint8_t* destination, uint32_t destinationSize, const T& value)
	{
		const auto bytesToCopy = std::min<uint32_t>(destinationSize, static_cast<uint32_t>(sizeof(T)));
		std::memcpy(destination, &value, bytesToCopy);
	}

	bool CopyParameterValueToBuffer(const std::any& value, UniformType type, uint8_t* destination, uint32_t destinationSize)
	{
		switch (type)
		{
		case UniformType::UNIFORM_BOOL:
		case UniformType::UNIFORM_INT:
		{
			int32_t encodedValue = 0;
			if (value.type() == typeid(bool))
				encodedValue = std::any_cast<bool>(value) ? 1 : 0;
			else if (value.type() == typeid(int))
				encodedValue = std::any_cast<int>(value);
			else
				return false;

			CopyValueBytes(destination, destinationSize, encodedValue);
			return true;
		}
		case UniformType::UNIFORM_FLOAT:
			if (value.type() == typeid(float))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<float>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_VEC2:
			if (value.type() == typeid(NLS::Maths::Vector2))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Vector2>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_VEC3:
			if (value.type() == typeid(NLS::Maths::Vector3))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Vector3>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_VEC4:
			if (value.type() == typeid(NLS::Maths::Vector4))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Vector4>(value));
				return true;
			}
			return false;
		case UniformType::UNIFORM_FLOAT_MAT4:
			if (value.type() == typeid(NLS::Maths::Matrix4))
			{
				CopyValueBytes(destination, destinationSize, std::any_cast<NLS::Maths::Matrix4>(value));
				return true;
			}
			return false;
		default:
			return false;
		}
	}

	struct MaterialConstantBufferBuildResult
	{
		std::vector<uint8_t> data;
		std::vector<NLS::Render::Resources::MaterialBindingDiagnostic> diagnostics;
		bool success = true;
	};

	MaterialConstantBufferBuildResult BuildMaterialConstantBufferData(
		const NLS::Render::Resources::ShaderConstantBufferDesc& constantBuffer,
		const NLS::Render::Resources::MaterialParameterBlock& parameterBlock)
	{
		MaterialConstantBufferBuildResult result;
		result.data.resize(constantBuffer.byteSize, 0u);

		auto addError = [&result, &constantBuffer](const std::string& memberName, std::string message)
		{
			result.success = false;
			result.diagnostics.push_back({
				NLS::Render::Resources::MaterialBindingDiagnosticSeverity::Error,
				constantBuffer.name,
				std::move(message)
			});
			if (!memberName.empty() &&
				result.diagnostics.back().message.find(memberName) == std::string::npos)
			{
				result.diagnostics.back().message += " Member: \"" + memberName + "\".";
			}
		};

		for (const auto& member : constantBuffer.members)
		{
			if (member.byteOffset > result.data.size() ||
				member.byteSize > result.data.size() - member.byteOffset)
			{
				addError(
					member.name,
					"Material constant buffer \"" + constantBuffer.name +
					"\" member \"" + member.name +
					"\" is outside the reflected buffer size.");
				continue;
			}

			const auto* parameter = parameterBlock.TryGet(member.name);
			if (parameter == nullptr)
			{
				addError(
					member.name,
					"Material constant buffer \"" + constantBuffer.name +
					"\" is missing value for member \"" + member.name + "\".");
				continue;
			}

			if (!CopyParameterValueToBuffer(
				*parameter,
				member.type,
				result.data.data() + member.byteOffset,
				member.byteSize))
			{
				addError(
					member.name,
					"Material constant buffer \"" + constantBuffer.name +
					"\" cannot copy value for member \"" + member.name +
					"\" because the material parameter type does not match reflection.");
			}
		}

		return result;
	}

	NLS::Render::RHI::ShaderStageMask ToShaderStageMask(const NLS::Render::ShaderCompiler::ShaderStage stage)
	{
		switch (stage)
		{
		case NLS::Render::ShaderCompiler::ShaderStage::Vertex: return NLS::Render::RHI::ShaderStageMask::Vertex;
		case NLS::Render::ShaderCompiler::ShaderStage::Compute: return NLS::Render::RHI::ShaderStageMask::Compute;
		case NLS::Render::ShaderCompiler::ShaderStage::Pixel:
		default:
			return NLS::Render::RHI::ShaderStageMask::Fragment;
		}
	}

	NLS::Render::RHI::BindingType ToBindingType(const ShaderResourceKind kind)
	{
		switch (kind)
		{
		case ShaderResourceKind::UniformBuffer: return NLS::Render::RHI::BindingType::UniformBuffer;
		case ShaderResourceKind::StructuredBuffer: return NLS::Render::RHI::BindingType::StructuredBuffer;
		case ShaderResourceKind::StorageBuffer: return NLS::Render::RHI::BindingType::StorageBuffer;
		case ShaderResourceKind::SampledTexture: return NLS::Render::RHI::BindingType::Texture;
		case ShaderResourceKind::Sampler:
		default:
			return NLS::Render::RHI::BindingType::Sampler;
		}
	}

	void UpsertBindingLayoutEntry(
		NLS::Render::RHI::RHIBindingLayoutDesc& layoutDesc,
		const std::string& name,
		const NLS::Render::RHI::BindingType bindingType,
		const uint32_t setIndex,
		const uint32_t bindingIndex,
		const uint32_t count,
		const NLS::Render::RHI::ShaderStageMask stageMask,
		const uint32_t elementStride = 0u)
	{
		auto existingEntry = std::find_if(
			layoutDesc.entries.begin(),
			layoutDesc.entries.end(),
			[&](const NLS::Render::RHI::RHIBindingLayoutEntry& candidate)
			{
				return candidate.set == setIndex &&
					candidate.binding == bindingIndex &&
					candidate.type == bindingType;
			});
		if (existingEntry != layoutDesc.entries.end())
		{
			existingEntry->stageMask = existingEntry->stageMask | stageMask;
			existingEntry->count = std::max(existingEntry->count, count);
			existingEntry->elementStride = std::max(existingEntry->elementStride, elementStride);
			if (existingEntry->name.empty())
				existingEntry->name = name;
			return;
		}

		NLS::Render::RHI::RHIBindingLayoutEntry entry;
		entry.name = name;
		entry.type = bindingType;
		entry.set = setIndex;
		entry.binding = bindingIndex;
		entry.count = std::max(1u, count);
		entry.stageMask = stageMask;
		entry.elementStride = elementStride;
		layoutDesc.entries.push_back(std::move(entry));
	}

	uint32_t ResolveResourceElementStride(
		const NLS::Render::RHI::BindingType bindingType,
		const uint32_t reflectedByteSize)
	{
		if (bindingType != NLS::Render::RHI::BindingType::StructuredBuffer &&
			bindingType != NLS::Render::RHI::BindingType::StorageBuffer)
		{
			return 0u;
		}

		return reflectedByteSize != 0u ? reflectedByteSize : sizeof(uint32_t);
	}

	std::vector<NLS::Render::RHI::RHIBindingLayoutDesc> BuildExplicitBindingLayoutDescs(
		const NLS::Render::Resources::Shader& shader,
		std::string_view debugNamePrefix)
	{
		if (shader.HasParameterStructs())
			return NLS::Render::Resources::BuildBindingLayoutDescsFromShaderParameters(
				shader.GetParameterStructs(),
				debugNamePrefix);

		return NLS::Render::Resources::BuildExplicitBindingLayoutDescsBySet(
			*shader.GetReflectionSnapshot(),
			debugNamePrefix);
	}

}

namespace NLS::Render::Resources
{
	bool MaterialIdentitySuggestsDecal(
		const std::string_view displayName,
		const std::string_view sourceSubAsset)
	{
		const auto hasDecalToken = [](const std::string_view value)
		{
			constexpr std::string_view kDecal = "decal";
			const auto isAlpha = [](const char character)
			{
				return std::isalpha(static_cast<unsigned char>(character)) != 0;
			};
			const auto isLower = [](const char character)
			{
				return std::islower(static_cast<unsigned char>(character)) != 0;
			};
			const auto isUpper = [](const char character)
			{
				return std::isupper(static_cast<unsigned char>(character)) != 0;
			};
			const auto matchesDecalAt = [kDecal](const std::string_view candidate, const size_t position)
			{
				if (position + kDecal.size() > candidate.size())
					return false;
				for (size_t index = 0u; index < kDecal.size(); ++index)
				{
					const auto character = static_cast<unsigned char>(candidate[position + index]);
					if (static_cast<char>(std::tolower(character)) != kDecal[index])
						return false;
				}
				return true;
			};

			size_t searchFrom = 0u;
			while (searchFrom < value.size())
			{
				size_t position = std::string_view::npos;
				for (size_t index = searchFrom; index + kDecal.size() <= value.size(); ++index)
				{
					if (matchesDecalAt(value, index))
					{
						position = index;
						break;
					}
				}
				if (position == std::string_view::npos)
					break;

				const size_t after = position + kDecal.size();
				const bool beginsToken = position == 0u ||
					!isAlpha(value[position - 1u]) ||
					(isLower(value[position - 1u]) && isUpper(value[position])) ||
					(isUpper(value[position]) && position + 1u < value.size() && isLower(value[position + 1u]));
				const bool endsToken = after == value.size() ||
					!isAlpha(value[after]) ||
					(isLower(value[after - 1u]) && isUpper(value[after]));
				if (beginsToken && endsToken)
					return true;

				searchFrom = position + 1u;
			}
			return false;
		};

		return !displayName.empty()
			? hasDecalToken(displayName)
			: hasDecalToken(sourceSubAsset);
	}

	const char* MaterialSurfaceModeName(const MaterialSurfaceMode mode)
	{
		switch (mode)
		{
		case MaterialSurfaceMode::Transparent: return "Transparent";
		case MaterialSurfaceMode::Decal: return "Decal";
		case MaterialSurfaceMode::Opaque:
		default: return "Opaque";
		}
	}

	std::optional<MaterialSurfaceMode> ParseMaterialSurfaceMode(const std::string& value)
	{
		std::string lowered = value;
		std::transform(
			lowered.begin(),
			lowered.end(),
			lowered.begin(),
			[](const unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		if (lowered == "opaque")
			return MaterialSurfaceMode::Opaque;
		if (lowered == "transparent" || lowered == "blend")
			return MaterialSurfaceMode::Transparent;
		if (lowered == "decal")
			return MaterialSurfaceMode::Decal;
		return std::nullopt;
	}

	size_t MaterialPipelineStateOverrides::GetHash() const
	{
		auto hashCombine = [](size_t& seed, const auto& value)
		{
			seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) +
				0x9e3779b97f4a7c15ull +
				(seed << 6) +
				(seed >> 2);
		};

		auto hashOptionalBool = [&hashCombine](size_t& seed, const std::optional<bool>& value)
		{
			hashCombine(seed, value.has_value());
			if (value.has_value())
				hashCombine(seed, *value);
		};

		auto hashOptionalUInt = [&hashCombine](size_t& seed, const std::optional<uint32_t>& value)
		{
			hashCombine(seed, value.has_value());
			if (value.has_value())
				hashCombine(seed, *value);
		};

		auto hashRenderTarget = [&hashCombine](size_t& seed, const RHI::RHIRenderTargetBlendStateDesc& target)
		{
			hashCombine(seed, target.blendEnable);
			hashCombine(seed, static_cast<uint32_t>(target.srcColor));
			hashCombine(seed, static_cast<uint32_t>(target.dstColor));
			hashCombine(seed, static_cast<uint32_t>(target.colorOp));
			hashCombine(seed, static_cast<uint32_t>(target.srcAlpha));
			hashCombine(seed, static_cast<uint32_t>(target.dstAlpha));
			hashCombine(seed, static_cast<uint32_t>(target.alphaOp));
			hashCombine(seed, static_cast<uint32_t>(target.colorWriteMask));
		};

		size_t seed = 0u;
		hashOptionalBool(seed, depthWrite);
		hashOptionalBool(seed, colorWrite);
		hashOptionalBool(seed, blending);
		hashOptionalBool(seed, depthTest);
		hashOptionalBool(seed, hasDepthAttachment);
		hashOptionalBool(seed, culling);
		hashOptionalBool(seed, stencilTest);
		hashOptionalBool(seed, alphaToCoverage);
		hashOptionalBool(seed, independentBlend);
		hashOptionalUInt(seed, stencilWriteMask);
		hashOptionalUInt(seed, sampleCount);
		hashCombine(seed, depthCompare.has_value());
		if (depthCompare.has_value())
			hashCombine(seed, static_cast<uint32_t>(*depthCompare));
		hashCombine(seed, cullFace.has_value());
		if (cullFace.has_value())
			hashCombine(seed, static_cast<uint32_t>(*cullFace));
		hashCombine(seed, depthFormat.has_value());
		if (depthFormat.has_value())
			hashCombine(seed, static_cast<uint32_t>(*depthFormat));
			hashCombine(seed, HasColorFormatsOverride());
		const auto formats = GetColorFormats();
		hashCombine(seed, formats.size());
			for (const auto format : formats)
				hashCombine(seed, static_cast<uint32_t>(format));
			hashCombine(seed, HasColorSpacesOverride());
			const auto spaces = GetColorSpaces();
			hashCombine(seed, spaces.size());
			for (const auto space : spaces)
				hashCombine(seed, static_cast<uint32_t>(space));
		hashCombine(seed, HasRenderTargetBlendStatesOverride());
		const auto renderTargetBlendStates = GetRenderTargetBlendStates();
		hashCombine(seed, renderTargetBlendStates.size());
		for (const auto& target : renderTargetBlendStates)
			hashRenderTarget(seed, target);
		return seed;
	}

	MaterialPipelineStateOverrides BuildMaterialPipelineStateOverrides(
		const NLS::Render::ShaderLab::ShaderLabPassState& state)
	{
		MaterialPipelineStateOverrides overrides;
		overrides.depthWrite = state.depthWrite;
		overrides.depthTest = state.depthCompare != Settings::EComparaisonAlgorithm::ALWAYS || state.depthWrite;
		overrides.depthCompare = state.depthCompare;
		const auto rasterState = NLS::Render::ShaderLab::ToRhiRasterState(state.cullMode);
		overrides.culling = rasterState.cullEnabled;
		overrides.cullFace = rasterState.cullFace;
		overrides.blending = state.blend.enabled;
		overrides.colorWrite = state.blend.colorWrite;
		overrides.alphaToCoverage = state.blend.alphaToCoverageEnable;
		overrides.independentBlend = state.blend.independentBlendEnable;
		if (!state.blend.renderTargets.empty())
			overrides.SetRenderTargetBlendStates(state.blend.renderTargets);
		return overrides;
	}

	struct Material::MaterialRuntimeState
	{
		struct MaterialConstantBufferState
		{
			std::unique_ptr<NLS::Render::Buffers::UniformBuffer> buffer;
			uint32_t size = 0;
		};

		MaterialResourceSet bindingSet;
		std::map<std::string, MaterialConstantBufferState> materialConstantBuffers;
		std::shared_ptr<RHI::RHIBindingLayout> explicitBindingLayout;
		std::shared_ptr<RHI::RHIBindingSet> explicitBindingSet;
		std::shared_ptr<RHI::RHIPipelineLayout> explicitPipelineLayout;
		std::unordered_map<ExplicitShaderCacheKey, std::shared_ptr<RHI::RHIBindingLayout>, ExplicitShaderCacheKeyHash>
			explicitBindingLayoutsByShaderKey;
		std::unordered_map<ExplicitShaderCacheKey, std::shared_ptr<RHI::RHIBindingSet>, ExplicitShaderCacheKeyHash>
			explicitBindingSetsByShaderKey;
		std::unordered_map<ExplicitShaderCacheKey, std::shared_ptr<RHI::RHIPipelineLayout>, ExplicitShaderCacheKeyHash>
			explicitPipelineLayoutsByShaderKey;
		std::unordered_map<uint64_t, std::pair<uint64_t, bool>> passDescriptorSetRequirementsByShader;
		std::vector<MaterialBindingDiagnostic> explicitBindingDiagnostics;
		std::vector<IndexedObjectDataBindingDiagnosticSource> indexedObjectDataBindingDiagnosticSources;
		std::vector<MaterialBindingDiagnostic> materialConstantBufferDiagnostics;
        uint64_t explicitBindingLayoutDeviceIdentity = 0u;
        uint64_t explicitBindingSetDeviceIdentity = 0u;
        uint64_t explicitPipelineLayoutDeviceIdentity = 0u;
		RHI::NativeBackendType explicitBindingLayoutBackend = RHI::NativeBackendType::None;
		RHI::NativeBackendType explicitBindingSetBackend = RHI::NativeBackendType::None;
		RHI::NativeBackendType explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		uint64_t explicitBindingLayoutShaderInstanceId = 0u;
		uint64_t explicitBindingLayoutShaderGeneration = 0u;
		uint64_t explicitBindingSetShaderInstanceId = 0u;
		uint64_t explicitBindingSetShaderGeneration = 0u;
		uint64_t explicitPipelineLayoutShaderInstanceId = 0u;
		uint64_t explicitPipelineLayoutShaderGeneration = 0u;
		uint64_t shaderGeneration = 0u;
		uint64_t explicitBindingSetCreationCount = 0u;
		uint64_t explicitSnapshotBufferCreationCount = 0u;
#if defined(NLS_ENABLE_TEST_HOOKS)
		uint64_t indexedObjectDataShaderValidationCount = 0u;
#endif
		bool explicitBindingLayoutDirty = true;
		bool explicitBindingSetDirty = true;
	};

	Material::MaterialRuntimeState& Material::GetRuntimeState() const
	{
		if (!m_runtimeState)
			m_runtimeState = std::make_unique<MaterialRuntimeState>();
		return *m_runtimeState;
	}

	void Material::ResetRuntimeState() const
	{
		auto& state = GetRuntimeState();
		state.bindingSet.Clear();
		state.materialConstantBuffers.clear();
		state.explicitBindingLayout.reset();
		state.explicitBindingSet.reset();
		state.explicitPipelineLayout.reset();
		state.explicitBindingLayoutsByShaderKey.clear();
		state.explicitBindingSetsByShaderKey.clear();
		state.explicitPipelineLayoutsByShaderKey.clear();
		state.passDescriptorSetRequirementsByShader.clear();
		state.explicitBindingDiagnostics.clear();
		state.indexedObjectDataBindingDiagnosticSources.clear();
		state.materialConstantBufferDiagnostics.clear();
        state.explicitBindingLayoutDeviceIdentity = 0u;
        state.explicitBindingSetDeviceIdentity = 0u;
        state.explicitPipelineLayoutDeviceIdentity = 0u;
		state.explicitBindingLayoutBackend = RHI::NativeBackendType::None;
		state.explicitBindingSetBackend = RHI::NativeBackendType::None;
		state.explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		state.explicitBindingLayoutShaderInstanceId = 0u;
		state.explicitBindingLayoutShaderGeneration = 0u;
		state.explicitBindingSetShaderInstanceId = 0u;
		state.explicitBindingSetShaderGeneration = 0u;
		state.explicitPipelineLayoutShaderInstanceId = 0u;
		state.explicitPipelineLayoutShaderGeneration = 0u;
		state.shaderGeneration = m_shader != nullptr ? m_shader->GetGeneration() : 0u;
		state.explicitBindingLayoutDirty = true;
		state.explicitBindingSetDirty = true;
	}

	void Material::EnsureShaderGenerationCacheCurrent() const
	{
		auto& state = GetRuntimeState();
		const auto currentGeneration = m_shader != nullptr ? m_shader->GetGeneration() : 0u;
		if (state.shaderGeneration == currentGeneration)
			return;

		const auto bindingSetCreationCount = state.explicitBindingSetCreationCount;
		const auto snapshotBufferCreationCount = state.explicitSnapshotBufferCreationCount;
		auto* material = const_cast<Material*>(this);
		const auto previousParameters = material->m_parameterBlock.Data();
		const auto previousTextureResourcePaths = material->m_textureResourcePaths;
		const auto previousSamplerOverrides = material->m_samplerOverrides;

		material->FillUniform();
		GetRuntimeState().explicitBindingSetCreationCount = bindingSetCreationCount;
		GetRuntimeState().explicitSnapshotBufferCreationCount = snapshotBufferCreationCount;
		for (const auto& [name, value] : previousParameters)
		{
			if (material->m_parameterBlock.Contains(name))
				material->m_parameterBlock.Set(name, value);
		}
		material->m_textureResourcePaths = previousTextureResourcePaths;
		material->m_samplerOverrides = previousSamplerOverrides;
		material->RebuildBindingSet();
		GetRuntimeState().shaderGeneration = currentGeneration;
	}

	void Material::PruneStaleExplicitShaderGenerationEntries(
		const uint64_t shaderInstanceId,
		const uint64_t shaderGeneration) const
	{
		if (shaderInstanceId == 0u)
			return;

		auto& state = GetRuntimeState();
		const auto isStaleGeneration = [shaderInstanceId, shaderGeneration](const ExplicitShaderCacheKey& key)
		{
			return key.shaderInstanceId == shaderInstanceId &&
				key.shaderGeneration != shaderGeneration;
		};
		auto eraseStaleEntries = [&isStaleGeneration](auto& entries)
		{
			for (auto entry = entries.begin(); entry != entries.end();)
			{
				if (isStaleGeneration(entry->first))
					entry = entries.erase(entry);
				else
					++entry;
			}
		};

		if (state.explicitBindingSetShaderInstanceId == shaderInstanceId &&
			state.explicitBindingSetShaderGeneration != shaderGeneration)
		{
			state.explicitBindingSet.reset();
			state.explicitBindingSetDeviceIdentity = 0u;
			state.explicitBindingSetBackend = RHI::NativeBackendType::None;
			state.explicitBindingSetShaderInstanceId = 0u;
			state.explicitBindingSetShaderGeneration = 0u;
			state.explicitBindingSetDirty = true;
		}
		if (state.explicitPipelineLayoutShaderInstanceId == shaderInstanceId &&
			state.explicitPipelineLayoutShaderGeneration != shaderGeneration)
		{
			state.explicitPipelineLayout.reset();
			state.explicitPipelineLayoutDeviceIdentity = 0u;
			state.explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
			state.explicitPipelineLayoutShaderInstanceId = 0u;
			state.explicitPipelineLayoutShaderGeneration = 0u;
		}
		if (state.explicitBindingLayoutShaderInstanceId == shaderInstanceId &&
			state.explicitBindingLayoutShaderGeneration != shaderGeneration)
		{
			state.explicitBindingLayout.reset();
			state.explicitBindingLayoutDeviceIdentity = 0u;
			state.explicitBindingLayoutBackend = RHI::NativeBackendType::None;
			state.explicitBindingLayoutShaderInstanceId = 0u;
			state.explicitBindingLayoutShaderGeneration = 0u;
			state.explicitBindingLayoutDirty = true;
		}

		eraseStaleEntries(state.explicitBindingSetsByShaderKey);
		eraseStaleEntries(state.explicitPipelineLayoutsByShaderKey);
		eraseStaleEntries(state.explicitBindingLayoutsByShaderKey);
		state.indexedObjectDataBindingDiagnosticSources.erase(
			std::remove_if(
				state.indexedObjectDataBindingDiagnosticSources.begin(),
				state.indexedObjectDataBindingDiagnosticSources.end(),
				[shaderInstanceId, shaderGeneration](const IndexedObjectDataBindingDiagnosticSource& source)
				{
					return source.shaderInstanceId == shaderInstanceId &&
						source.shaderGeneration != shaderGeneration;
				}),
			state.indexedObjectDataBindingDiagnosticSources.end());
		RefreshIndexedObjectDataBindingDiagnostics(
			state.explicitBindingDiagnostics,
			state.indexedObjectDataBindingDiagnosticSources);
	}

	void Material::InvalidateExplicitBindingSetCache() const
	{
		auto& state = GetRuntimeState();
		++m_bindingRevision;
		state.explicitBindingSet.reset();
		state.explicitPipelineLayout.reset();
		state.explicitBindingSetsByShaderKey.clear();
		state.explicitPipelineLayoutsByShaderKey.clear();
		state.explicitBindingDiagnostics.clear();
		state.indexedObjectDataBindingDiagnosticSources.clear();
        state.explicitBindingSetDeviceIdentity = 0u;
        state.explicitPipelineLayoutDeviceIdentity = 0u;
		state.explicitBindingSetBackend = RHI::NativeBackendType::None;
		state.explicitPipelineLayoutBackend = RHI::NativeBackendType::None;
		state.explicitBindingSetShaderInstanceId = 0u;
		state.explicitBindingSetShaderGeneration = 0u;
		state.explicitPipelineLayoutShaderInstanceId = 0u;
		state.explicitPipelineLayoutShaderGeneration = 0u;
		state.explicitBindingSetDirty = true;
	}

	Material::Material(Shader* p_shader)
		: m_surfaceMode(MaterialSurfaceMode::Opaque)
		, m_instanceId(NextMaterialInstanceId())
	{
		m_runtimeState = std::make_unique<MaterialRuntimeState>();
		RegisterLiveMaterial(this);
		SetShader(p_shader);
	}

	Material::~Material()
	{
		UnregisterLiveMaterial(this);
	}

	void Material::SetShader(Shader* p_shader)
	{
		m_shader = p_shader;
		if (m_shader != nullptr && !m_shaderLabSourcePathExplicit && !m_shader->GetImportedArtifactSourcePath().empty())
			m_shaderLabSourcePath = m_shader->GetImportedArtifactSourcePath();
		if (m_shader == nullptr && !m_shaderLabSourcePathExplicit)
			m_shaderLabSourcePath.clear();
		if (m_shader == nullptr)
			m_shaderReferencePath.clear();
		++m_renderStateRevision;
		ResetRuntimeState();

		if (m_shader)
			FillUniform();
		else
		{
			m_parameterBlock.Clear();
			m_textureResourcePaths.clear();
			m_samplerOverrides.clear();
			m_bindingLayout.bindings.clear();
		}
	}

	void Material::FillUniform()
	{
		m_parameterBlock.Clear();
		m_textureResourcePaths.clear();
		m_samplerOverrides.clear();
		m_bindingLayout.bindings.clear();
		ResetRuntimeState();

		if (!m_shader)
			return;

		FillMissingMaterialDefaultsFromShader(m_parameterBlock, m_shader);

		RebuildBindingLayout();
		RebuildBindingSet();
	}

	std::optional<ShaderPropertyDesc> Material::FindMaterialProperty(const std::string& key) const
	{
		if (m_shader == nullptr)
			return std::nullopt;

		const auto reflection = m_shader->GetReflectionSnapshot();
		const auto& properties = reflection->properties;
		const auto found = std::find_if(
			properties.begin(),
			properties.end(),
			[&key](const ShaderPropertyDesc& property)
			{
				return property.name == key && ShouldExposeToMaterial(property);
			});
		return found != properties.end() ? std::optional<ShaderPropertyDesc>(*found) : std::nullopt;
	}

	bool Material::EnsureMaterialParameterExists(const std::string& key)
	{
		if (m_parameterBlock.Contains(key))
			return true;

		const auto property = FindMaterialProperty(key);
		if (!property.has_value())
			return false;

		if (const auto uniformInfo = m_shader->GetUniformInfo(property->name); uniformInfo.has_value())
			m_parameterBlock.Set(property->name, uniformInfo->defaultValue);
		else if (const auto defaultValue = CreateDefaultMaterialValue(property->type); defaultValue.has_value())
			m_parameterBlock.Set(property->name, defaultValue);
		else
			return false;

		InvalidateExplicitBindingSetCache();
		return true;
	}

	void Material::SyncParameterLayout()
	{
		FillUniform();
	}

	void Material::RebuildBindingLayout()
	{
		ResetRuntimeState();

		m_bindingLayout = BuildMaterialBindingLayoutForShader(m_shader);
		m_materialLayout.bindings = m_bindingLayout;
	}

	void Material::RebuildBindingSet() const
	{
		RebuildBindingSet(m_shader);
	}

	void Material::RebuildBindingSet(const Shader* effectiveShader) const
	{
		auto& state = GetRuntimeState();
		EnsureShaderGenerationCacheCurrent();
		state.materialConstantBufferDiagnostics.clear();
		const auto* shader = effectiveShader != nullptr ? effectiveShader : m_shader;
		FillMissingMaterialDefaultsFromShader(const_cast<Material*>(this)->m_parameterBlock, shader);
		const auto bindingLayout = shader == m_shader
			? m_bindingLayout
			: BuildMaterialBindingLayoutForShader(shader);
		state.bindingSet.SetLayout(bindingLayout);

		for (const auto& binding : bindingLayout.bindings)
		{
			if (binding.kind == ShaderResourceKind::Sampler)
			{
				const auto override = m_samplerOverrides.find(binding.name);
				state.bindingSet.SetSampler(
					binding.name,
					override != m_samplerOverrides.end()
						? override->second
						: BuildDefaultSamplerDesc(binding.name));
				continue;
			}

			const auto* parameter = m_parameterBlock.TryGet(binding.name);
			if (!parameter && !(binding.type == UniformType::UNIFORM_SAMPLER_2D && IsMaterialTextureBindingName(binding.name)))
				continue;

			switch (binding.type)
			{
			case UniformType::UNIFORM_SAMPLER_2D:
				if (parameter && parameter->type() == typeid(Texture2D*))
				{
					auto* texture = std::any_cast<Texture2D*>(*parameter);
					const auto resolvedTexture = texture != nullptr ? texture : GetDefaultWhiteTexture2D();
					state.bindingSet.SetTexture(
						binding.name,
						resolvedTexture != nullptr ? resolvedTexture->GetTextureHandle() : nullptr);
				}
				else if (IsMaterialTextureBindingName(binding.name))
				{
					auto* texture = GetDefaultWhiteTexture2D();
					state.bindingSet.SetTexture(
						binding.name,
						texture != nullptr ? texture->GetTextureHandle() : nullptr);
				}
				break;
			case UniformType::UNIFORM_SAMPLER_CUBE:
				if (parameter->type() == typeid(TextureCube*))
				{
					const auto* texture = std::any_cast<TextureCube*>(*parameter);
					state.bindingSet.SetTexture(
						binding.name,
						texture != nullptr ? texture->GetTextureHandle() : nullptr);
				}
				break;
			default:
				break;
			}
		}

		if (!shader)
			return;

		if (ShouldLogMaterialBindingDiagnostics() && shader->path.find("Skybox") != std::string::npos)
		{
			const auto reflection = shader->GetReflectionSnapshot();
			NLS_LOG_INFO("[SkyboxMaterial] Reflection constant buffer count = " + std::to_string(reflection->constantBuffers.size()));
			for (const auto& constantBuffer : reflection->constantBuffers)
			{
				NLS_LOG_INFO(
					"[SkyboxMaterial] CBuffer name=" + constantBuffer.name +
					" space=" + std::to_string(constantBuffer.bindingSpace) +
					" binding=" + std::to_string(constantBuffer.bindingIndex) +
					" size=" + std::to_string(constantBuffer.byteSize));
				if (constantBuffer.name == "MaterialConstants")
				{
					for (const auto& member : constantBuffer.members)
					{
						NLS_LOG_INFO(
							"[SkyboxMaterial]   member name=" + member.name +
							" type=" + std::to_string(static_cast<int>(member.type)) +
							" offset=" + std::to_string(member.byteOffset) +
							" size=" + std::to_string(member.byteSize));
					}
				}
			}

			if (const auto* useProceduralSky = m_parameterBlock.TryGet("u_UseProceduralSky"); useProceduralSky != nullptr && useProceduralSky->type() == typeid(bool))
				NLS_LOG_INFO(std::string("[SkyboxMaterial] Parameter u_UseProceduralSky=") + (std::any_cast<bool>(*useProceduralSky) ? "true" : "false"));
			else
				NLS_LOG_INFO("[SkyboxMaterial] Parameter u_UseProceduralSky is missing");
		}

		const auto reflection = shader->GetReflectionSnapshot();
		for (const auto& constantBuffer : reflection->constantBuffers)
		{
			if (constantBuffer.bindingSpace != NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace ||
				constantBuffer.byteSize == 0)
			{
				continue;
			}

			const auto bindingPoint = NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(
				constantBuffer.bindingSpace,
				constantBuffer.bindingIndex);

			auto& bufferState = state.materialConstantBuffers[constantBuffer.name];
			if (!bufferState.buffer || bufferState.size != constantBuffer.byteSize)
			{
				bufferState.buffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
					constantBuffer.byteSize,
					bindingPoint,
					0,
					Settings::EAccessSpecifier::STREAM_DRAW);
				bufferState.size = constantBuffer.byteSize;
			}

			auto bufferData = BuildMaterialConstantBufferData(constantBuffer, m_parameterBlock);
			if (!bufferData.success)
			{
				state.materialConstantBufferDiagnostics.insert(
					state.materialConstantBufferDiagnostics.end(),
					bufferData.diagnostics.begin(),
					bufferData.diagnostics.end());
				state.materialConstantBuffers.erase(constantBuffer.name);
				continue;
			}
			if (ShouldLogMaterialBindingDiagnostics() && shader->path.find("Skybox") != std::string::npos)
			{
				NLS_LOG_INFO(
					"[SkyboxMaterial] Buffer \"" + constantBuffer.name +
					"\" members=" + std::to_string(constantBuffer.members.size()) +
					" bytes=" + std::to_string(bufferData.data.size()) +
					" firstInt=" + std::to_string(bufferData.data.size() >= sizeof(int32_t) ? *reinterpret_cast<const int32_t*>(bufferData.data.data()) : -1));
			}
			if (!bufferData.data.empty())
				bufferState.buffer->SetRawData(bufferData.data.data(), static_cast<uint32_t>(bufferData.data.size()));

			state.bindingSet.SetBuffer(
				constantBuffer.name,
				bufferState.buffer->GetBufferHandle());
		}
	}

	Shader*& Material::GetShader()
	{
		return m_shader;
	}

	const Shader* Material::GetShader() const
	{
		return m_shader;
	}

	Shader* Material::ResolveShaderForLightMode(std::string_view lightMode) const
	{
		if (lightMode.empty())
			return m_shader;

		if (!m_shaderLabSourcePathExplicit && m_shaderLabPassShadersByLightMode.empty())
		{
			if (m_shader != nullptr && !m_shader->GetShaderLabLightMode().empty())
				return m_shader->GetShaderLabLightMode() == lightMode ? m_shader : nullptr;
			return lightMode == "Forward" ? m_shader : nullptr;
		}

		const auto found = m_shaderLabPassShadersByLightMode.find(std::string(lightMode));
		if (found != m_shaderLabPassShadersByLightMode.end() &&
			found->second != nullptr &&
			found->second->GetShaderLabLightMode() == lightMode)
		{
			return found->second;
		}

		for (const auto& [_, shader] : m_shaderLabPassShadersByLightMode)
		{
			if (shader != nullptr && shader->GetShaderLabLightMode() == lightMode)
				return shader;
		}

		return nullptr;
	}

	void Material::SetShaderLabSourcePath(std::string sourcePath)
	{
		const auto sourcePathExplicit = !sourcePath.empty();
		if (m_shaderLabSourcePath == sourcePath && m_shaderLabSourcePathExplicit == sourcePathExplicit)
			return;

		m_shaderLabSourcePath = std::move(sourcePath);
		m_shaderLabSourcePathExplicit = sourcePathExplicit;
		m_shaderLabPassShadersByLightMode.clear();
		++m_renderStateRevision;
		ResetRuntimeState();
	}

	const std::string& Material::GetShaderLabSourcePath() const
	{
		return m_shaderLabSourcePath;
	}

	bool Material::HasExplicitShaderLabSourcePath() const
	{
		return m_shaderLabSourcePathExplicit;
	}

	void Material::SetShaderReferencePath(std::string shaderPath)
	{
		if (m_shaderReferencePath == shaderPath)
			return;

		m_shaderReferencePath = std::move(shaderPath);
		++m_renderStateRevision;
		ResetRuntimeState();
	}

	const std::string& Material::GetShaderReferencePath() const
	{
		return m_shaderReferencePath;
	}

	void Material::RegisterShaderLabPassShader(Shader* shader)
	{
		if (shader == nullptr)
			return;

		const auto sourcePath = shader->GetImportedArtifactSourcePath();
		const auto lightMode = shader->GetShaderLabLightMode();
		if (sourcePath.empty() || lightMode.empty())
			return;
		if (!m_shaderLabSourcePath.empty() && !ShaderLabSourceIdentityMatches(sourcePath, m_shaderLabSourcePath))
			return;

		if (m_shaderLabSourcePath.empty())
			m_shaderLabSourcePath = sourcePath;

		const auto [_, inserted] = m_shaderLabPassShadersByLightMode.try_emplace(lightMode, shader);
		if (!inserted)
			return;
		const bool becomesPrimaryShader = m_shader == nullptr || lightMode == "Forward";
		const bool primaryShaderChanged = becomesPrimaryShader && m_shader != shader;
		if (becomesPrimaryShader)
			m_shader = shader;

		if (primaryShaderChanged)
		{
			FillMissingMaterialDefaultsFromShader(m_parameterBlock, m_shader);
			m_bindingLayout = BuildMaterialBindingLayoutForShader(m_shader);
			m_materialLayout.bindings = m_bindingLayout;
		}

		++m_renderStateRevision;
		ResetRuntimeState();
	}

	void Material::ClearShaderReferencesFromLiveMaterials(const Shader* shader)
	{
		if (shader == nullptr)
			return;

		std::vector<Material*> materials;
		{
			std::lock_guard lock(LiveMaterialRegistryMutex());
			materials.assign(LiveMaterialRegistry().begin(), LiveMaterialRegistry().end());
		}

		for (auto* material : materials)
		{
			if (material != nullptr)
				material->ClearShaderReferences(shader);
		}
	}

	void Material::ClearShaderReferences(const Shader* shader)
	{
		if (shader == nullptr)
			return;

		bool changed = false;
		if (m_shader == shader)
		{
			m_shader = nullptr;
			m_shaderReferencePath.clear();
			changed = true;
		}

		for (auto it = m_shaderLabPassShadersByLightMode.begin(); it != m_shaderLabPassShadersByLightMode.end();)
		{
			if (it->second == shader)
			{
				it = m_shaderLabPassShadersByLightMode.erase(it);
				changed = true;
			}
			else
			{
				++it;
			}
		}

		if (!changed)
			return;

		if (m_shader == nullptr)
		{
			if (const auto found = m_shaderLabPassShadersByLightMode.find("Forward");
				found != m_shaderLabPassShadersByLightMode.end())
			{
				m_shader = found->second;
			}
			else if (!m_shaderLabPassShadersByLightMode.empty())
			{
				m_shader = m_shaderLabPassShadersByLightMode.begin()->second;
			}
		}

		++m_renderStateRevision;
		ResetRuntimeState();
	}

	bool Material::HasShader() const
	{
		return m_shader != nullptr;
	}

	bool Material::IsValid() const
	{
		return HasShader();
	}

	void Material::SetBlendable(bool p_transparent)
	{
		if (m_surfaceMode == MaterialSurfaceMode::Decal)
		{
			if (!m_blendable)
			{
				m_blendable = true;
				++m_renderStateRevision;
			}
			return;
		}

		if (m_blendable == p_transparent)
		{
			if (m_surfaceMode == MaterialSurfaceMode::Opaque && p_transparent)
			{
				m_surfaceMode = MaterialSurfaceMode::Transparent;
				++m_renderStateRevision;
			}
			else if (m_surfaceMode == MaterialSurfaceMode::Transparent && !p_transparent)
			{
				m_surfaceMode = MaterialSurfaceMode::Opaque;
				++m_renderStateRevision;
			}
			return;
		}
		m_blendable = p_transparent;
		if (m_surfaceMode == MaterialSurfaceMode::Opaque && p_transparent)
			m_surfaceMode = MaterialSurfaceMode::Transparent;
		else if (m_surfaceMode == MaterialSurfaceMode::Transparent && !p_transparent)
			m_surfaceMode = MaterialSurfaceMode::Opaque;
		++m_renderStateRevision;
	}

	void Material::SetSurfaceMode(const MaterialSurfaceMode surfaceMode)
	{
		const bool blendable = surfaceMode == MaterialSurfaceMode::Transparent ||
			surfaceMode == MaterialSurfaceMode::Decal;
		if (m_surfaceMode == surfaceMode && m_blendable == blendable)
			return;

		m_surfaceMode = surfaceMode;
		m_blendable = blendable;
		++m_renderStateRevision;
	}

	void Material::SetBackfaceCulling(bool p_backfaceCulling)
	{
		if (m_backfaceCulling == p_backfaceCulling)
			return;
		m_backfaceCulling = p_backfaceCulling;
		++m_renderStateRevision;
	}

	void Material::SetFrontfaceCulling(bool p_frontfaceCulling)
	{
		if (m_frontfaceCulling == p_frontfaceCulling)
			return;
		m_frontfaceCulling = p_frontfaceCulling;
		++m_renderStateRevision;
	}

	void Material::SetDepthTest(bool p_depthTest)
	{
		if (m_depthTest == p_depthTest)
			return;
		m_depthTest = p_depthTest;
		++m_renderStateRevision;
	}

	void Material::SetDepthWriting(bool p_depthWriting)
	{
		if (m_depthWriting == p_depthWriting)
			return;
		m_depthWriting = p_depthWriting;
		++m_renderStateRevision;
	}

	void Material::SetColorWriting(bool p_colorWriting)
	{
		if (m_colorWriting == p_colorWriting)
			return;
		m_colorWriting = p_colorWriting;
		++m_renderStateRevision;
	}

	void Material::SetGPUInstances(int p_instances)
	{
		if (m_gpuInstances == p_instances)
			return;
		m_gpuInstances = p_instances;
		++m_renderStateRevision;
	}

	void Material::EnableKeyword(std::string keyword)
	{
		if (keyword.empty() || m_shaderLabKeywords.Contains(keyword))
			return;
		m_shaderLabKeywords.Enable(std::move(keyword));
		++m_renderStateRevision;
		ResetRuntimeState();
	}

	void Material::DisableKeyword(std::string_view keyword)
	{
		if (!m_shaderLabKeywords.Contains(keyword))
			return;
		m_shaderLabKeywords.Disable(keyword);
		++m_renderStateRevision;
		ResetRuntimeState();
	}

	bool Material::IsKeywordEnabled(std::string_view keyword) const
	{
		return m_shaderLabKeywords.Contains(keyword);
	}

	std::vector<std::string> Material::GetShaderLabKeywordNames() const
	{
		return m_shaderLabKeywords.ToVector();
	}

	const NLS::Render::ShaderLab::ShaderLabKeywordSet& Material::GetShaderLabKeywords() const
	{
		return m_shaderLabKeywords;
	}

	bool Material::IsBlendable() const { return m_blendable; }
	MaterialSurfaceMode Material::GetSurfaceMode() const { return m_surfaceMode; }
	bool Material::IsDecal() const { return m_surfaceMode == MaterialSurfaceMode::Decal; }
	bool Material::IsTransparentSurface() const { return m_surfaceMode == MaterialSurfaceMode::Transparent; }
	bool Material::HasBackfaceCulling() const { return m_backfaceCulling; }
	bool Material::HasFrontfaceCulling() const { return m_frontfaceCulling; }
	bool Material::HasDepthTest() const { return m_depthTest; }
	bool Material::HasDepthWriting() const { return m_depthWriting; }
	bool Material::HasColorWriting() const { return m_colorWriting; }
	int Material::GetGPUInstances() const { return m_gpuInstances; }

	const Data::StateMask Material::GenerateStateMask() const
	{
		Data::StateMask stateMask{};
		stateMask.depthWriting = m_depthWriting;
		stateMask.colorWriting = m_colorWriting;
		stateMask.blendable = m_blendable;
		stateMask.depthTest = m_depthTest;
		stateMask.frontfaceCulling = m_frontfaceCulling;
		stateMask.backfaceCulling = m_backfaceCulling;
		return stateMask;
	}

	std::shared_ptr<RHI::RHIGraphicsPipeline> Material::BuildRecordedGraphicsPipeline(
		const std::shared_ptr<RHI::RHIDevice>& device,
        const std::shared_ptr<RHI::PipelineCache>& pipelineCache,
		Settings::EPrimitiveMode primitiveMode,
		const Data::PipelineState& pipelineState,
		MaterialPipelineStateOverrides overrides,
		bool* hasPipelineLayout,
		bool* hasVertexShader,
		bool* hasFragmentShader,
		const Shader* effectiveShader) const
	{
		const auto* shader = effectiveShader != nullptr ? effectiveShader : m_shader;
		const auto pipelineLayout = GetExplicitPipelineLayout(device, shader);
		const auto shaderLabKeywordHash = m_shaderLabKeywords.Hash();
		const auto vertexShader = device != nullptr && shader != nullptr
			? shader->GetOrCreateExplicitShaderModule(device, NLS::Render::ShaderCompiler::ShaderStage::Vertex, shaderLabKeywordHash)
			: nullptr;
		const auto fragmentShader = device != nullptr && shader != nullptr
			? shader->GetOrCreateExplicitShaderModule(device, NLS::Render::ShaderCompiler::ShaderStage::Pixel, shaderLabKeywordHash)
			: nullptr;

		if (hasPipelineLayout != nullptr)
			*hasPipelineLayout = pipelineLayout != nullptr;
		if (hasVertexShader != nullptr)
			*hasVertexShader = vertexShader != nullptr;
		if (hasFragmentShader != nullptr)
			*hasFragmentShader = fragmentShader != nullptr;

		if (device != nullptr &&
			pipelineLayout != nullptr &&
			vertexShader != nullptr &&
			fragmentShader != nullptr)
		{
			RHI::RHIGraphicsPipelineDesc desc;
			desc.pipelineLayout = pipelineLayout;
			desc.vertexShader = vertexShader;
			desc.fragmentShader = fragmentShader;
			desc.reflection = shader ? shader->GetReflectionSnapshot() : nullptr;
			desc.rasterState.cullEnabled = m_backfaceCulling || m_frontfaceCulling;
			desc.rasterState.cullFace = m_backfaceCulling && m_frontfaceCulling
				? Settings::ECullFace::FRONT_AND_BACK
				: m_frontfaceCulling ? Settings::ECullFace::FRONT : Settings::ECullFace::BACK;
			desc.blendState.enabled = m_blendable;
			desc.blendState.colorWrite = m_colorWriting;
			desc.blendState.renderTargets = { RHI::RHIRenderTargetBlendStateDesc{} };
			desc.blendState.renderTargets[0].blendEnable = m_blendable;
			desc.blendState.renderTargets[0].colorWriteMask = m_colorWriting
				? RHI::RHIColorWriteMask::All
				: RHI::RHIColorWriteMask::None;
			desc.depthStencilState.depthTest = m_depthTest;
			desc.depthStencilState.depthWrite = m_depthWriting;
			switch (primitiveMode)
			{
			case Settings::EPrimitiveMode::LINES:
				desc.primitiveTopology = RHI::PrimitiveTopology::LineList;
				break;
			case Settings::EPrimitiveMode::POINTS:
				desc.primitiveTopology = RHI::PrimitiveTopology::PointList;
				break;
			case Settings::EPrimitiveMode::TRIANGLES:
			default:
				desc.primitiveTopology = RHI::PrimitiveTopology::TriangleList;
				break;
			}
			desc.renderTargetLayout.colorFormats = { RHI::TextureFormat::RGBA8 };
			desc.renderTargetLayout.depthFormat = RHI::TextureFormat::Depth24Stencil8;
			desc.renderTargetLayout.hasDepth = true;
			desc.renderTargetLayout.sampleCount = 1;
			desc.debugName = "RecordedGraphicsPipeline";
			desc.vertexBuffers.push_back({ 0u, static_cast<uint32_t>(sizeof(NLS::Render::Geometry::Vertex)), false });
			desc.vertexAttributes = {
				{ 0u, 0u, 0u, 12u },
				{ 1u, 0u, 12u, 8u },
				{ 2u, 0u, 20u, 12u },
				{ 3u, 0u, 32u, 12u },
				{ 4u, 0u, 44u, 12u }
			};
			RHI::ApplyPipelineStateToGraphicsPipelineDesc(pipelineState, desc);
			if (shader != nullptr && shader->GetShaderLabPassState().has_value())
				ApplyPipelineStateOverrides(
					desc,
					BuildMaterialPipelineStateOverrides(*shader->GetShaderLabPassState()));
			ApplyPipelineStateOverrides(desc, overrides);
			if (!desc.renderTargetLayout.hasDepth)
			{
				desc.depthStencilState.depthTest = false;
				desc.depthStencilState.depthWrite = false;
				desc.depthStencilState.stencilTest = false;
			}
			if (pipelineCache == nullptr)
				return nullptr;

			const auto cacheKey = RHI::BuildGraphicsPipelineCacheKey(desc);
			return pipelineCache->GetOrCreateGraphicsPipeline(
				cacheKey,
				[device, desc]()
				{
					return device->CreateGraphicsPipeline(desc);
				},
				RHI::PipelineCacheRequestMode::Runtime);
		}

		return nullptr;
	}

	std::shared_ptr<RHI::RHIBindingSet> Material::GetRecordedBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		return GetRecordedBindingSet(device, m_shader);
	}

	std::shared_ptr<RHI::RHIBindingSet> Material::GetRecordedBindingSet(
		const std::shared_ptr<RHI::RHIDevice>& device,
		const Shader* effectiveShader) const
	{
		return GetExplicitBindingSet(device, effectiveShader);
	}

	const MaterialParameterBlock& Material::GetParameterBlock() const
	{
		return m_parameterBlock;
	}

	const MaterialResourceSet& Material::GetBindingSet() const
	{
		RebuildBindingSet();
		return GetRuntimeState().bindingSet;
	}

	const std::shared_ptr<RHI::RHIBindingLayout>& Material::GetExplicitBindingLayout(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		return GetExplicitBindingLayout(device, m_shader);
	}

	const std::shared_ptr<RHI::RHIBindingLayout>& Material::GetExplicitBindingLayout(
		const std::shared_ptr<RHI::RHIDevice>& device,
		const Shader* effectiveShader) const
	{
		EnsureShaderGenerationCacheCurrent();
		auto& state = GetRuntimeState();
		const auto* shader = effectiveShader != nullptr ? effectiveShader : m_shader;
		const auto shaderInstanceId = shader != nullptr ? shader->GetInstanceId() : 0u;
		const auto shaderGeneration = shader != nullptr ? shader->GetGeneration() : 0u;
		const auto backend = ResolveDeviceBackendType(device);
        const auto deviceIdentity = ResolveDeviceCacheIdentity(device);
		const auto cacheKey = BuildExplicitShaderCacheKey(device, shader);
		if (const auto cached = state.explicitBindingLayoutsByShaderKey.find(cacheKey);
			cached != state.explicitBindingLayoutsByShaderKey.end())
		{
			state.explicitBindingLayout = cached->second;
			state.explicitBindingLayoutDeviceIdentity = deviceIdentity;
			state.explicitBindingLayoutBackend = backend;
			state.explicitBindingLayoutShaderInstanceId = shaderInstanceId;
			state.explicitBindingLayoutShaderGeneration = shaderGeneration;
			state.explicitBindingLayoutDirty = false;
			return state.explicitBindingLayout;
		}
		PruneStaleExplicitShaderGenerationEntries(shaderInstanceId, shaderGeneration);
		if (!state.explicitBindingLayoutDirty &&
			state.explicitBindingLayout != nullptr &&
            state.explicitBindingLayoutDeviceIdentity == deviceIdentity &&
			state.explicitBindingLayoutBackend == backend &&
			state.explicitBindingLayoutShaderInstanceId == shaderInstanceId &&
			state.explicitBindingLayoutShaderGeneration == shaderGeneration)
		{
			return state.explicitBindingLayout;
		}

		state.explicitBindingLayout.reset();
		ClearNonIndexedObjectDataBindingDiagnostics(state.explicitBindingDiagnostics);
        state.explicitBindingLayoutDeviceIdentity = deviceIdentity;
		state.explicitBindingLayoutBackend = backend;
		state.explicitBindingLayoutShaderInstanceId = shaderInstanceId;
		state.explicitBindingLayoutShaderGeneration = shaderGeneration;
		if (!shader)
		{
			state.explicitBindingLayoutDirty = false;
			return state.explicitBindingLayout;
		}

		if (!shader->HasParameterStructs())
		{
			const auto validation = ValidateShaderBindingReflection(*shader->GetReflectionSnapshot());
			if (validation.HasErrors())
			{
				for (const auto& diagnostic : validation.diagnostics)
				{
					if (diagnostic.severity == ShaderBindingValidationSeverity::Error)
					{
						state.explicitBindingDiagnostics.push_back({
							MaterialBindingDiagnosticSeverity::Error,
							{},
							diagnostic.message
						});
					}
				}
				state.explicitBindingLayoutDirty = false;
				return state.explicitBindingLayout;
			}
		}

		const auto layoutDescs = BuildExplicitBindingLayoutDescs(
			*shader,
			path.empty() ? "Material" : path);
		constexpr uint32_t materialSetIndex = NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet;
		if (device != nullptr &&
			layoutDescs.size() > materialSetIndex &&
			!layoutDescs[materialSetIndex].entries.empty())
		{
			auto layoutDesc = layoutDescs[materialSetIndex];
			layoutDesc.debugName = path.empty() ? "MaterialBindingLayout" : path + ":MaterialBindingLayout";
			state.explicitBindingLayout = device->CreateBindingLayout(layoutDesc);
		}
		state.explicitBindingLayoutsByShaderKey[cacheKey] = state.explicitBindingLayout;
		state.explicitBindingLayoutDirty = false;
		return state.explicitBindingLayout;
	}

	const std::shared_ptr<RHI::RHIBindingSet>& Material::GetExplicitBindingSet(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		return GetExplicitBindingSet(device, m_shader);
	}

	const std::shared_ptr<RHI::RHIBindingSet>& Material::GetExplicitBindingSet(
		const std::shared_ptr<RHI::RHIDevice>& device,
		const Shader* effectiveShader) const
	{
		EnsureShaderGenerationCacheCurrent();
		auto& state = GetRuntimeState();
		const auto* shader = effectiveShader != nullptr ? effectiveShader : m_shader;
		const auto shaderInstanceId = shader != nullptr ? shader->GetInstanceId() : 0u;
		const auto shaderGeneration = shader != nullptr ? shader->GetGeneration() : 0u;
		const auto backend = ResolveDeviceBackendType(device);
        const auto deviceIdentity = ResolveDeviceCacheIdentity(device);
		const auto cacheKey = BuildExplicitShaderCacheKey(device, shader);
		if (const auto cached = state.explicitBindingSetsByShaderKey.find(cacheKey);
			cached != state.explicitBindingSetsByShaderKey.end())
		{
			state.explicitBindingSet = cached->second;
			state.explicitBindingSetDeviceIdentity = deviceIdentity;
			state.explicitBindingSetBackend = backend;
			state.explicitBindingSetShaderInstanceId = shaderInstanceId;
			state.explicitBindingSetShaderGeneration = shaderGeneration;
			state.explicitBindingSetDirty = false;
			return state.explicitBindingSet;
		}
		PruneStaleExplicitShaderGenerationEntries(shaderInstanceId, shaderGeneration);
		if (!state.explicitBindingSetDirty &&
			state.explicitBindingSet != nullptr &&
            state.explicitBindingSetDeviceIdentity == deviceIdentity &&
			state.explicitBindingSetBackend == backend &&
			state.explicitBindingSetShaderInstanceId == shaderInstanceId &&
			state.explicitBindingSetShaderGeneration == shaderGeneration)
		{
			return state.explicitBindingSet;
		}

		RebuildBindingSet(shader);

		const auto& explicitLayout = GetExplicitBindingLayout(device, shader);
		state.explicitBindingSet.reset();
        state.explicitBindingSetDeviceIdentity = deviceIdentity;
		state.explicitBindingSetBackend = backend;
		state.explicitBindingSetShaderInstanceId = shaderInstanceId;
		state.explicitBindingSetShaderGeneration = shaderGeneration;
		if (device == nullptr || explicitLayout == nullptr)
		{
			state.explicitBindingSetDirty = false;
			return state.explicitBindingSet;
		}

		RHI::RHIBindingSetDesc bindingSetDesc;
		bindingSetDesc.layout = explicitLayout;
		bindingSetDesc.debugName = path.empty() ? "MaterialBindingSet" : path + ":MaterialBindingSet";
		ClearNonIndexedObjectDataBindingDiagnostics(state.explicitBindingDiagnostics);
		state.explicitBindingDiagnostics.insert(
			state.explicitBindingDiagnostics.end(),
			state.materialConstantBufferDiagnostics.begin(),
			state.materialConstantBufferDiagnostics.end());

		auto addBindingDiagnostic = [&state](MaterialBindingDiagnosticSeverity severity, std::string bindingName, std::string message)
		{
			state.explicitBindingDiagnostics.push_back({
				severity,
				std::move(bindingName),
				std::move(message)
			});
		};
		auto hasExistingBindingError = [&state](const std::string& bindingName)
		{
			return std::any_of(
				state.explicitBindingDiagnostics.begin(),
				state.explicitBindingDiagnostics.end(),
				[&bindingName](const MaterialBindingDiagnostic& diagnostic)
				{
					return diagnostic.severity == MaterialBindingDiagnosticSeverity::Error &&
						diagnostic.bindingName == bindingName;
				});
		};

		for (const auto& entry : explicitLayout->GetDesc().entries)
		{
			RHI::RHIBindingSetEntry bindingEntry;
			bindingEntry.binding = entry.binding;
			bindingEntry.type = entry.type;

			switch (entry.type)
			{
			case RHI::BindingType::UniformBuffer:
			case RHI::BindingType::StructuredBuffer:
			case RHI::BindingType::StorageBuffer:
			{
				auto bufferState = state.materialConstantBuffers.find(entry.name);
				if (bufferState == state.materialConstantBuffers.end() || !bufferState->second.buffer)
				{
					if (!hasExistingBindingError(entry.name))
					{
						addBindingDiagnostic(
							MaterialBindingDiagnosticSeverity::Error,
							entry.name,
							"Material binding \"" + entry.name + "\" is missing required buffer resource.");
					}
					if (ShouldLogMaterialBindingDiagnostics() && shader != nullptr && shader->path.find("Skybox") != std::string::npos)
					{
						NLS_LOG_INFO(
							"[SkyboxMaterial] Missing explicit buffer for entry \"" + entry.name +
							"\". known buffers=" + std::to_string(state.materialConstantBuffers.size()));
					}
					continue;
				}

				bindingEntry.buffer = bufferState->second.buffer->CreateExplicitSnapshotBuffer(entry.name + "Snapshot");
				if (bindingEntry.buffer != nullptr)
					++state.explicitSnapshotBufferCreationCount;
				if (bindingEntry.buffer == nullptr)
				bindingEntry.buffer = bufferState->second.buffer->GetBufferHandle();
				if (bindingEntry.buffer == nullptr)
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null buffer descriptor.");
					continue;
				}
				bindingEntry.bufferRange = bufferState->second.size;
				bindingEntry.elementStride = entry.elementStride;
				break;
			}
			case RHI::BindingType::Texture:
			case RHI::BindingType::RWTexture:
			{
				const auto* parameter = m_parameterBlock.TryGet(entry.name);
				if (parameter == nullptr)
				{
					if (IsMaterialTextureBindingName(entry.name))
					{
						const auto* texture = GetDefaultWhiteTexture2D();
						if (texture != nullptr && texture->GetTextureHandle())
						{
							bindingEntry.textureView = texture->GetOrCreateExplicitTextureView(entry.name + "View");
							if (bindingEntry.textureView != nullptr)
								break;
						}
					}

					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" is missing required texture parameter.");
					if (ShouldLogMaterialBindingDiagnostics() && shader != nullptr && shader->path.find("Skybox") != std::string::npos)
						NLS_LOG_INFO("[SkyboxMaterial] Missing texture parameter for entry \"" + entry.name + "\"");
					continue;
				}

				const Texture* texture = nullptr;
				switch (parameter->type() == typeid(Texture2D*) ? UniformType::UNIFORM_SAMPLER_2D :
					parameter->type() == typeid(TextureCube*) ? UniformType::UNIFORM_SAMPLER_CUBE :
					UniformType::UNIFORM_FLOAT)
				{
				case UniformType::UNIFORM_SAMPLER_2D:
				{
					auto* texture2D = std::any_cast<Texture2D*>(*parameter);
					texture = texture2D != nullptr ? texture2D : GetDefaultWhiteTexture2D();
					break;
				}
				case UniformType::UNIFORM_SAMPLER_CUBE:
				{
					auto* textureCube = std::any_cast<TextureCube*>(*parameter);
					texture = textureCube != nullptr ? textureCube : GetDefaultWhiteTextureCube();
					break;
				}
				default:
					break;
				}

				if (texture == nullptr || !texture->GetTextureHandle())
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null texture descriptor.");
					if (ShouldLogMaterialBindingDiagnostics() && shader != nullptr && shader->path.find("Skybox") != std::string::npos)
						NLS_LOG_INFO("[SkyboxMaterial] Texture entry \"" + entry.name + "\" resolved to null texture or null handle, skipping");
					continue;
				}

				bindingEntry.textureView = texture->GetOrCreateExplicitTextureView(entry.name + "View");
				if (bindingEntry.textureView == nullptr)
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null texture view descriptor.");
					if (ShouldLogMaterialBindingDiagnostics() && shader != nullptr && shader->path.find("Skybox") != std::string::npos)
						NLS_LOG_INFO("[SkyboxMaterial] Texture entry \"" + entry.name + "\" GetOrCreateExplicitTextureView returned null, skipping");
					continue;
				}

				if (ShouldLogMaterialBindingDiagnostics() && shader != nullptr && shader->path.find("Skybox") != std::string::npos)
					NLS_LOG_INFO("[SkyboxMaterial] Texture entry \"" + entry.name + "\" added to binding set");
				break;
			}
			case RHI::BindingType::Sampler:
			{
				const auto defaultSampler = BuildDefaultSamplerDesc(entry.name);
				const auto* sampler = state.bindingSet.GetSampler(entry.name);
				bindingEntry.sampler = device->CreateSampler(
					sampler != nullptr ? *sampler : defaultSampler,
					entry.name + "Sampler");
				if (bindingEntry.sampler == nullptr)
				{
					addBindingDiagnostic(
						MaterialBindingDiagnosticSeverity::Error,
						entry.name,
						"Material binding \"" + entry.name + "\" resolved to a null sampler descriptor.");
					continue;
				}
				break;
			}
			default:
				continue;
			}

			bindingSetDesc.entries.push_back(std::move(bindingEntry));
		}

		if (ShouldLogMaterialBindingDiagnostics() && shader != nullptr && shader->path.find("Skybox") != std::string::npos)
			NLS_LOG_INFO("[SkyboxMaterial] Explicit binding set entry count = " + std::to_string(bindingSetDesc.entries.size()));

		state.explicitBindingSet = device->CreateBindingSet(bindingSetDesc);
		if (state.explicitBindingSet != nullptr)
			++state.explicitBindingSetCreationCount;
		state.explicitBindingSetsByShaderKey[cacheKey] = state.explicitBindingSet;
		state.explicitBindingSetDirty = false;
		return state.explicitBindingSet;
	}

	const std::shared_ptr<RHI::RHIPipelineLayout>& Material::GetExplicitPipelineLayout(const std::shared_ptr<RHI::RHIDevice>& device) const
	{
		return GetExplicitPipelineLayout(device, m_shader);
	}

	const std::shared_ptr<RHI::RHIPipelineLayout>& Material::GetExplicitPipelineLayout(
		const std::shared_ptr<RHI::RHIDevice>& device,
		const Shader* effectiveShader) const
	{
		EnsureShaderGenerationCacheCurrent();
		auto& state = GetRuntimeState();
		const auto* shader = effectiveShader != nullptr ? effectiveShader : m_shader;
		const auto shaderInstanceId = shader != nullptr ? shader->GetInstanceId() : 0u;
		const auto shaderGeneration = shader != nullptr ? shader->GetGeneration() : 0u;
		auto backend = ResolveDeviceBackendType(device);
        const auto deviceIdentity = ResolveDeviceCacheIdentity(device);
		if (device != nullptr)
		{
			const auto nativeInfo = device->GetNativeDeviceInfo();
			if (nativeInfo.backend != RHI::NativeBackendType::None)
				backend = nativeInfo.backend;
		}
		const auto cacheKey = BuildExplicitShaderCacheKey(device, shader);
		if (const auto cached = state.explicitPipelineLayoutsByShaderKey.find(cacheKey);
			cached != state.explicitPipelineLayoutsByShaderKey.end())
		{
			state.explicitPipelineLayout = cached->second;
			state.explicitPipelineLayoutDeviceIdentity = deviceIdentity;
			state.explicitPipelineLayoutBackend = backend;
			state.explicitPipelineLayoutShaderInstanceId = shaderInstanceId;
			state.explicitPipelineLayoutShaderGeneration = shaderGeneration;
			return state.explicitPipelineLayout;
		}
		PruneStaleExplicitShaderGenerationEntries(shaderInstanceId, shaderGeneration);
		if (state.explicitPipelineLayout != nullptr &&
            state.explicitPipelineLayoutDeviceIdentity == deviceIdentity &&
            state.explicitPipelineLayoutBackend == backend &&
			state.explicitPipelineLayoutShaderInstanceId == shaderInstanceId &&
			state.explicitPipelineLayoutShaderGeneration == shaderGeneration)
			return state.explicitPipelineLayout;

		state.explicitPipelineLayout.reset();
        state.explicitPipelineLayoutDeviceIdentity = deviceIdentity;
		state.explicitPipelineLayoutBackend = backend;
		state.explicitPipelineLayoutShaderInstanceId = shaderInstanceId;
		state.explicitPipelineLayoutShaderGeneration = shaderGeneration;
		if (device == nullptr)
		{
			state.explicitPipelineLayoutsByShaderKey[cacheKey] = state.explicitPipelineLayout;
			return state.explicitPipelineLayout;
		}

		if (shader == nullptr)
		{
			state.explicitPipelineLayoutsByShaderKey[cacheKey] = state.explicitPipelineLayout;
			return state.explicitPipelineLayout;
		}

#if defined(NLS_ENABLE_TEST_HOOKS)
		++state.indexedObjectDataShaderValidationCount;
#endif
		const auto objectDrawConstantsValidation = ValidateObjectDrawConstants(*shader);
		const auto indexedObjectDataValidation = ValidateIndexedObjectDataShader(*shader);
		const bool hasIncompatibleObjectDrawConstants =
			objectDrawConstantsValidation.status == ObjectDrawConstantsStatus::Incompatible;
		if (hasIncompatibleObjectDrawConstants ||
			indexedObjectDataValidation.status == IndexedObjectDataShaderStatus::Incompatible)
		{
			const auto& diagnostic = hasIncompatibleObjectDrawConstants
				? objectDrawConstantsValidation.diagnostic
				: indexedObjectDataValidation.diagnostic;
			const auto existingDiagnostic = std::find_if(
				state.indexedObjectDataBindingDiagnosticSources.begin(),
				state.indexedObjectDataBindingDiagnosticSources.end(),
				[shaderInstanceId, shaderGeneration](const IndexedObjectDataBindingDiagnosticSource& source)
				{
					return source.shaderInstanceId == shaderInstanceId &&
						source.shaderGeneration == shaderGeneration;
				});
			if (existingDiagnostic == state.indexedObjectDataBindingDiagnosticSources.end())
			{
				state.indexedObjectDataBindingDiagnosticSources.push_back({
					shaderInstanceId,
					shaderGeneration,
					diagnostic
				});
			}
			else
			{
				existingDiagnostic->message = diagnostic;
			}
			RefreshIndexedObjectDataBindingDiagnostics(
				state.explicitBindingDiagnostics,
				state.indexedObjectDataBindingDiagnosticSources);
			state.explicitPipelineLayoutsByShaderKey[cacheKey] = state.explicitPipelineLayout;
			return state.explicitPipelineLayout;
		}

		const auto layoutDescs = BuildExplicitBindingLayoutDescs(
			*shader,
			path.empty() ? "Material" : path);
		if (layoutDescs.empty())
		{
			state.explicitPipelineLayoutsByShaderKey[cacheKey] = state.explicitPipelineLayout;
			return state.explicitPipelineLayout;
		}

		const bool requiresIndexedObjectData = indexedObjectDataValidation.IsCompatible();
		if (requiresIndexedObjectData && !BackendSupportsIndexedObjectDataPushConstants(backend))
		{
			state.explicitPipelineLayoutsByShaderKey[cacheKey] = state.explicitPipelineLayout;
			return state.explicitPipelineLayout;
		}

		RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
		pipelineLayoutDesc.debugName = path.empty() ? "MaterialPipelineLayout" : path + ":MaterialPipelineLayout";
		if (objectDrawConstantsValidation.IsCompatible() &&
			BackendSupportsIndexedObjectDataPushConstants(backend))
		{
			pipelineLayoutDesc.pushConstants.push_back({
				kIndexedObjectDataPushConstantStageMask,
				0u,
				kIndexedObjectDataPushConstantSize,
				1u,
				NLS::Render::RHI::BindingPointMap::kObjectBindingSpace
			});
		}
		pipelineLayoutDesc.bindingLayouts.reserve(layoutDescs.size());
		for (const auto& layoutDesc : layoutDescs)
		{
			if (layoutDesc.entries.empty())
				continue;

			pipelineLayoutDesc.bindingLayouts.push_back(device->CreateBindingLayout(layoutDesc));
		}
		state.explicitPipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
		state.explicitPipelineLayoutsByShaderKey[cacheKey] = state.explicitPipelineLayout;
		return state.explicitPipelineLayout;
	}

	const std::vector<MaterialBindingDiagnostic>& Material::GetLastExplicitBindingDiagnostics() const
	{
		return GetRuntimeState().explicitBindingDiagnostics;
	}

	bool Material::HasExplicitBindingErrors() const
	{
		const auto& diagnostics = GetRuntimeState().explicitBindingDiagnostics;
		return std::any_of(
			diagnostics.begin(),
			diagnostics.end(),
			[](const MaterialBindingDiagnostic& diagnostic)
			{
				return diagnostic.severity == MaterialBindingDiagnosticSeverity::Error;
			});
	}

	bool Material::RequiresPassDescriptorSet() const
	{
		return RequiresPassDescriptorSet(m_shader);
	}

	bool Material::RequiresPassDescriptorSet(const Shader* effectiveShader) const
	{
		EnsureShaderGenerationCacheCurrent();
		const auto* shader = effectiveShader != nullptr ? effectiveShader : m_shader;
		if (shader == nullptr)
			return false;

		auto& state = GetRuntimeState();
		const auto shaderInstanceId = shader->GetInstanceId();
		const auto shaderGeneration = shader->GetGeneration();
		if (const auto cached = state.passDescriptorSetRequirementsByShader.find(shaderInstanceId);
			cached != state.passDescriptorSetRequirementsByShader.end() &&
			cached->second.first == shaderGeneration)
		{
			return cached->second.second;
		}

		bool requiresPassDescriptorSet = false;

		if (shader->HasParameterStructs())
		{
			const auto layoutDescs = BuildExplicitBindingLayoutDescs(
				*shader,
				path.empty() ? "Material" : path);
			constexpr uint32_t passSetIndex = NLS::Render::RHI::BindingPointMap::kPassDescriptorSet;
			requiresPassDescriptorSet =
				layoutDescs.size() > passSetIndex && !layoutDescs[passSetIndex].entries.empty();
		}
		else
		{
			const auto reflection = shader->GetReflectionSnapshot();
			for (const auto& constantBuffer : reflection->constantBuffers)
			{
				if (constantBuffer.bindingSpace == NLS::Render::RHI::BindingPointMap::kPassBindingSpace)
				{
					requiresPassDescriptorSet = true;
					break;
				}
			}

			if (!requiresPassDescriptorSet)
			{
				for (const auto& property : reflection->properties)
				{
					if (property.bindingSpace == NLS::Render::RHI::BindingPointMap::kPassBindingSpace)
					{
						requiresPassDescriptorSet = true;
						break;
					}
				}
			}
		}

		state.passDescriptorSetRequirementsByShader[shaderInstanceId] = {
			shaderGeneration,
			requiresPassDescriptorSet
		};
		return requiresPassDescriptorSet;
	}

	void Material::SetRawParameter(const std::string& name, std::any value)
	{
		m_parameterBlock.Set(name, std::move(value));
		InvalidateExplicitBindingSetCache();
	}

	void Material::MarkParametersDirty()
	{
		m_parameterBlock.MarkDirty();
		InvalidateExplicitBindingSetCache();
	}

#if defined(NLS_ENABLE_TEST_HOOKS)
	uint64_t Material::GetCachedShaderGenerationForTesting() const
	{
		return GetRuntimeState().shaderGeneration;
	}

	uint64_t Material::GetIndexedObjectDataShaderValidationCountForTesting() const
	{
		return GetRuntimeState().indexedObjectDataShaderValidationCount;
	}

	Material::ExplicitShaderCacheEntryCountsForTesting Material::GetExplicitShaderCacheEntryCountsForTesting(
		const uint64_t shaderInstanceId,
		const uint64_t shaderGeneration) const
	{
		const auto& state = GetRuntimeState();
		auto countEntries = [shaderInstanceId, shaderGeneration](const auto& entries)
		{
			return static_cast<size_t>(std::count_if(
				entries.begin(),
				entries.end(),
				[shaderInstanceId, shaderGeneration](const auto& entry)
				{
					return entry.first.shaderInstanceId == shaderInstanceId &&
						entry.first.shaderGeneration == shaderGeneration;
				}));
		};
		return {
			countEntries(state.explicitBindingLayoutsByShaderKey),
			countEntries(state.explicitBindingSetsByShaderKey),
			countEntries(state.explicitPipelineLayoutsByShaderKey)
		};
	}

	size_t Material::GetExplicitBindingDiagnosticCountForTesting(
		const uint64_t shaderInstanceId,
		const uint64_t shaderGeneration) const
	{
		const auto& diagnostics = GetRuntimeState().indexedObjectDataBindingDiagnosticSources;
		return static_cast<size_t>(std::count_if(
			diagnostics.begin(),
			diagnostics.end(),
			[shaderInstanceId, shaderGeneration](const IndexedObjectDataBindingDiagnosticSource& diagnostic)
			{
				return diagnostic.shaderInstanceId == shaderInstanceId &&
					diagnostic.shaderGeneration == shaderGeneration;
			}));
	}
#endif

	void Material::SetTextureResourcePath(const std::string& name, std::string path)
	{
		if (name.empty())
			return;

		if (path.empty())
		{
			ClearTextureResourcePath(name);
			return;
		}

		if (const auto found = m_textureResourcePaths.find(name);
			found != m_textureResourcePaths.end() &&
			found->second == path)
		{
			return;
		}

		m_textureResourcePaths[name] = std::move(path);
		InvalidateExplicitBindingSetCache();
	}

	void Material::ClearTextureResourcePath(const std::string& name)
	{
		if (m_textureResourcePaths.erase(name) > 0u)
			InvalidateExplicitBindingSetCache();
	}

	std::string Material::GetTextureResourcePath(const std::string& name) const
	{
		const auto found = m_textureResourcePaths.find(name);
		return found != m_textureResourcePaths.end() ? found->second : std::string {};
	}

	const std::map<std::string, std::string>& Material::GetTextureResourcePaths() const
	{
		return m_textureResourcePaths;
	}

	void Material::SetSamplerOverride(const std::string& name, const RHI::SamplerDesc& sampler)
	{
		if (name.empty())
			return;

		m_samplerOverrides[name] = sampler;
		InvalidateExplicitBindingSetCache();
	}

	void Material::ClearSamplerOverride(const std::string& name)
	{
		if (m_samplerOverrides.erase(name) > 0u)
			InvalidateExplicitBindingSetCache();
	}

	void Material::ClearSamplerOverrides()
	{
		if (m_samplerOverrides.empty())
			return;

		m_samplerOverrides.clear();
		InvalidateExplicitBindingSetCache();
	}

	const RHI::SamplerDesc* Material::GetSamplerOverride(const std::string& name) const
	{
		const auto found = m_samplerOverrides.find(name);
		return found != m_samplerOverrides.end() ? &found->second : nullptr;
	}

	const std::map<std::string, RHI::SamplerDesc>& Material::GetSamplerOverrides() const
	{
		return m_samplerOverrides;
	}

	uint64_t Material::GetInstanceId() const
	{
		return m_instanceId;
	}

	uint64_t Material::GetParameterRevision() const
	{
		return m_parameterBlock.GetRevision();
	}

	uint64_t Material::GetRenderStateRevision() const
	{
		return m_renderStateRevision;
	}

	uint64_t Material::GetBindingRevision() const
	{
		return m_bindingRevision;
	}

	uint64_t Material::GetExplicitBindingSetCreationCount() const
	{
		return GetRuntimeState().explicitBindingSetCreationCount;
	}

	uint64_t Material::GetExplicitSnapshotBufferCreationCount() const
	{
		return GetRuntimeState().explicitSnapshotBufferCreationCount;
	}

	const std::map<std::string, std::any>& Material::GetUniformsData() const
	{
		return m_parameterBlock.Data();
	}
}
