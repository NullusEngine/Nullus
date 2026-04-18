#pragma once

#include <memory>

#include "Rendering/Core/ARenderPass.h"
#include "Rendering/Debug/DebugDrawTypes.h"

namespace NLS::Render::Resources
{
    class Material;
    class Mesh;
    class Shader;
}

namespace NLS::Render::Debug
{
    class NLS_RENDER_API DebugDrawPass : public Core::ARenderPass
    {
    public:
        explicit DebugDrawPass(Core::CompositeRenderer& renderer);
        ~DebugDrawPass() override;

    protected:
        void Draw(PipelineState pso) override;
        virtual void RenderPrimitive(const DebugDrawPrimitive& primitive, PipelineState pso);

    private:
        void RenderPoint(const DebugDrawPrimitive& primitive, PipelineState pso);
        void RenderLine(const DebugDrawPrimitive& primitive, PipelineState pso);
        void RenderTriangle(const DebugDrawPrimitive& primitive, PipelineState pso);
        void RenderWithMesh(
            const DebugDrawPrimitive& primitive,
            Resources::Mesh& mesh,
            Settings::EPrimitiveMode primitiveMode,
            PipelineState pso);

    private:
        Resources::Shader* m_primitiveShader = nullptr;
        Resources::Mesh* m_pointMesh = nullptr;
        Resources::Mesh* m_lineMesh = nullptr;
        Resources::Mesh* m_triangleMesh = nullptr;
        bool m_ownsPrimitiveShader = false;
        std::unique_ptr<Resources::Material> m_primitiveMaterial;
    };
}
