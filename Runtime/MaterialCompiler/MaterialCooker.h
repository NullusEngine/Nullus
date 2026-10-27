#pragma once
#include "MaterialShaderCompiler.h"
#include "Rendering/Assets/MaterialAsset.h"
#include "Assets/ArtifactWriter.h"

namespace NLS::MaterialCompiler
{
struct MaterialCookEntry
{
    Core::Assets::AssetId id;
    std::string path;
    Render::Assets::ResolvedMaterialAsset resolved;
};
struct MaterialCookTexture
{
    Core::Assets::AssetId id;
    std::string subAssetKey;
    std::vector<uint8_t> nativeArtifact;
};
struct MaterialCookOptions
{
    MaterialShaderCompileOptions compiler;
    std::filesystem::path outputRoot;
    Core::Assets::AssetId shaderId;
    std::string targetPlatform="editor";
    std::vector<Render::ShaderLab::ShaderLabKeywordSet> variantCollection;
    std::map<std::string,MaterialCookTexture> frozenTextures;
    std::function<std::optional<MaterialCookTexture>(std::string_view)> textureProvider;
};
struct MaterialCookResult
{
    std::vector<Core::Assets::ArtifactManifest> manifests;
    std::filesystem::path databasePath;
    std::string snapshotId;
    std::string error;
    bool Succeeded() const{return error.empty() && !databasePath.empty();}
};
// Cooks the actual resolved static assignments, plus an explicit collection.
// Authoring graphs/functions, sparse parents and source archives stay in the
// build cache. ArtifactDB is the final publication commit point.
MaterialCookResult CookMaterialFamily(const Render::ShaderLab::ShaderLabAssetDesc& shader,
    const Render::Resources::MaterialParameterSchema& schema,const std::vector<MaterialCookEntry>& materials,
    const MaterialCookOptions& options);
}
