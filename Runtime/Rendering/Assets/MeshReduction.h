#pragma once

#include "RenderDef.h"
#include "Rendering/Assets/MeshArtifact.h"

#include <cstdint>
#include <optional>

namespace NLS::Render::Assets
{
// Reduces a triangle mesh to at most targetTriangleCount triangles.
// The reducer preserves vertex attributes and the source material assignment.
NLS_RENDER_API std::optional<MeshArtifactData> ReduceMeshArtifact(
    const MeshArtifactData& source,
    uint32_t targetTriangleCount);
}
