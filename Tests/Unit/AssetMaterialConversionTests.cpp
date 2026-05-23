#include <gtest/gtest.h>

#include "Rendering/Assets/MaterialConversion.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Loaders/MaterialLoader.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/Resources/MaterialResourceSet.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Assets/NativeArtifactContainer.h"
#include "Debug/Logger.h"
#include "Guid.h"
#include "Math/Vector4.h"

#include <algorithm>
#include <any>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using NLS::Render::Assets::ConvertedMaterialArtifact;
using NLS::Render::Assets::MaterialAlphaMode;
using NLS::Render::Assets::MaterialSourceModel;
using NLS::Render::Assets::MaterialTextureColorSpace;

const NLS::Render::Assets::ConvertedMaterialTextureSlot* FindSlot(
    const ConvertedMaterialArtifact& material,
    const std::string& slot)
{
    const auto found = std::find_if(
        material.textureSlots.begin(),
        material.textureSlots.end(),
        [&slot](const NLS::Render::Assets::ConvertedMaterialTextureSlot& candidate)
        {
            return candidate.slot == slot;
        });
    return found != material.textureSlots.end() ? &*found : nullptr;
}

const NLS::Render::Assets::ConvertedMaterialFactor* FindFactor(
    const ConvertedMaterialArtifact& material,
    const std::string& name)
{
    const auto found = std::find_if(
        material.factors.begin(),
        material.factors.end(),
        [&name](const NLS::Render::Assets::ConvertedMaterialFactor& candidate)
        {
            return candidate.name == name;
        });
    return found != material.factors.end() ? &*found : nullptr;
}

bool HasDiagnosticCode(
    const ConvertedMaterialArtifact& material,
    const std::string& code)
{
    return std::any_of(
        material.diagnostics.begin(),
        material.diagnostics.end(),
        [&code](const NLS::Render::Assets::MaterialConversionDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        });
}

class ScopedDriverService final
{
public:
    explicit ScopedDriverService(NLS::Render::Context::Driver& driver)
    {
        NLS::Core::ServiceLocator::Provide(driver);
    }

    ~ScopedDriverService()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
    }

    ScopedDriverService(const ScopedDriverService&) = delete;
    ScopedDriverService& operator=(const ScopedDriverService&) = delete;
};

class ScopedShaderManagerAssetPaths final
{
public:
    ScopedShaderManagerAssetPaths(
        const std::string& projectAssetsPath,
        const std::string& engineAssetsPath)
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
    }

    ~ScopedShaderManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
        NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath({});
    }

    ScopedShaderManagerAssetPaths(const ScopedShaderManagerAssetPaths&) = delete;
    ScopedShaderManagerAssetPaths& operator=(const ScopedShaderManagerAssetPaths&) = delete;
};

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

void WriteNativeArtifactTextFile(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const std::string& schemaName,
    const uint32_t schemaVersion,
    const std::string& contents)
{
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = artifactType;
    metadata.schemaName = schemaName;
    metadata.schemaVersion = schemaVersion;

    const auto payload = std::vector<uint8_t>(contents.begin(), contents.end());
    WriteBinaryFile(path, NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload));
}

NLS::Render::Resources::ShaderReflection MakeAlbedoMapShaderReflection()
{
    NLS::Render::Resources::ShaderReflection reflection;
    reflection.properties = {
        {
            "u_AlbedoMap",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::SampledTexture,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        }
    };
    return reflection;
}

NLS::Render::Resources::ShaderReflection MakeStandardPbrShaderReflection()
{
    auto reflection = MakeAlbedoMapShaderReflection();
    reflection.constantBuffers.push_back({
        "MaterialConstants",
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        32u,
        {
            {
                "u_Albedo",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
                0u,
                16u,
                1u
            },
            {
                "u_Metallic",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
                16u,
                4u,
                1u
            },
            {
                "u_Roughness",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
                20u,
                4u,
                1u
            }
        }
    });
    reflection.properties.push_back({
        "u_Albedo",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        0u,
        16u,
        "MaterialConstants"
    });
    reflection.properties.push_back({
        "u_Metallic",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        16u,
        4u,
        "MaterialConstants"
    });
    reflection.properties.push_back({
        "u_Roughness",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        0u,
        -1,
        1,
        20u,
        4u,
        "MaterialConstants"
    });
    reflection.properties.push_back({
        "u_NormalMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        1u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    reflection.properties.push_back({
        "u_MetallicMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        2u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    reflection.properties.push_back({
        "u_RoughnessMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        3u,
        -1,
        1,
        0u,
        0u,
        {}
    });
    return reflection;
}

NLS::Render::Assets::ShaderArtifact MakeShaderArtifact(
    std::string sourcePath,
    std::string subAssetKey,
    NLS::Render::Resources::ShaderReflection reflection)
{
    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = std::move(sourcePath);
    artifact.subAssetKey = std::move(subAssetKey);
    artifact.reflection = std::move(reflection);
    artifact.stages.push_back({
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
        "VSMain",
        "vs_6_0",
        {
            NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
            {1u, 2u, 3u, 4u},
            {},
            {},
            "test-vertex",
            "Library/Artifacts/shader-guid/shader.nshader"
        }
    });
    artifact.stages.push_back({
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {
            NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
            {5u, 6u, 7u, 8u},
            {},
            {},
            "test-pixel",
            "Library/Artifacts/shader-guid/shader.nshader"
        }
    });
    return artifact;
}

NLS::Render::Assets::ShaderArtifact MakeAlbedoMapShaderArtifact()
{
    return MakeShaderArtifact(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        "shader:StandardPBR",
        MakeAlbedoMapShaderReflection());
}

NLS::Render::Assets::ShaderArtifact MakeStandardPbrShaderArtifact()
{
    return MakeShaderArtifact(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        "shader:StandardPBR",
        MakeStandardPbrShaderReflection());
}

std::filesystem::path WriteStandardPbrShaderArtifact(const std::filesystem::path& root)
{
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
    WriteBinaryFile(shaderArtifactPath, NLS::Render::Assets::SerializeShaderArtifact(MakeStandardPbrShaderArtifact()));
    return shaderArtifactPath;
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}
}

TEST(AssetMaterialConversionTests, GltfPbrConversionMapsTextureSlotsFactorsSamplerAndAlpha)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "samplers": [
        { "wrapS": 33071, "wrapT": 10497, "minFilter": 9987, "magFilter": 9729 }
      ],
      "images": [
        { "uri": "BaseColor.png", "name": "BaseColor" },
        { "uri": "MetalRough.png", "name": "MetalRough" },
        { "uri": "Normal.png", "name": "Normal" },
        { "uri": "Occlusion.png", "name": "Occlusion" },
        { "uri": "Emissive.png", "name": "Emissive" }
      ],
      "textures": [
        { "source": 0, "sampler": 0 },
        { "source": 1, "sampler": 0 },
        { "source": 2, "sampler": 0 },
        { "source": 3, "sampler": 0 },
        { "source": 4, "sampler": 0 }
      ],
      "materials": [
        {
          "name": "HeroMaterial",
          "doubleSided": true,
          "alphaMode": "BLEND",
          "alphaCutoff": 0.4,
          "emissiveFactor": [0.1, 0.2, 0.3],
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.8, 0.7, 0.6, 0.5],
            "metallicFactor": 0.25,
            "roughnessFactor": 0.75,
            "baseColorTexture": { "index": 0 },
            "metallicRoughnessTexture": { "index": 1 }
          },
          "normalTexture": { "index": 2, "scale": 0.6 },
          "occlusionTexture": { "index": 3, "strength": 0.8 },
          "emissiveTexture": { "index": 4 }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1010101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_EQ(material.subAssetKey, "material:material/0");
    EXPECT_EQ(material.workflow, "metallic-roughness");
    EXPECT_TRUE(material.doubleSided);
    EXPECT_EQ(material.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_DOUBLE_EQ(material.alphaCutoff, 0.4);

    const auto* baseColor = FindSlot(material, "BaseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_EQ(baseColor->textureKey, "image/0");
    EXPECT_EQ(baseColor->textureResourcePath, "BaseColor.png");
    EXPECT_EQ(baseColor->colorSpace, MaterialTextureColorSpace::SRgb);
    EXPECT_EQ(baseColor->sampler.wrapS, "ClampToEdge");
    EXPECT_EQ(baseColor->sampler.wrapT, "Repeat");
    EXPECT_EQ(baseColor->sampler.minFilter, "LinearMipmapLinear");

    const auto* metalRough = FindSlot(material, "MetallicRoughness");
    ASSERT_NE(metalRough, nullptr);
    EXPECT_EQ(metalRough->colorSpace, MaterialTextureColorSpace::Linear);
    EXPECT_NE(FindSlot(material, "Normal"), nullptr);
    EXPECT_NE(FindSlot(material, "Occlusion"), nullptr);
    EXPECT_NE(FindSlot(material, "Emissive"), nullptr);

    const auto* baseFactor = FindFactor(material, "BaseColor");
    ASSERT_NE(baseFactor, nullptr);
    ASSERT_EQ(baseFactor->values.size(), 4u);
    EXPECT_DOUBLE_EQ(baseFactor->values[0], 0.8);
    EXPECT_DOUBLE_EQ(baseFactor->values[3], 0.5);
    EXPECT_DOUBLE_EQ(FindFactor(material, "Metallic")->scalar, 0.25);
    EXPECT_DOUBLE_EQ(FindFactor(material, "Roughness")->scalar, 0.75);
    EXPECT_DOUBLE_EQ(FindFactor(material, "NormalScale")->scalar, 0.6);
    EXPECT_DOUBLE_EQ(FindFactor(material, "OcclusionStrength")->scalar, 0.8);
    EXPECT_FALSE(material.serializedPayload.empty());
}

TEST(AssetMaterialConversionTests, GltfTextureUrisSerializeAsRuntimeResourcePaths)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "images": [
        { "uri": "textures/BaseColor.png", "name": "BaseColor" }
      ],
      "textures": [
        { "source": 0 }
      ],
      "materials": [
        {
          "name": "HeroMaterial",
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0 }
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1020101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        {std::filesystem::path("Models/Hero")});

    const auto* baseColor = FindSlot(material, "BaseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_EQ(baseColor->textureKey, "image/0");
    EXPECT_EQ(baseColor->textureResourcePath, "Models/Hero/textures/BaseColor.png");
    EXPECT_NE(
        material.serializedPayload.find("value=\"Models/Hero/textures/BaseColor.png\""),
        std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("resourcePath=\"Models/Hero/textures/BaseColor.png\""),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("value=\"image/0\""), std::string::npos);
}

TEST(AssetMaterialConversionTests, EmbeddedOrUnnamedGltfImagesDoNotSerializeVirtualImageKeysAsDiskPaths)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "images": [
        { "name": "EmbeddedBaseColor" }
      ],
      "textures": [
        { "source": 0 }
      ],
      "materials": [
        {
          "name": "EmbeddedMaterial",
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0 }
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1020202-0202-4202-8202-020202020202")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        {std::filesystem::path("Models/Hero")});

    const auto* baseColor = FindSlot(material, "BaseColor");
    ASSERT_NE(baseColor, nullptr);
    EXPECT_EQ(baseColor->textureKey, "image/0");
    EXPECT_TRUE(baseColor->textureResourcePath.empty());
    EXPECT_EQ(material.serializedPayload.find("value=\"image/0\""), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("resourcePath=\"image/0\""), std::string::npos);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadUsesRuntimeMaterialXmlSchema)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "HeroMaterial",
          "doubleSided": true,
          "alphaMode": "BLEND",
          "alphaCutoff": 0.4,
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.8, 0.7, 0.6, 0.5],
            "metallicFactor": 0.25,
            "roughnessFactor": 0.75
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_NE(material.serializedPayload.find("<root>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<shader>:Shaders/StandardPBR.hlsl</shader>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<blendable>true</blendable>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<backfaceCulling>false</backfaceCulling>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<depthWriting>false</depthWriting>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<alphaMode>Blend</alphaMode>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<alphaCutoff>0.400000</alphaCutoff>"), std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.800000 0.700000 0.600000 0.500000\"/>"),
        std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("<uniform name=\"u_Metallic\" type=\"float\" value=\"0.250000\"/>"),
        std::string::npos);
    EXPECT_NE(
        material.serializedPayload.find("<uniform name=\"u_Roughness\" type=\"float\" value=\"0.750000\"/>"),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("MATERIAL="), std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("TEXTURE_SLOT="), std::string::npos);
}

TEST(AssetMaterialConversionTests, MaterialConversionCanReferenceShaderArtifactHandle)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [{ "name": "HeroMaterial" }]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110101-0101-4101-8101-010101010102")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    NLS::Render::Assets::MaterialConversionContext context;
    context.shaderResourcePath = "Library/Artifacts/shader-guid/shader.nshader";
    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness,
        context);

    EXPECT_NE(
        material.serializedPayload.find("<shader>Library/Artifacts/shader-guid/shader.nshader</shader>"),
        std::string::npos);
    EXPECT_EQ(material.serializedPayload.find("<shader>:Shaders/StandardPBR.hlsl</shader>"), std::string::npos);
}

TEST(AssetMaterialConversionTests, ImportedModelMaterialsDefaultToDoubleSidedVisibility)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "OneSidedSource",
          "doubleSided": false
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1110202-0202-4202-8202-020202020202")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto material = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_NE(material.serializedPayload.find("<backfaceCulling>false</backfaceCulling>"), std::string::npos);
    EXPECT_NE(material.serializedPayload.find("<frontfaceCulling>false</frontfaceCulling>"), std::string::npos);
}

TEST(AssetMaterialConversionTests, EngineDefaultMaterialIsDoubleSidedForDeferredAssetVisibility)
{
    const auto defaultMaterialPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Materials/Default.mat";

    std::ifstream input(defaultMaterialPath, std::ios::binary);
    const std::string payload{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(payload.empty());
    EXPECT_NE(payload.find("<backfaceCulling>false</backfaceCulling>"), std::string::npos);
}

TEST(AssetMaterialConversionTests, ShaderReflectionFallsBackToRuntimeCompileBackendWhenLocatedDriverHasNoRhi)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_reflection_artifact_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);

    static NLS::Render::Settings::DriverSettings settings = []()
    {
        NLS::Render::Settings::DriverSettings driverSettings;
        driverSettings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        driverSettings.enableThreadedRendering = true;
        driverSettings.threadedFrameSlotCount = 1u;
        return driverSettings;
    }();
    static NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);

    EXPECT_NE(shader->GetUniformInfo("u_Albedo"), nullptr);
    EXPECT_FALSE(shader->GetReflection().constantBuffers.empty());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadLoadsAsRuntimeMaterialResource)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "materials": [
        {
          "name": "HeroMaterial",
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.2, 0.4, 0.6, 0.8],
            "metallicFactor": 0.3,
            "roughnessFactor": 0.7
          }
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("e1120101-0101-4101-8101-010101010101")),
        "Hero");
    ASSERT_EQ(scene.materials.size(), 1u);

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        MaterialSourceModel::GltfPbrMetallicRoughness);

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_converted_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    static NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto materialPath = root / "Hero.nmat";
    {
        std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
        output << converted.serializedPayload;
    }

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->GetShader(), shader);

    const auto* albedoValue = loaded->GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.2f);
    EXPECT_FLOAT_EQ(albedo.y, 0.4f);
    EXPECT_FLOAT_EQ(albedo.z, 0.6f);
    EXPECT_FLOAT_EQ(albedo.w, 0.8f);
    EXPECT_FLOAT_EQ(loaded->Get<float>("u_Metallic"), 0.3f);
    EXPECT_FLOAT_EQ(loaded->Get<float>("u_Roughness"), 0.7f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderResolvesShaderArtifactPayloadWithoutRuntimeSourceCompile)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shader_artifact_material_" + NLS::Guid::New().ToString());
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
    WriteNativeArtifactTextFile(
        shaderArtifactPath,
        NLS::Core::Assets::ArtifactType::Shader,
        "shader",
        1u,
        R"(NULLUS_IMPORTED_SHADER_ARTIFACT=1
SOURCE=Assets/Shaders/ArtifactShader.hlsl
SUB_ASSET=shader:ArtifactShader
TARGET_PLATFORM=editor
STAGE_BEGIN
STAGE=Vertex
TARGET=DXIL
ENTRY=VSMain
PROFILE=vs_6_0
STATUS=Succeeded
CACHE_KEY=test-vertex
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=01020304
STAGE_END
STAGE_BEGIN
STAGE=Pixel
TARGET=DXIL
ENTRY=PSMain
PROFILE=ps_6_0
STATUS=Succeeded
CACHE_KEY=test-pixel
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=05060708
STAGE_END
CBUFFER_BEGIN
NAME=MaterialConstants
STAGE=Pixel
SPACE=2
BINDING=0
BYTE_SIZE=64
MEMBER_BEGIN
NAME=u_Albedo
TYPE=vec4
BYTE_OFFSET=0
BYTE_SIZE=16
ARRAY_SIZE=1
MEMBER_END
CBUFFER_END
PROPERTY_BEGIN
NAME=u_Albedo
TYPE=vec4
KIND=Value
STAGE=Pixel
SPACE=2
BINDING=0
LOCATION=-1
ARRAY_SIZE=1
BYTE_OFFSET=0
BYTE_SIZE=16
PARENT_CBUFFER=MaterialConstants
PROPERTY_END
)");

    const auto materialPath = root / "Assets" / "Materials" / "ArtifactMaterial.nmat";
    WriteTextFile(
        materialPath,
        "<root>\n"
        "  <shader>Library/Artifacts/shader-guid/shader.nshader</shader>\n"
        "  <uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.250000 0.500000 0.750000 1.000000\"/>\n"
        "</root>\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        (root / "Assets").string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* loaded = NLS::Render::Resources::Loaders::MaterialLoader::Create(materialPath.string());
    ASSERT_NE(loaded, nullptr);
    ASSERT_NE(loaded->GetShader(), nullptr);
    const auto* albedoValue = loaded->GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.25f);
    EXPECT_FLOAT_EQ(albedo.y, 0.5f);
    EXPECT_FLOAT_EQ(albedo.z, 0.75f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    const auto* vertex = loaded->GetShader()->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    ASSERT_NE(vertex, nullptr);
    EXPECT_EQ(vertex->entryPoint, "VSMain");
    EXPECT_EQ(vertex->output.bytecode, std::vector<uint8_t>({1u, 2u, 3u, 4u}));

    const auto* pixel = loaded->GetShader()->FindCompiledArtifact(
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    ASSERT_NE(pixel, nullptr);
    EXPECT_EQ(pixel->entryPoint, "PSMain");
    EXPECT_EQ(pixel->output.bytecode, std::vector<uint8_t>({5u, 6u, 7u, 8u}));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loaded));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, ConvertedMaterialPayloadLoadsDeclaredTextureSamplers)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_load_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";
    const auto texturePath = root / "Textures" / "HeroBaseColor.png";
    WriteBinaryFile(texturePath, TinyPng());

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto textureResourcePath = texturePath.lexically_normal().generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + textureResourcePath + "\"/>\n"
        "</root>\n";
    WriteBinaryFile(materialPath, std::vector<uint8_t>(payload.begin(), payload.end()));

    auto* skippedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false});
    ASSERT_NE(skippedTextures, nullptr);
    const auto* skippedAlbedoMap = skippedTextures->GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(skippedAlbedoMap, nullptr);
    ASSERT_EQ(skippedAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*skippedAlbedoMap), nullptr);
    EXPECT_EQ(skippedTextures->GetTextureResourcePath("u_AlbedoMap"), textureResourcePath);
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(skippedTextures));

    auto* loadedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {true});
    ASSERT_NE(loadedTextures, nullptr);
    const auto* loadedAlbedoMap = loadedTextures->GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(loadedAlbedoMap, nullptr);
    ASSERT_EQ(loadedAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_NE(std::any_cast<NLS::Render::Resources::Texture2D*>(*loadedAlbedoMap), nullptr);
    EXPECT_EQ(loadedTextures->GetTextureResourcePath("u_AlbedoMap"), textureResourcePath);
    EXPECT_TRUE(textureManager.IsResourceRegistered(textureResourcePath));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(loadedTextures));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderKeepsDistinctTextureSlotsWhenTextureLoadingIsDeferred)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slots_deferred_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    const auto shaderArtifactPath = WriteStandardPbrShaderArtifact(root);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const std::string albedoPath = (root / "Textures" / "HeroBaseColor.ntex").lexically_normal().generic_string();
    const std::string normalPath = (root / "Textures" / "HeroNormal.ntex").lexically_normal().generic_string();
    const std::string metalRoughPath = (root / "Textures" / "HeroMetalRough.ntex").lexically_normal().generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + albedoPath + "\"/>\n"
        "  <uniform name=\"u_NormalMap\" type=\"sampler2D\" value=\"" + normalPath + "\"/>\n"
        "  <uniform name=\"u_MetallicMap\" type=\"sampler2D\" value=\"" + metalRoughPath + "\"/>\n"
        "  <uniform name=\"u_RoughnessMap\" type=\"sampler2D\" value=\"" + metalRoughPath + "\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, payload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);

    EXPECT_EQ(material->GetTextureResourcePath("u_AlbedoMap"), albedoPath);
    EXPECT_EQ(material->GetTextureResourcePath("u_NormalMap"), normalPath);
    EXPECT_EQ(material->GetTextureResourcePath("u_MetallicMap"), metalRoughPath);
    EXPECT_EQ(material->GetTextureResourcePath("u_RoughnessMap"), metalRoughPath);
    EXPECT_NE(material->GetTextureResourcePath("u_AlbedoMap"), material->GetTextureResourcePath("u_NormalMap"));
    EXPECT_FALSE(textureManager.IsResourceRegistered(albedoPath));
    EXPECT_FALSE(textureManager.IsResourceRegistered(normalPath));
    EXPECT_FALSE(textureManager.IsResourceRegistered(metalRoughPath));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(material));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderAppliesTextureSlotSamplerMetadataToRuntimeSampler)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slot_sampler_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::ShaderReflection reflection;
    reflection.properties = {
        {
            "u_AlbedoMap",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::SampledTexture,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        },
        {
            "u_LinearWrapSampler",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::Sampler,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        }
    };
    const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const std::string texturePath = (root / "Textures" / "HeroBaseColor.ntex")
        .lexically_normal()
        .generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + texturePath + "\"/>\n"
        "  <textureSlot name=\"BaseColor\" texture=\"image/0\" resourcePath=\"" + texturePath + "\""
        " colorSpace=\"SRgb\" wrapS=\"ClampToEdge\" wrapT=\"MirrorRepeat\" minFilter=\"Nearest\" magFilter=\"Nearest\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, payload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);

    const auto* sampler = material->GetBindingSet().GetSampler("u_LinearWrapSampler");
    ASSERT_NE(sampler, nullptr);
    EXPECT_EQ(sampler->wrapU, NLS::Render::RHI::TextureWrap::ClampToEdge);
    EXPECT_EQ(sampler->wrapV, NLS::Render::RHI::TextureWrap::MirrorRepeat);
    EXPECT_EQ(sampler->minFilter, NLS::Render::RHI::TextureFilter::Nearest);
    EXPECT_EQ(sampler->magFilter, NLS::Render::RHI::TextureFilter::Nearest);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(material));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialReloadClearsPreviousTextureSlotSamplerMetadata)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_texture_slot_sampler_reload_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::ShaderReflection reflection;
    reflection.properties = {
        {
            "u_AlbedoMap",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::SampledTexture,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        },
        {
            "u_LinearWrapSampler",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::Sampler,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        }
    };
    const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const std::string texturePath = (root / "Textures" / "HeroBaseColor.ntex")
        .lexically_normal()
        .generic_string();
    const std::string firstPayload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + texturePath + "\"/>\n"
        "  <textureSlot name=\"BaseColor\" texture=\"image/0\" resourcePath=\"" + texturePath + "\""
        " colorSpace=\"SRgb\" wrapS=\"ClampToEdge\" wrapT=\"ClampToEdge\" minFilter=\"Nearest\" magFilter=\"Nearest\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, firstPayload);

    auto* material = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, true});
    ASSERT_NE(material, nullptr);
    ASSERT_NE(material->GetBindingSet().GetSampler("u_LinearWrapSampler"), nullptr);
    EXPECT_EQ(
        material->GetBindingSet().GetSampler("u_LinearWrapSampler")->minFilter,
        NLS::Render::RHI::TextureFilter::Nearest);

    const std::string secondPayload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + texturePath + "\"/>\n"
        "</root>\n";
    WriteTextFile(materialPath, secondPayload);
    NLS::Render::Resources::Loaders::MaterialLoader::Reload(*material, materialPath.string(), {false, true});

    const auto* sampler = material->GetBindingSet().GetSampler("u_LinearWrapSampler");
    ASSERT_NE(sampler, nullptr);
    EXPECT_EQ(sampler->wrapU, NLS::Render::RHI::TextureWrap::Repeat);
    EXPECT_EQ(sampler->wrapV, NLS::Render::RHI::TextureWrap::Repeat);
    EXPECT_EQ(sampler->minFilter, NLS::Render::RHI::TextureFilter::Linear);
    EXPECT_EQ(sampler->magFilter, NLS::Render::RHI::TextureFilter::Linear);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(material));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderDoesNotWarnWhenTextureLoadingIsIntentionallyDeferred)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_deferred_material_texture_load_" + NLS::Guid::New().ToString());
    const auto materialPath = root / "Materials" / "Hero.nmat";
    const auto texturePath = root / "Textures" / "HeroBaseColor.png";
    WriteBinaryFile(texturePath, TinyPng());

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(
        "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto textureResourcePath = texturePath.lexically_normal().generic_string();
    const std::string payload =
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"" + textureResourcePath + "\"/>\n"
        "</root>\n";
    WriteBinaryFile(materialPath, std::vector<uint8_t>(payload.begin(), payload.end()));

    bool sawTextureFailureWarning = false;
    const auto listener = NLS::Debug::Logger::LogEvent +=
        [&sawTextureFailureWarning](const NLS::Debug::LogData& log)
        {
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING &&
                log.message.find("Material texture failed to load") != std::string::npos)
            {
                sawTextureFailureWarning = true;
            }
        };

    auto* skippedTextures = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false});

    NLS::Debug::Logger::LogEvent -= listener;

    ASSERT_NE(skippedTextures, nullptr);
    EXPECT_FALSE(sawTextureFailureWarning);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(skippedTextures));
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialLoaderCanKeepShaderResolutionCacheOnly)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_shader_deferred_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderPath = projectAssets / "Shaders" / "Cold.hlsl";
    const auto materialPath = root / "Assets" / "Materials" / "Cold.nmat";

    WriteTextFile(
        shaderPath,
        "struct VSOutput { float4 position : SV_Position; };\n"
        "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
        "    VSOutput output;\n"
        "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
        "    return output;\n"
        "}\n"
        "float4 PSMain(VSOutput input) : SV_Target0 { return float4(1.0f, 1.0f, 1.0f, 1.0f); }\n");
    WriteTextFile(
        materialPath,
        "<root>\n"
        "  <shader>Shaders/Cold.hlsl</shader>\n"
        "</root>\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);

    auto* deferred = NLS::Render::Resources::Loaders::MaterialLoader::Create(
        materialPath.string(),
        {false, false});

    ASSERT_NE(deferred, nullptr);
    EXPECT_EQ(deferred->GetShader(), nullptr);
    EXPECT_FALSE(shaderManager.IsResourceRegistered("Shaders/Cold.hlsl"));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::MaterialLoader::Destroy(deferred));
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialPrewarmDoesNotPoisonLaterShaderLoading)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_prewarm_reload_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
    const auto materialPath = root / "Library" / "Artifacts" / "material-guid" / "materials" / "Hero.nmat";

    WriteNativeArtifactTextFile(
        shaderArtifactPath,
        NLS::Core::Assets::ArtifactType::Shader,
        "shader",
        1u,
        R"(NULLUS_IMPORTED_SHADER_ARTIFACT=1
SOURCE=Assets/Shaders/Hero.hlsl
SUB_ASSET=shader:Hero
TARGET_PLATFORM=editor
STAGE_BEGIN
STAGE=Vertex
TARGET=DXIL
ENTRY=VSMain
PROFILE=vs_6_0
STATUS=Succeeded
CACHE_KEY=test-vertex
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=01020304
STAGE_END
STAGE_BEGIN
STAGE=Pixel
TARGET=DXIL
ENTRY=PSMain
PROFILE=ps_6_0
STATUS=Succeeded
CACHE_KEY=test-pixel
ARTIFACT_PATH=Library/Artifacts/shader-guid/shader.nshader
BYTECODE_HEX=05060708
STAGE_END
)");
    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>Library/Artifacts/shader-guid/shader.nshader</shader>\n"
        "</root>\n");

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    const auto resourcePath = std::filesystem::path("Library/Artifacts/material-guid/materials/Hero.nmat")
        .generic_string();
    EXPECT_EQ(materialManager.PrewarmArtifact(resourcePath), nullptr);
    EXPECT_FALSE(materialManager.IsResourceRegistered(resourcePath));
    EXPECT_FALSE(shaderManager.IsResourceRegistered("Library/Artifacts/shader-guid/shader.nshader"));

    auto* loaded = materialManager.GetResource(resourcePath, true);
    ASSERT_NE(loaded, nullptr);
    EXPECT_NE(loaded->GetShader(), nullptr);
    EXPECT_TRUE(shaderManager.IsResourceRegistered("Library/Artifacts/shader-guid/shader.nshader"));

    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, MaterialArtifactCanLoadShaderWhileDeferringTextures)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_artifact_deferred_textures_" + NLS::Guid::New().ToString());
    const auto projectAssets = root / "Assets";
    const auto materialPath = root / "Library" / "Artifacts" / "material-guid" / "materials" / "Hero.nmat";
    const auto texturePath = root / "Library" / "Artifacts" / "texture-guid" / "textures" / "BaseColor.ntex";

    WriteNativeArtifactTextFile(
        materialPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "<root>\n"
        "  <shader>:Shaders/StandardPBR.hlsl</shader>\n"
        "  <uniform name=\"u_AlbedoMap\" type=\"sampler2D\" value=\"Library/Artifacts/texture-guid/textures/BaseColor.ntex\"/>\n"
        "</root>\n");
    const auto shaderArtifactPath = root / "Library" / "Artifacts" / "shader-guid" / "shader.nshader";
    WriteBinaryFile(shaderArtifactPath, NLS::Render::Assets::SerializeShaderArtifact(MakeAlbedoMapShaderArtifact()));

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssets.string() + "/",
        "App/Assets/Engine/");
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
    ASSERT_NE(shader, nullptr);
    shaderManager.RegisterResource(":Shaders/StandardPBR.hlsl", shader);

    const auto resourcePath = std::filesystem::path("Library/Artifacts/material-guid/materials/Hero.nmat")
        .generic_string();
    const auto textureResourcePath = std::filesystem::path("Library/Artifacts/texture-guid/textures/BaseColor.ntex")
        .generic_string();
    auto* loaded = materialManager.LoadArtifactWithoutTextures(resourcePath);

    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(materialManager.IsResourceRegistered(resourcePath));
    EXPECT_NE(loaded->GetShader(), nullptr);
    EXPECT_EQ(loaded->GetTextureResourcePath("u_AlbedoMap"), textureResourcePath);
    EXPECT_FALSE(textureManager.IsResourceRegistered(textureResourcePath));
    const auto* albedoMap = loaded->GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(albedoMap, nullptr);
    ASSERT_EQ(albedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMap), nullptr);

    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderReadsImportedTextureArtifactPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_imported_texture_artifact_load_" + NLS::Guid::New().ToString());
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "BaseColor.ntex";

    const std::string header =
        "NULLUS_IMPORTED_TEXTURE_ARTIFACT=1\n"
        "URI=Textures/BaseColor.png\n"
        "MIME_TYPE=image/png\n"
        "BYTE_LENGTH=67\n"
        "PAYLOAD_BEGIN\n";
    auto bytes = std::vector<uint8_t>(header.begin(), header.end());
    const auto png = TinyPng();
    bytes.insert(bytes.end(), png.begin(), png.end());
    WriteBinaryFile(texturePath, bytes);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, 1u);
    EXPECT_EQ(texture->height, 1u);
    EXPECT_EQ(texture->path, texturePath.string());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureLoaderReadsNativeTextureArtifactWithoutEncodedPayload)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_native_texture_artifact_load_" + NLS::Guid::New().ToString());
    const auto texturePath = root / "Library" / "Artifacts" / "Hero" / "textures" / "NativeBaseColor.ntex";

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        }
    });
    artifact.mips.push_back({1u, 1u, 1u, 4u, 4u, {128u, 128u, 128u, 255u}});
    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    WriteBinaryFile(texturePath, bytes);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->width, 2u);
    EXPECT_EQ(texture->height, 2u);
    EXPECT_TRUE(texture->isMimapped);
    EXPECT_EQ(texture->path, texturePath.string());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    std::filesystem::remove_all(root);
}

TEST(AssetMaterialConversionTests, TextureArtifactSerializesNativeRgba8MipChain)
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        }
    });
    artifact.mips.push_back({
        1u,
        1u,
        1u,
        4u,
        4u,
        {128u, 128u, 128u, 255u}
    });

    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>('N'));
    EXPECT_EQ(bytes[1], static_cast<uint8_t>('L'));
    EXPECT_EQ(bytes[2], static_cast<uint8_t>('S'));
    EXPECT_EQ(bytes[3], static_cast<uint8_t>('A'));

    const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 2u);
    EXPECT_EQ(decoded->height, 2u);
    EXPECT_EQ(decoded->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(decoded->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    ASSERT_EQ(decoded->mips.size(), 2u);
    EXPECT_EQ(decoded->mips[0].width, 2u);
    EXPECT_EQ(decoded->mips[0].height, 2u);
    EXPECT_EQ(decoded->mips[0].rowPitch, 8u);
    EXPECT_EQ(decoded->mips[0].slicePitch, 16u);
    EXPECT_EQ(decoded->mips[0].pixels.size(), 16u);
    EXPECT_EQ(decoded->mips[1].width, 1u);
    EXPECT_EQ(decoded->mips[1].height, 1u);
    EXPECT_EQ(decoded->mips[1].pixels, (std::vector<uint8_t>{128u, 128u, 128u, 255u}));
}

TEST(AssetMaterialConversionTests, FbxAndObjChannelsMapOrDiagnoseParserExposedData)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e2010101-0101-4101-8101-010101010101"));
    scene.textures.push_back({"fbx/texture/diffuse", "Diffuse", "Diffuse.png", "image/png"});
    scene.textures.push_back({"fbx/texture/normal", "Normal", "Normal.png", "image/png"});
    scene.textures.push_back({"fbx/texture/metallic", "Metallic", "Metallic.png", "image/png"});
    scene.textures.push_back({"fbx/texture/roughness", "Roughness", "Roughness.png", "image/png"});
    scene.textures.push_back({"fbx/texture/opacity", "Opacity", "Opacity.png", "image/png"});
    scene.textures.push_back({"fbx/texture/emissive", "Emissive", "Emissive.png", "image/png"});
    scene.textures.push_back({"fbx/texture/specular", "Specular", "Specular.png", "image/png"});

    NLS::Render::Assets::ImportedSceneNamedRecord fbx;
    fbx.sourceKey = "fbx/material/HeroSurface";
    fbx.name = "HeroSurface";
    fbx.materialChannels.push_back({"diffuse", "fbx/texture/diffuse", {1.0, 0.8, 0.6}, false, 0.0});
    fbx.materialChannels.push_back({"normal", "fbx/texture/normal", {}, false, 0.0});
    fbx.materialChannels.push_back({"roughness", "fbx/texture/roughness", {}, true, 0.35});
    fbx.materialChannels.push_back({"metallic", "fbx/texture/metallic", {}, true, 0.45});
    fbx.materialChannels.push_back({"occlusion", {}, {}, true, 0.9});
    fbx.materialChannels.push_back({"opacity", "fbx/texture/opacity", {}, true, 0.6});
    fbx.materialChannels.push_back({"emissive", "fbx/texture/emissive", {0.1, 0.2, 0.3}, false, 0.0});
    fbx.materialChannels.push_back({"specular", "fbx/texture/specular", {0.7, 0.8, 0.9}, false, 0.0});
    fbx.materialChannels.push_back({"doubleSided", {}, {}, true, 1.0});

    const auto convertedFbx = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        fbx,
        MaterialSourceModel::FbxParserMaterial);
    EXPECT_EQ(convertedFbx.workflow, "parser-fbx");
    EXPECT_EQ(convertedFbx.alphaMode, MaterialAlphaMode::Blend);
    EXPECT_TRUE(convertedFbx.doubleSided);
    EXPECT_NE(FindSlot(convertedFbx, "BaseColor"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Normal"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Metallic"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Roughness"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Opacity"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Emissive"), nullptr);
    EXPECT_NE(FindSlot(convertedFbx, "Specular"), nullptr);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "Roughness")->scalar, 0.35);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "Metallic")->scalar, 0.45);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "OcclusionStrength")->scalar, 0.9);
    EXPECT_DOUBLE_EQ(FindFactor(convertedFbx, "Alpha")->scalar, 0.6);
    EXPECT_EQ(FindFactor(convertedFbx, "Emissive")->values.size(), 3u);
    EXPECT_EQ(FindFactor(convertedFbx, "Specular")->values.size(), 3u);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_MetallicMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Metallic.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_RoughnessMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Roughness.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_OpacityMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Opacity.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_EmissiveMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Emissive.png"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("u_SpecularMap"), std::string::npos);
    EXPECT_NE(convertedFbx.serializedPayload.find("Specular.png"), std::string::npos);
    EXPECT_NE(
        convertedFbx.serializedPayload.find("<uniform name=\"u_Emissive\" type=\"vec4\" value=\"0.100000 0.200000 0.300000 1.000000\"/>"),
        std::string::npos);
    EXPECT_NE(
        convertedFbx.serializedPayload.find("<uniform name=\"u_Specular\" type=\"vec4\" value=\"0.700000 0.800000 0.900000 1.000000\"/>"),
        std::string::npos);

    NLS::Render::Assets::ImportedSceneNamedRecord obj;
    obj.sourceKey = "mtl/material/BodyPaint";
    obj.name = "BodyPaint";
    obj.materialChannels.push_back({"diffuse", "fbx/texture/diffuse", {0.7, 0.6, 0.5}, false, 0.0});
    obj.materialChannels.push_back({"shininess", {}, {}, true, 64.0});
    obj.materialChannels.push_back({"illumination", {}, {}, true, 7.0});

    const auto convertedObj = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        obj,
        MaterialSourceModel::ObjMtl);
    EXPECT_EQ(convertedObj.workflow, "mtl");
    EXPECT_NE(FindSlot(convertedObj, "BaseColor"), nullptr);
    EXPECT_NE(FindFactor(convertedObj, "SpecularPower"), nullptr);
    EXPECT_TRUE(HasDiagnosticCode(convertedObj, "material-illumination-model-unsupported"));
}

TEST(AssetMaterialConversionTests, PbrShadersSampleNormalMapsWhenEnabled)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto read = [](const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    };

    const auto standardPbr = read(root / "App/Assets/Engine/Shaders/StandardPBR.hlsl");
    const auto deferredGBuffer = read(root / "App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");

    ASSERT_FALSE(standardPbr.empty());
    ASSERT_FALSE(deferredGBuffer.empty());
    EXPECT_NE(standardPbr.find("ComputeNormal"), std::string::npos);
    EXPECT_NE(standardPbr.find("u_NormalMap.Sample"), std::string::npos);
    EXPECT_NE(standardPbr.find("u_EnableNormalMapping > 0.5f"), std::string::npos);
    EXPECT_NE(deferredGBuffer.find("ComputeNormal"), std::string::npos);
    EXPECT_NE(deferredGBuffer.find("u_NormalMap.Sample"), std::string::npos);
    EXPECT_NE(deferredGBuffer.find("u_EnableNormalMapping > 0.5f"), std::string::npos);
}

TEST(AssetMaterialConversionTests, MissingAndUnsupportedTexturesProduceDiagnosticsWithColorSpacePolicy)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e3010101-0101-4101-8101-010101010101"));
    scene.textures.push_back({"image/normal", "PackedNormal", "PackedNormal.gif", "image/gif"});

    NLS::Render::Assets::ImportedSceneNamedRecord material;
    material.sourceKey = "material/broken";
    material.name = "Broken";
    material.baseColorTextureKey = "image/missing";
    material.normalTextureKey = "image/normal";

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        material,
        MaterialSourceModel::GltfPbrMetallicRoughness);

    EXPECT_TRUE(HasDiagnosticCode(converted, "material-missing-texture"));
    EXPECT_TRUE(HasDiagnosticCode(converted, "material-unsupported-texture-encoding"));
    const auto* normal = FindSlot(converted, "Normal");
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->colorSpace, MaterialTextureColorSpace::Linear);

    const auto payloads = NLS::Render::Assets::BuildMaterialArtifactPayloads({converted});
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads.front().subAssetKey, "material:material/broken");
    EXPECT_EQ(payloads.front().artifactType, NLS::Core::Assets::ArtifactType::Material);
}

TEST(AssetMaterialConversionTests, MaterialArtifactPayloadPathsAreSafeForArtifactWriter)
{
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e4010101-0101-4101-8101-010101010101"));
    NLS::Render::Assets::ConvertedMaterialArtifact material;
    material.subAssetKey = "material:material/body";
    material.displayName = "Body";
    material.serializedPayload = "MATERIAL=material:material/body\n";

    const auto payloads = NLS::Render::Assets::BuildMaterialArtifactPayloads({material});
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads.front().relativePath.generic_string().find(':'), std::string::npos);

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_material_payload_" + NLS::Guid::New().ToString());
    NLS::Core::Assets::ArtifactWriteRequest request;
    request.sourceAssetId = assetId;
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = material.subAssetKey;
    request.artifacts = payloads;

    const NLS::Core::Assets::ArtifactWriter writer(root / "staging", root / "committed");
    const auto result = writer.WriteAndCommit(request, nullptr);
    std::filesystem::remove_all(root);

    ASSERT_TRUE(result.committed);
    ASSERT_EQ(result.manifest.subAssets.size(), 1u);
    EXPECT_EQ(result.manifest.subAssets.front().subAssetKey, material.subAssetKey);
    EXPECT_NE(result.manifest.subAssets.front().artifactPath.find(".nmat"), std::string::npos);
}

TEST(AssetMaterialConversionTests, MaterialArtifactPayloadPathsPreserveDistinctSubAssetKeys)
{
    NLS::Render::Assets::ConvertedMaterialArtifact colonMaterial;
    colonMaterial.subAssetKey = "material:body";
    colonMaterial.serializedPayload = "MATERIAL=material:body\n";

    NLS::Render::Assets::ConvertedMaterialArtifact slashMaterial;
    slashMaterial.subAssetKey = "material/body";
    slashMaterial.serializedPayload = "MATERIAL=material/body\n";

    const auto payloads = NLS::Render::Assets::BuildMaterialArtifactPayloads({
        colonMaterial,
        slashMaterial
    });

    ASSERT_EQ(payloads.size(), 2u);
    EXPECT_NE(payloads[0].relativePath, payloads[1].relativePath);
    EXPECT_EQ(payloads[0].relativePath.generic_string(), "materials/material%3Abody.nmat");
    EXPECT_EQ(payloads[1].relativePath.generic_string(), "materials/material%2Fbody.nmat");
}
