#include "AssemblyEngine.h"
#include "Components/Component.h"
#include "Components/TransformComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "GameObject.h"
#include "Gen/MetaGenerated.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine
{
using namespace Components;

void AssemblyEngine::Initialize()
{
    NLS_META_GENERATED_LINK_FUNCTION();
}
} // namespace NLS::Engine
