#include "Assets/ArtifactManifest.h"

namespace NLS::Core::Assets
{
const ImportedArtifact* ArtifactManifest::FindPrimaryArtifact() const
{
    return FindSubAsset(primarySubAssetKey);
}

const ImportedArtifact* ArtifactManifest::FindSubAsset(const std::string& subAssetKey) const
{
    for (const auto& artifact : subAssets)
    {
        if (artifact.subAssetKey == subAssetKey)
            return &artifact;
    }
    return nullptr;
}
}
