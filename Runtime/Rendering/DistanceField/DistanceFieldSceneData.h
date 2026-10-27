#pragma once

#include <deque>
#include <span>
#include <unordered_map>
#include "Math/Matrix4.h"
#include "Rendering/DistanceField/DistanceFieldTypes.h"

namespace NLS::Render::DistanceField
{
    struct DistanceFieldObjectInput
    {
        uint64_t sceneId = 0;
        uint32_t index = 0, generation = 0;
        Maths::Matrix4 localToWorld = Maths::Matrix4::Identity;
        std::shared_ptr<const MeshDistanceFieldArtifact> asset;
        std::string meshArtifactPath;
        bool enabled = true;
        bool mostlyStatic = true;
    };
    struct DistanceFieldObject
    {
        DistanceFieldObjectInput input;
        Maths::Matrix4 worldToLocal;
        Maths::Vector3 boundsMin,boundsMax;
        float distanceScale = 0;
    };
    struct DistanceFieldDirtyBounds { Maths::Vector3 min,max; };
    NLS_RENDER_API std::vector<DistanceFieldObjectInput> GroupDistanceFieldObjects(std::vector<DistanceFieldObjectInput> objects);

    class NLS_RENDER_API DistanceFieldSceneData
    {
    public:
        explicit DistanceFieldSceneData(uint64_t sceneId = 0) : m_sceneId(sceneId) {}
        void Synchronize(std::span<const DistanceFieldObjectInput> objects);
        [[nodiscard]] const std::vector<DistanceFieldObject>& Objects() const { return m_objects; }
        [[nodiscard]] uint64_t Revision() const { return m_revision; }
        // False means the bounded history expired and the view must fully rebuild.
        bool ChangesSince(uint64_t revision,std::vector<DistanceFieldDirtyBounds>& bounds) const;
        DistanceFieldSample SampleReference(const Maths::Vector3& worldPosition,uint32_t mip = 2) const;
    private:
        uint64_t m_sceneId = 0,m_revision = 1;
        std::vector<DistanceFieldObject> m_objects;
        struct Change { uint64_t revision; std::vector<DistanceFieldDirtyBounds> bounds; };
        std::deque<Change> m_changes;
    };

    struct DistanceFieldDirtyPage
    {
        uint32_t tableIndex = 0, clipmap = 0, cache = 0, padding = 0;
        Maths::Vector3 min;
        float voxelSize = 0;
    };
    struct DistanceFieldClipmap
    {
        Maths::Vector3 min;
        float voxelSize = 0;
        std::array<int32_t,3> origin{};
    };
    class NLS_RENDER_API DistanceFieldViewState
    {
    public:
        static constexpr uint32_t GridResolution = 20;
        static constexpr uint32_t PagesPerClipmap = 8000;
        static constexpr uint32_t PageTableEntries = PagesPerClipmap * 4 * 2;
        std::array<DistanceFieldClipmap,4> clipmaps;
        std::vector<DistanceFieldDirtyPage> Update(const DistanceFieldSceneData& scene,
            Maths::Vector3 camera,bool forceRebuild = false);
        void Invalidate() { m_revision=0; }
        [[nodiscard]] uint64_t Revision() const { return m_revision; }
    private:
        uint64_t m_revision = 0;
    };
}
