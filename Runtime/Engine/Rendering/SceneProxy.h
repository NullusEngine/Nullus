#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Math/Matrix4.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <Rendering/Geometry/Bounds.h>
#include <Rendering/Geometry/BoundingSphere.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Settings/ELightType.h>
#include <Core/ResourceManagement/ResourceHandle.h>

#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
    struct LargeSceneSettings;

    struct SceneProxyHandle
    {
        uint64_t sceneId = 0u;
        uint32_t index = ~0u;
        uint32_t generation = 0u;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return sceneId != 0u && index != ~0u && generation != 0u;
        }

        [[nodiscard]] bool operator==(const SceneProxyHandle& other) const noexcept = default;
    };

    enum class SceneProxyKind : uint8_t
    {
        Primitive = 0u,
        Light,
        SkyBox
    };

    enum class SceneProxyCommandKind : uint8_t
    {
        Add = 0u,
        Remove,
        UpdateTransform,
        UpdatePayload,
        Recreate
    };

    struct SceneProxyTransformState
    {
        Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
        Maths::Vector3 worldPosition {};
        Maths::Quaternion worldRotation = Maths::Quaternion::Identity;
        Maths::Vector3 worldScale {1.0f, 1.0f, 1.0f};
        uint64_t transformRevision = 0u;
    };

    struct SceneProxyResourceBinding
    {
        using MeshHandle = NLS::Core::ResourceManagement::ResourceHandle<NLS::Render::Resources::Mesh>;
        using MaterialHandle = NLS::Core::ResourceManagement::ResourceHandle<NLS::Render::Resources::Material>;

        std::shared_ptr<MeshHandle> meshHandle;
        std::shared_ptr<MaterialHandle> materialHandle;
        NLS::Render::Resources::Mesh* mesh = nullptr;
        NLS::Render::Resources::Material* material = nullptr;
        std::string meshPath;
        std::string materialPath;
        std::string requestedMaterialPath;
        uint64_t meshContentRevision = 0u;
        uint64_t materialInstanceId = 0u;
        uint64_t requestedMaterialInstanceId = 0u;
        uint64_t materialParameterRevision = 0u;
        uint64_t materialRenderStateRevision = 0u;
        bool valid = false;
        bool hasExplicitMaterialPath = false;
        bool materialResolved = false;

        [[nodiscard]] bool operator==(const SceneProxyResourceBinding& other) const noexcept
        {
            return mesh == other.mesh &&
                material == other.material &&
                meshContentRevision == other.meshContentRevision &&
                materialInstanceId == other.materialInstanceId &&
                materialParameterRevision == other.materialParameterRevision &&
                materialRenderStateRevision == other.materialRenderStateRevision;
        }
    };

    struct SceneProxyLightData
    {
        NLS::Render::Settings::ELightType type = NLS::Render::Settings::ELightType::POINT;
        Maths::Vector3 color {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float range = 10.0f;
        float cutoff = 12.0f;
        float outerCutoff = 15.0f;
        Maths::Vector3 size {1.0f, 1.0f, 1.0f};
    };

    struct SceneProxyPayload
    {
        SceneProxyKind kind = SceneProxyKind::Primitive;
        uint64_t ownerInstanceId = 0u;
        uint64_t parentInstanceId = 0u;
        std::string sourceObjectKey;
        std::string largeSceneHLODMetadata;
        SceneProxyResourceBinding resources;
        std::optional<SceneProxyLightData> light;
        NLS::Render::Geometry::BoundingSphere modelBoundingSphere;
        NLS::Render::Geometry::BoundingSphere customBoundingSphere;
        NLS::Render::Geometry::Bounds modelBounds;
        Maths::Matrix4 userMatrix = Maths::Matrix4::Identity;
        uint32_t layer = 0u;
        uint64_t ownerRenderRevision = 0u;
        uint64_t meshRendererRenderRevision = 0u;
        uint64_t hlodScopeRootInstanceId = 0u;
        std::string hlodScopeMetadata;
        bool ownerAlive = false;
        bool ownerActive = false;
        bool transientRenderingSuppressed = false;
        bool distanceCullingEnabled = false;
        float minDrawDistance = 0.0f;
        float maxDrawDistance = 0.0f;
        uint8_t frustumBehaviour = 0u;
    };

    struct SceneProxyCommand
    {
        SceneProxyCommandKind kind = SceneProxyCommandKind::Add;
        SceneProxyHandle handle;
        SceneProxyPayload payload;
        SceneProxyTransformState transform;
    };

    struct SceneProxyUpdateBatch
    {
        uint64_t frameId = 0u;
        uint64_t sceneId = 0u;
        uint64_t sceneRevision = 0u;
        std::vector<SceneProxyCommand> commands;

        [[nodiscard]] bool Empty() const noexcept { return commands.empty(); }
    };

    struct SceneProxyExtractionOptions
    {
        NLS::Render::Resources::Material* defaultMaterial = nullptr;
        NLS::Render::Resources::Material* overrideMaterial = nullptr;
        bool requireExplicitMaterialTextures = false;
        bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;
        const LargeSceneSettings* largeSceneSettings = nullptr;
    };
}
