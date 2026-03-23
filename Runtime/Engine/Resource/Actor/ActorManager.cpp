#include "GameObject.h"
#include <algorithm>

#include "Components/TransformComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Rendering/Geometry/Vertex.h"
#include "Resources/Material.h"

#include "ActorLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "ResourceManagement/ModelManager.h"
#include "ResourceManagement/TextureManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include <Utils/PathParser.h>
#include "Resource/Actor/ActorManager.h"
#include "Resource/Actor/ActorLoader.h"

namespace NLS::Engine
{
using namespace Components;
using namespace NLS::Render::Resources;

Actor* ActorManager::CreateResource(const std::string& path)
{
	auto&& realPath = GetRealPath(path);
	return ActorLoader::LoadActor(path, realPath);
}

void ActorManager::DestroyResource(Actor* resource)
{
	if (resource)
	{
		delete resource;
	}
}

void ActorManager::ReloadResource(Actor* p_resource, const std::string& p_path)
{
}
} // namespace NLS::Engine
