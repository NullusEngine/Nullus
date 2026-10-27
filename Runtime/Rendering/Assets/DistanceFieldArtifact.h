#pragma once

#include <filesystem>
#include <optional>
#include "Assets/ArtifactManifest.h"
#include "Rendering/DistanceField/DistanceFieldTypes.h"

namespace NLS::Render::DistanceField
{
    NLS_RENDER_API std::string DistanceFieldSubAssetKey(const std::string& meshKey, uint32_t mip = MipCount);
    NLS_RENDER_API std::vector<uint8_t> SerializeDistanceFieldArtifact(const MeshDistanceFieldArtifact& asset);
    NLS_RENDER_API std::vector<uint8_t> SerializeDistanceFieldMip(const DistanceFieldMip& mip);
    NLS_RENDER_API std::optional<MeshDistanceFieldArtifact> DeserializeDistanceFieldArtifact(const std::vector<uint8_t>& bytes);
    NLS_RENDER_API std::optional<DistanceFieldMip> DeserializeDistanceFieldMip(const std::vector<uint8_t>& bytes);
    NLS_RENDER_API std::shared_ptr<MeshDistanceFieldArtifact> LoadDistanceFieldArtifact(const std::filesystem::path& path);
    NLS_RENDER_API std::optional<DistanceFieldMip> LoadDistanceFieldMip(const std::filesystem::path& path);

    struct MeshDistanceFieldSource
    {
        std::string mainPath;
        std::array<std::string, MipCount> mipPaths;
        std::string contentHash;
        std::string groupMainPath;
        std::array<std::string, MipCount> groupMipPaths;
        std::vector<std::string> groupMembers;
        bool operator==(const MeshDistanceFieldSource&) const = default;
    };
    NLS_RENDER_API void RegisterDistanceFieldManifest(const NLS::Core::Assets::ArtifactManifest& manifest,
        const std::filesystem::path& artifactRoot = {});
    NLS_RENDER_API std::optional<MeshDistanceFieldSource> FindMeshDistanceFieldSource(const std::string& meshArtifactPath);
    NLS_RENDER_API uint64_t GetDistanceFieldSourceRevision();
    NLS_RENDER_API bool HasDistanceFieldSources();
}
