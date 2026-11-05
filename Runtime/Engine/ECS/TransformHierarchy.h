#pragma once
#include <Engine/EngineDef.h>
#include <ECS/Systems/SystemScheduler.h>
#include <array>

namespace NLS::Engine::ECSIntegration
{
struct LocalTransform
{
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> scale{1, 1, 1};
    bool operator==(const LocalTransform&) const = default;
};
struct Parent
{
    ECS::Entity entity;
    bool operator==(const Parent&) const = default;
};
struct WorldTransform
{
    // Existing Math::Matrix4 row-major layout, column-vector T * R * S.
    std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    uint64_t revision = 0;
    bool operator==(const WorldTransform&) const = default;
};
struct TransformHierarchyStatistics
{
    size_t entities = 0, roots = 0, levels = 0, recomputed = 0, staleParents = 0, jobs = 0;
};

class NLS_ENGINE_API TransformHierarchySystem final : public ECS::System
{
public:
    TransformHierarchySystem();
    ~TransformHierarchySystem() override;
    void OnCreate(ECS::World&) override;
    void OnUpdate(ECS::SystemContext&) override;
    void OnStop(ECS::World&) noexcept override;
    TransformHierarchyStatistics GetStatistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
NLS_ENGINE_API void RegisterTransformComponents(ECS::World& world);
NLS_ENGINE_API TransformHierarchySystem& RegisterTransformSystem(ECS::World& world,
    ECS::SystemPhase phase = ECS::SystemPhase::LateUpdate, std::string name = "Nullus.ECS.TransformHierarchy");
NLS_ENGINE_API void AddTransform(ECS::World& world, ECS::Entity entity, LocalTransform local = {}, ECS::Entity parent = {});
// Reparent at a structural safe point; keeps the local pose and rejects cycles.
NLS_ENGINE_API void SetTransformParent(ECS::World& world, ECS::Entity child, ECS::Entity parent);
}
