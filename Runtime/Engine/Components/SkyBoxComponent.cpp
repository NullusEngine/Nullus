

#include "Components/SkyBoxComponent.h"

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ResourceManagement/ModelManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "GameObject.h"

using namespace NLS;
using namespace NLS::Engine::Components;

SkyBoxComponent::SkyBoxComponent()
{
	mModel = NLS_SERVICE(NLS::Core::ResourceManagement::ModelManager).CreateResource(":Models\\SkyCube.obj");

	auto shader = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager).CreateResource(":Shaders\\Skybox.glsl");
	mMaterial = new NLS::Render::Resources::Material(shader);
	mMaterial->SetDepthWriting(false);
}

void SkyBoxComponent::SetCubeMap(NLS::Render::Resources::TextureCube* cubmap)
{
	if (mMaterial)
	{
		mMaterial->Set("cubeTex", cubmap);
	}
}
#include "UDRefl/ReflMngr.hpp"
using namespace UDRefl;
void SkyBoxComponent::Bind()
{
    Mngr.RegisterType<SkyBoxComponent>();
    Mngr.AddBases<SkyBoxComponent, Component>();
}
