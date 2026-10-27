#pragma once

#include <functional>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "Rendering/Resources/MaterialParameterSchema.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Assets
{
struct NLS_RENDER_API MaterialAssetReference
{
    std::string guid;
    std::string subAssetKey;
    std::string path;
    bool Empty() const { return guid.empty() && path.empty(); }
    std::string Key() const { return (guid.empty() ? path : guid) + "#" + subAssetKey; }
    bool operator==(const MaterialAssetReference&) const = default;
};

struct MaterialParameterOverride
{
    Resources::MaterialParameterId id = 0;
    std::string name;
    Resources::UniformType type = Resources::UniformType::UNIFORM_FLOAT;
    Resources::MaterialParameterValue value;
};

struct NLS_RENDER_API MaterialAsset
{
    uint32_t version = 2;
    MaterialAssetReference identity;
    MaterialAssetReference shader;
    MaterialAssetReference parent;
    std::vector<MaterialParameterOverride> parameters;
    std::map<std::string, bool> staticParameters;
    std::map<std::string, std::string> renderStateOverrides;
    std::map<std::string, RHI::SamplerDesc> samplerOverrides;
    bool Validate(std::string& error) const;
};

struct ResolvedMaterialAsset
{
    MaterialAssetReference shader;
    std::vector<MaterialAssetReference> ancestry;
    std::map<std::string, MaterialParameterOverride> parameters;
    std::map<std::string, bool> staticParameters;
    std::map<std::string, std::string> renderStateOverrides;
    std::map<std::string, RHI::SamplerDesc> samplerOverrides;
    std::vector<MaterialParameterOverride> orphanParameters;
};

class NLS_RENDER_API MaterialAssetResolver
{
public:
    using Provider = std::function<std::optional<MaterialAsset>(const MaterialAssetReference&)>;
    static std::optional<ResolvedMaterialAsset> Resolve(const MaterialAsset& asset,
        const Provider& provider, std::string& error, const Resources::MaterialParameterSchema* schema = nullptr);
};

// Version 2 metadata accompanies a legacy-compatible flat projection. Only
// authoring data is persisted; GPU defaults and dynamic instance edits are not.
NLS_RENDER_API std::string SerializeMaterialAsset(const MaterialAsset& asset);
NLS_RENDER_API std::optional<MaterialAsset> DeserializeMaterialAsset(std::string_view payload, std::string& error);
NLS_RENDER_API std::string BuildResolvedMaterialPayload(const ResolvedMaterialAsset& asset);
NLS_RENDER_API bool SaveMaterialAsset(const MaterialAsset& asset, const std::filesystem::path& path, std::string& error);
}
