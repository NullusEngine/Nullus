#pragma once

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
        void RenderLineCommand(const LineDrawCommand& command, Resources::Mesh& mesh, PipelineState pso);

    private:
        Resources::Shader* m_primitiveShader = nullptr;
        Resources::Mesh* m_pointMesh = nullptr;
        Resources::Mesh* m_lineMesh = nullptr;
        Resources::Mesh* m_triangleMesh = nullptr;
        bool m_ownsPrimitiveShader = false;
        std::unique_ptr<Resources::Material> m_primitiveMaterial;
        std::vector<LineMeshSlot> m_lineMeshSlots;
        size_t m_nextLineMeshSlot = 0u;
        std::vector<Geometry::Vertex> m_lineBatchVertices;
        std::vector<DrawCommand> m_drawCommands;
    };

#if defined(NLS_ENABLE_TEST_HOOKS)
    struct NLS_RENDER_API DebugDrawPassTestAccess final
    {
        static const Resources::Mesh* GetLineMeshSlotMesh(const DebugDrawPass& pass, size_t slotIndex);
        static uint32_t GetLineMeshSlotCapacity(const DebugDrawPass& pass, size_t slotIndex);
    };
#endif
}
