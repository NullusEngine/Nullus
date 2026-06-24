#pragma once

#include "Assets/AssetId.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Math/Quaternion.h"
#include "Math/Vector3.h"
#include "Serialize/ObjectId.h"

#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
struct PreviewDrawItem
{
    NLS::Engine::Serialize::ObjectId sourceObject;
    NLS::Core::Assets::AssetId meshAssetId;
    std::string meshPath;
    std::vector<NLS::Core::Assets::AssetId> materialAssetIds;
    std::vector<std::string> materialPaths;
    NLS::Maths::Vector3 localPosition {0.0f, 0.0f, 0.0f};
    NLS::Maths::Quaternion localRotation {NLS::Maths::Quaternion::Identity};
    NLS::Maths::Vector3 localScale {1.0f, 1.0f, 1.0f};
};

struct PreviewRenderableSnapshot
{
    std::vector<PreviewDrawItem> drawItems;
};

PreviewRenderableSnapshot BuildPreviewRenderableSnapshot(
    const NLS::Engine::Assets::PrefabArtifact& prefab);
}
