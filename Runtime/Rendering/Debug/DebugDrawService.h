#pragma once

#include <functional>
#include <cstdint>
#include <vector>

#include "Rendering/Debug/DebugDrawTypes.h"

namespace NLS::Render::Debug
{
    class NLS_RENDER_API DebugDrawService
    {
    public:
        explicit DebugDrawService(size_t maxVisiblePrimitives = 4096u);

        bool SubmitPoint(
            const Maths::Vector3& position,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitPoint(
            const Maths::Vector3& position,
            const Maths::Vector3& color,
            float pointSize = 4.0f,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitLine(
            const Maths::Vector3& start,
            const Maths::Vector3& end,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitLine(
            const Maths::Vector3& start,
            const Maths::Vector3& end,
            const Maths::Vector3& color,
            float lineWidth = 1.0f,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitTriangle(
            const Maths::Vector3& a,
            const Maths::Vector3& b,
            const Maths::Vector3& c,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitTriangle(
            const Maths::Vector3& a,
            const Maths::Vector3& b,
            const Maths::Vector3& c,
            const Maths::Vector3& color,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitLine(
            const Data::PipelineState& pipelineState,
            const Maths::Vector3& start,
            const Maths::Vector3& end,
            const Maths::Vector3& color,
            float lineWidth,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitBox(
            const Data::PipelineState& pipelineState,
            const Maths::Vector3& position,
            const Maths::Quaternion& rotation,
            const Maths::Vector3& size,
            const Maths::Vector3& color,
            float lineWidth,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitSphere(
            const Data::PipelineState& pipelineState,
            const Maths::Vector3& position,
            const Maths::Quaternion& rotation,
            float radius,
            const Maths::Vector3& color,
            float lineWidth,
            const DebugDrawSubmitOptions& options = {});

        bool SubmitCapsule(
            const Data::PipelineState& pipelineState,
            const Maths::Vector3& position,
            const Maths::Quaternion& rotation,
            float radius,
            float height,
            const Maths::Vector3& color,
            float lineWidth,
            const DebugDrawSubmitOptions& options = {});

        void SetCategoryEnabled(DebugDrawCategory category, bool enabled);
        bool IsCategoryEnabled(DebugDrawCategory category) const;
        void SetEnabled(bool enabled);
        bool IsEnabled() const;

        bool SetPersistentPrimitiveGroup(uint64_t groupId, std::vector<DebugDrawPrimitive> primitives);
        bool RemovePersistentPrimitiveGroup(uint64_t groupId);
        uint64_t GetContentRevision() const;
        bool HasVisiblePrimitives() const;
        void CollectVisiblePrimitives(
            std::vector<std::reference_wrapper<const DebugDrawPrimitive>>& outPrimitives) const;
        std::vector<std::reference_wrapper<const DebugDrawPrimitive>> CollectVisiblePrimitives() const;
        std::vector<std::reference_wrapper<const DebugDrawPrimitive>> CollectVisibleLines() const;
        DebugDrawLimitState GetLimitState() const;
        size_t GetQueuedPrimitiveCount() const;
        size_t GetQueuedLineCount() const;
        size_t GetMaxVisibleLines() const;

        void EndFrame();
        void Clear();

    private:
        struct PrimitiveEntry
        {
            DebugDrawPrimitive primitive;
            uint64_t persistentGroupId = 0u;
        };

        bool CanReserve(size_t additionalPrimitiveCount);
        void UpdateLimitState();
        bool SubmitPrimitive(DebugDrawPrimitive primitive);
        static DebugDrawSubmitOptions WithColorAndLineWidth(
            DebugDrawSubmitOptions options,
            const Maths::Vector3& color,
            float lineWidth);
        static DebugDrawSubmitOptions WithColorAndPointSize(
            DebugDrawSubmitOptions options,
            const Maths::Vector3& color,
            float pointSize);
        static size_t CategoryIndex(DebugDrawCategory category);

    private:
        size_t m_maxVisiblePrimitives = 0u;
        bool m_enabled = true;
        DebugDrawLimitState m_limitState = DebugDrawLimitState::WithinLimits;
        std::vector<bool> m_categoryVisibility;
        std::vector<PrimitiveEntry> m_primitives;
        uint64_t m_contentRevision = 1u;
    };
}
