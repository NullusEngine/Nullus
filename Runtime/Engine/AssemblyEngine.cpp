#include "AssemblyEngine.h"
#include "Components/Component.h"
#include "UDRefl/ReflMngr.hpp"
#include "Components/TransformComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "GameObject.h"
namespace NLS
{
namespace Engine
{
    using namespace UDRefl;
    using namespace Components;
void AssemblyEngine::Initialize()
{
    Mngr.RegisterType<GameObject>();

    Mngr.RegisterType<Component>();
    Mngr.AddMethod<&Component::CreateBy>("CreateBy");

    Mngr.RegisterType<TransformComponent>();
    Mngr.AddBases<TransformComponent, Component>();

    Mngr.RegisterType<CameraComponent>();
    Mngr.AddBases<CameraComponent, Component>();

    Mngr.RegisterType<LightComponent>();
    Mngr.AddBases<LightComponent, Component>();

    Mngr.RegisterType<MaterialRenderer>();
    Mngr.AddBases<MaterialRenderer, Component>();

    Mngr.RegisterType<MeshRenderer>();
    Mngr.AddBases<MeshRenderer, Component>();
}
} // namespace Engine
} // namespace NLS