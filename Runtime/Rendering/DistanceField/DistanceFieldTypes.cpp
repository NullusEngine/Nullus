#include "Rendering/DistanceField/DistanceFieldTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::Render::DistanceField
{
    bool ValidateDistanceFieldMip(const DistanceFieldMip& mip, std::string* error)
    {
        const auto fail = [&](const char* why) { if (error) *error = why; return false; };
        uint64_t count = 1;
        for (auto n : mip.grid)
        {
            if (!n || n > 128) return fail("Distance field grid dimension is out of range");
            count *= n;
        }
        if (!std::isfinite(mip.maxDistance) || mip.maxDistance <= 0 ||
            count != mip.indirection.size() || mip.bricks.size() % 512u)
            return fail("Distance field payload size or distance range is invalid");
        for (auto entry : mip.indirection)
            if (entry != OutsideBrick && entry != InsideBrick && entry >= mip.BrickCount())
                return fail("Distance field indirection points outside the brick payload");
        if (error) error->clear();
        return true;
    }

    namespace
    {
        float SampleDistance(const MeshDistanceFieldArtifact& asset, const Maths::Vector3& p,
            const DistanceFieldMip& mip)
        {
            uint32_t cell[3];
            float fraction[3];
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                const float size = asset.boundsMax.array[axis] - asset.boundsMin.array[axis];
                const float t = std::clamp((p.array[axis] - asset.boundsMin.array[axis]) / size,
                    0.0f, 1.0f) * mip.grid[axis] * BrickIntervals;
                cell[axis] = (std::min)(static_cast<uint32_t>(t / BrickIntervals), mip.grid[axis] - 1);
                fraction[axis] = t - cell[axis] * BrickIntervals;
            }
            const uint32_t entry = mip.indirection[(cell[2] * mip.grid[1] + cell[1]) * mip.grid[0] + cell[0]];
            if (entry == OutsideBrick) return mip.maxDistance;
            if (entry == InsideBrick) return -mip.maxDistance;
            uint32_t i[3]; float f[3];
            for (uint32_t a = 0; a < 3; ++a)
            {
                i[a] = (std::min)(static_cast<uint32_t>(fraction[a]), BrickIntervals - 1);
                f[a] = fraction[a] - i[a];
            }
            float value = 0;
            for (uint32_t z = 0; z < 2; ++z)
                for (uint32_t y = 0; y < 2; ++y)
                    for (uint32_t x = 0; x < 2; ++x)
                    {
                        const auto q = mip.bricks[uint64_t(entry) * 512 +
                            ((i[2] + z) * 8 + i[1] + y) * 8 + i[0] + x];
                        value += (float(q) / 255.0f * 2.0f - 1.0f) * mip.maxDistance *
                            (x ? f[0] : 1 - f[0]) * (y ? f[1] : 1 - f[1]) * (z ? f[2] : 1 - f[2]);
                    }
            return value;
        }
    }

    DistanceFieldSample SampleMeshDistanceField(const MeshDistanceFieldArtifact& asset,
        const Maths::Vector3& p, uint32_t index)
    {
        DistanceFieldSample result;
        if (index >= MipCount) return result;
        const auto& mip = asset.mips[index];
        if (mip.indirection.empty() || !mip.grid[0] || !mip.grid[1] || !mip.grid[2]) return result;
        float outsideSquared = 0;
        for (uint32_t a = 0; a < 3; ++a)
        {
            if (!std::isfinite(p.array[a]) || !(asset.boundsMax.array[a] > asset.boundsMin.array[a])) return result;
            const float delta = (std::max)({asset.boundsMin.array[a] - p.array[a],
                p.array[a] - asset.boundsMax.array[a], 0.0f});
            outsideSquared += delta * delta;
            result.voxelSize = (std::max)(result.voxelSize,
                (asset.boundsMax.array[a] - asset.boundsMin.array[a]) / (mip.grid[a] * BrickIntervals));
        }
        const float clampedDistance = SampleDistance(asset, p, mip);
        result.distance = outsideSquared > 0 ? std::sqrt(outsideSquared +
            (std::max)(clampedDistance, 0.0f) * (std::max)(clampedDistance, 0.0f)) : clampedDistance;
        result.valid = true;
        for (uint32_t a = 0; a < 3; ++a)
        {
            auto lo = p, hi = p;
            lo.array[a] -= result.voxelSize;
            hi.array[a] += result.voxelSize;
            result.gradient.array[a] = (SampleDistance(asset, hi, mip) - SampleDistance(asset, lo, mip)) /
                (2 * result.voxelSize);
        }
        return result;
    }
}
