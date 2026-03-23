

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ResourceManagement/ModelManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Debug/Logger.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "GameObject.h"

namespace NLS::Engine::Components
{
MeshRenderer::MeshRenderer()
{
    m_modelChangedEvent += [this]
        {
            if (auto materialRenderer = m_owner->GetComponent<MaterialRenderer>())
                materialRenderer->UpdateMaterialList();
        };
}

void MeshRenderer::SetModel(Render::Resources::Model* p_model)
{
    m_model = p_model;
    m_modelChangedEvent.Invoke();
}

Render::Resources::Model* MeshRenderer::GetModel() const
{
    return m_model;
}

std::string MeshRenderer::GetModelPath() const
{
    if (m_model)
        return m_model->path;
    return {};
}

void MeshRenderer::SetModelPath(const std::string& p_path)
{
    if (p_path.empty())
    {
        SetModel(nullptr);
        return;
    }

    auto* model = NLS_SERVICE(Core::ResourceManagement::ModelManager)[p_path];
    if (!model)
        NLS_LOG_WARNING("Failed to resolve model path during reflection load: " + p_path);
    SetModel(model);
}

void MeshRenderer::SetFrustumBehaviour(EFrustumBehaviour p_boundingMode)
{
    m_frustumBehaviour = p_boundingMode;
}

MeshRenderer::EFrustumBehaviour MeshRenderer::GetFrustumBehaviour() const
{
    return m_frustumBehaviour;
}

const Render::Geometry::BoundingSphere& MeshRenderer::GetCustomBoundingSphere() const
{
    return m_customBoundingSphere;
}

void MeshRenderer::SetCustomBoundingSphere(const Render::Geometry::BoundingSphere& p_boundingSphere)
{
    m_customBoundingSphere = p_boundingSphere;
}
}
