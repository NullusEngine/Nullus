

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ResourceManagement/ModelManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "GameObject.h"
using namespace NLS;
using namespace NLS::Engine::Components;

NLS::Engine::Components::MeshRenderer::MeshRenderer()
{
	m_modelChangedEvent += [this]
		{
			if (auto materialRenderer = m_owner->GetComponent<MaterialRenderer>())
				materialRenderer->UpdateMaterialList();
		};
}
#include "UDRefl/ReflMngr.hpp"
using namespace NLS::UDRefl;
void MeshRenderer::Bind()
{
    Mngr.RegisterType<MeshRenderer>();
    Mngr.AddBases<MeshRenderer, Component>();
}

void MeshRenderer::SetModel(Render::Resources::Model* p_model)
{
	m_model = p_model;
	m_modelChangedEvent.Invoke();
}

Render::Resources::Model * MeshRenderer::GetModel() const
{
	return m_model;
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
