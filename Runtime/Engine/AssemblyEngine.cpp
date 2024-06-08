#include "AssemblyEngine.h"
#include "Component.h"
#include "UDRefl/ReflMngr.hpp"
#include "TransformComponent.h"
namespace NLS
{
namespace Engine
{
    using namespace UDRefl;
void AssemblyEngine::Initialize()
{
    Mngr.RegisterType<Component>();
    Mngr.AddMethod<&Component::CreateBy>("CreateBy");

    Mngr.RegisterType<TransformComponent>();
    Mngr.AddBases<TransformComponent, Component>();
}
} // namespace Engine
} // namespace NLS