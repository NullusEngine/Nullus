#include "Rendering/Assets/MeshReduction.h"

#include "Rendering/Geometry/BoundingSphereUtils.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace NLS::Render::Assets
{
namespace
{
constexpr uint32_t kMinimumTriangleCount = 1u;
constexpr float kMaximumRelativeError = 1.0f;
constexpr size_t kMaximumAttempts = 8u;

bool IsValidInput(const MeshArtifactData& mesh)
{
    if (mesh.vertices.empty() ||
        mesh.indices.empty() ||
        mesh.indices.size() % 3u != 0u)
    {
        return false;
    }

    for (const auto& vertex : mesh.vertices)
    {
        if (!std::isfinite(vertex.position[0]) ||
            !std::isfinite(vertex.position[1]) ||
            !std::isfinite(vertex.position[2]))
        {
            return false;
        }
    }

    return std::all_of(
        mesh.indices.begin(),
        mesh.indices.end(),
        [&mesh](const uint32_t index)
        {
            return index < mesh.vertices.size();
        });
}

std::optional<MeshArtifactData> CompactMesh(
    const MeshArtifactData& source,
    const uint32_t* indices,
    const size_t indexCount)
{
    if (indices == nullptr || indexCount == 0u || indexCount % 3u != 0u)
        return std::nullopt;

    constexpr uint32_t kUnmapped = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> remap(source.vertices.size(), kUnmapped);

    MeshArtifactData result;
    result.materialIndex = source.materialIndex;
    result.vertices.reserve((std::min)(source.vertices.size(), indexCount));
    result.indices.reserve(indexCount);

    for (size_t offset = 0u; offset < indexCount; offset += 3u)
    {
        const uint32_t first = indices[offset + 0u];
        const uint32_t second = indices[offset + 1u];
        const uint32_t third = indices[offset + 2u];
        if (first >= source.vertices.size() ||
            second >= source.vertices.size() ||
            third >= source.vertices.size() ||
            first == second ||
            first == third ||
            second == third)
        {
            continue;
        }

        for (const uint32_t sourceIndex : {first, second, third})
        {
            auto& compactedIndex = remap[sourceIndex];
            if (compactedIndex == kUnmapped)
            {
                compactedIndex = static_cast<uint32_t>(result.vertices.size());
                result.vertices.push_back(source.vertices[sourceIndex]);
            }
            result.indices.push_back(compactedIndex);
        }
    }

    if (result.indices.empty())
        return std::nullopt;

    result.boundingSphere = Geometry::ComputeBoundingSphere(result.vertices);
    result.hasBoundingSphere = true;
    return result;
}

std::optional<MeshArtifactData> BuildDeterministicSubset(
    const MeshArtifactData& source,
    const uint32_t targetTriangleCount)
{
    const size_t sourceTriangleCount = source.indices.size() / 3u;
    const uint32_t requestedTriangleCount = (std::min)(
        targetTriangleCount,
        static_cast<uint32_t>(sourceTriangleCount));
    if (requestedTriangleCount < kMinimumTriangleCount)
        return std::nullopt;

    std::vector<uint32_t> selected;
    selected.reserve(static_cast<size_t>(requestedTriangleCount) * 3u);
    for (uint32_t outputTriangle = 0u;
        outputTriangle < requestedTriangleCount;
        ++outputTriangle)
    {
        const auto sourceTriangle = static_cast<size_t>(
            (static_cast<uint64_t>(outputTriangle) * sourceTriangleCount) /
            requestedTriangleCount);
        const auto sourceOffset = sourceTriangle * 3u;
        selected.insert(
            selected.end(),
            source.indices.begin() + sourceOffset,
            source.indices.begin() + sourceOffset + 3u);
    }
    return CompactMesh(source, selected.data(), selected.size());
}
}

std::optional<MeshArtifactData> ReduceMeshArtifact(
    const MeshArtifactData& source,
    const uint32_t targetTriangleCount)
{
    if (targetTriangleCount == 0u || !IsValidInput(source))
        return std::nullopt;

    const auto sourceTriangleCount = source.indices.size() / 3u;
    if (targetTriangleCount >= sourceTriangleCount)
        return CompactMesh(source, source.indices.data(), source.indices.size());

    const auto targetIndexCount = static_cast<size_t>(targetTriangleCount) * 3u;
    std::vector<uint32_t> simplifiedIndices(source.indices.size());
    auto attemptTarget = targetIndexCount;
    for (size_t attempt = 0u;
        attempt < kMaximumAttempts && attemptTarget >= 3u;
        ++attempt)
    {
        const auto simplifiedCount = meshopt_simplify(
            simplifiedIndices.data(),
            source.indices.data(),
            source.indices.size(),
            source.vertices.front().position,
            source.vertices.size(),
            sizeof(Geometry::Vertex),
            attemptTarget,
            kMaximumRelativeError,
            meshopt_SimplifyLockBorder | meshopt_SimplifyRegularize,
            nullptr);
        if (simplifiedCount >= 3u && simplifiedCount <= targetIndexCount)
        {
            if (auto compacted = CompactMesh(
                    source,
                    simplifiedIndices.data(),
                    simplifiedCount))
            {
                return compacted;
            }
        }

        if (attemptTarget == 3u)
            break;
        attemptTarget = (std::max)(size_t{3u}, (attemptTarget * 3u) / 4u);
        attemptTarget -= attemptTarget % 3u;
    }

    return BuildDeterministicSubset(source, targetTriangleCount);
}
}
