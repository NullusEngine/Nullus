#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "Rendering/Core/ARenderPass.h"
#include "Rendering/Debug/DebugDrawTypes.h"
#include "Rendering/Geometry/Vertex.h"

namespace NLS::Render::Resources
{
    class Material;
    class Mesh;
    class Shader;
}

namespace NLS::Render::Debug
{
	class DebugDrawService;

#if defined(NLS_ENABLE_TEST_HOOKS)
    struct DebugDrawPassTestAccess;
#endif

    class NLS_RENDER_API DebugDrawPass : public Core::ARenderPass
    {
    public:
        struct LineSegment
        {
            Maths::Vector3 start;
            Maths::Vector3 end;
        };

        struct LineBatch
        {
            DebugDrawStyle style;
            std::vector<LineSegment> segments;
        };

        explicit DebugDrawPass(Core::CompositeRenderer& renderer);
        ~DebugDrawPass() override;

    protected:
        void Draw(PipelineState pso) override;
        virtual void RenderPrimitive(const DebugDrawPrimitive& primitive, PipelineState pso);
        virtual void RenderLineBatch(const LineBatch& batch, PipelineState pso);

    private:
#if defined(NLS_ENABLE_TEST_HOOKS)
        friend struct DebugDrawPassTestAccess;
#endif

        enum class DrawCommandType
        {
            Primitive,
            Line
        };

        struct LineDrawCommand
        {
            DebugDrawStyle style;
            uint32_t vertexStart = 0u;
            uint32_t vertexCount = 0u;
        };

        struct DrawCommand
        {
            DrawCommandType type = DrawCommandType::Primitive;
            DebugDrawPrimitive primitive;
            LineDrawCommand line;
        };

        struct LineMeshSlot
        {
            std::unique_ptr<Resources::Mesh> mesh;
            uint32_t capacity = 0u;
        };

        struct CachedLineMaterial
        {
            DebugDrawStyle style;
            std::unique_ptr<Resources::Material> material;
            bool hasStyle = false;
        };

        static constexpr size_t kMinLineMeshSlotCount = 3u;

        void RenderPoint(const DebugDrawPrimitive& primitive, PipelineState pso);
        void RenderLine(const DebugDrawPrimitive& primitive, PipelineState pso);
        void RenderTriangle(const DebugDrawPrimitive& primitive, PipelineState pso);
        void RenderWithMesh(
            const DebugDrawPrimitive& primitive,
            Resources::Mesh& mesh,
            Settings::EPrimitiveMode primitiveMode,
            PipelineState pso);
        void RenderCollectedCommands(PipelineState pso);
        Resources::Mesh* UploadLineVertices(const Geometry::BoundingSphere& boundingSphere);
        void RenderPrimitiveNow(const DebugDrawPrimitive& primitive, PipelineState pso);
        Resources::Material* ResolveLineMaterial(size_t lineCommandIndex, const DebugDrawStyle& style);
        void RenderLineCommand(
            const LineDrawCommand& command,
            Resources::Mesh& mesh,
            Resources::Material& material,
            PipelineState pso);

    private:
        Resources::Shader* m_primitiveShader = nullptr;
        Resources::Mesh* m_pointMesh = nullptr;
        Resources::Mesh* m_lineMesh = nullptr;
        Resources::Mesh* m_triangleMesh = nullptr;
        bool m_ownsPrimitiveShader = false;
        std::unique_ptr<Resources::Material> m_primitiveMaterial;
        std::vector<CachedLineMaterial> m_cachedLineMaterials;
        std::vector<LineMeshSlot> m_lineMeshSlots;
        size_t m_nextLineMeshSlot = 0u;
        std::vector<Geometry::Vertex> m_lineBatchVertices;
        std::vector<DrawCommand> m_drawCommands;
        std::vector<std::reference_wrapper<const DebugDrawPrimitive>> m_visiblePrimitives;
        const DebugDrawService* m_collectedPrimitiveService = nullptr;
        uint64_t m_collectedContentRevision = 0u;
        uint64_t m_commandBuildCount = 0u;
        const DebugDrawService* m_uploadedLineService = nullptr;
        Resources::Mesh* m_uploadedLineMesh = nullptr;
        std::vector<Geometry::Vertex> m_uploadedLineVertices;
        uint64_t m_uploadedLineContentRevision = 0u;
        uint64_t m_lineMeshUploadCount = 0u;
    };

#if defined(NLS_ENABLE_TEST_HOOKS)
    struct NLS_RENDER_API DebugDrawPassTestAccess final
    {
        static const Resources::Mesh* GetLineMeshSlotMesh(const DebugDrawPass& pass, size_t slotIndex);
        static uint32_t GetLineMeshSlotCapacity(const DebugDrawPass& pass, size_t slotIndex);
        static uint64_t GetCommandBuildCount(const DebugDrawPass& pass);
        static uint64_t GetLineMeshUploadCount(const DebugDrawPass& pass);
        static size_t GetCachedLineMaterialCount(const DebugDrawPass& pass);
        static uint64_t GetCachedLineMaterialParameterRevision(const DebugDrawPass& pass, size_t index);
    };
#endif
}
