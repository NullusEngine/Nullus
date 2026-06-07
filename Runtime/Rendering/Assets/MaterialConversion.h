#pragma once

#include "Assets/ArtifactWriter.h"
#include "Rendering/Assets/ImportedScene.h"
#include "RenderDef.h"

#include <filesystem>
#include <unordered_map>
#include <string>
#include <vector>

namespace NLS::Render::Assets
{
enum class MaterialSourceModel
{
    GltfPbrMetallicRoughness,
    FbxParserMaterial,
    ObjMtl
};

enum class MaterialTextureColorSpace
{
    SRgb,
    Linear
};

enum class MaterialAlphaMode
{
    Opaque,
    Mask,
    Blend
};

struct MaterialSamplerState
{
    std::string wrapS = "Repeat";
    std::string wrapT = "Repeat";
    std::string minFilter = "Linear";
    std::string magFilter = "Linear";
};

struct ConvertedMaterialTextureSlot
{
    std::string slot;
    std::string textureKey;
    std::string textureResourcePath;
    MaterialTextureColorSpace colorSpace = MaterialTextureColorSpace::SRgb;
    MaterialSamplerState sampler;
};

struct ConvertedMaterialFactor
{
    std::string name;
    std::vector<double> values;
    bool hasScalar = false;
    double scalar = 0.0;
};

struct MaterialConversionDiagnostic
{
    std::string code;
    std::string message;
};

struct ConvertedMaterialArtifact
{
    std::string subAssetKey;
    std::string displayName;
    std::string workflow;
    bool doubleSided = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    double alphaCutoff = 0.5;
    std::vector<ConvertedMaterialTextureSlot> textureSlots;
    std::vector<ConvertedMaterialFactor> factors;
    std::vector<MaterialConversionDiagnostic> diagnostics;
    std::string serializedPayload;
};

struct MaterialConversionContext
{
    std::filesystem::path texturePathPrefix;
    std::unordered_map<std::string, std::filesystem::path> importedTextureArtifactPaths;
    std::string shaderResourcePath;
    bool ignoreFbxTexturedNeutralDiffuseTint = true;
};

NLS_RENDER_API ConvertedMaterialArtifact ConvertImportedSceneMaterial(
    const ImportedScene& scene,
    const ImportedSceneNamedRecord& material,
    MaterialSourceModel sourceModel);

NLS_RENDER_API ConvertedMaterialArtifact ConvertImportedSceneMaterial(
    const ImportedScene& scene,
    const ImportedSceneNamedRecord& material,
    MaterialSourceModel sourceModel,
    const MaterialConversionContext& context);

NLS_RENDER_API std::vector<NLS::Core::Assets::ArtifactPayload> BuildMaterialArtifactPayloads(
    const std::vector<ConvertedMaterialArtifact>& materials);
}
