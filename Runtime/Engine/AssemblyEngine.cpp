#include "AssemblyEngine.h"
#include "Component.h"
#include "UDRefl/ReflMngr.hpp"
#include "Transform.h"
namespace NLS
{
namespace Engine
{
    using namespace UDRefl;
void AssemblyEngine::Initialize()
{
    Mngr.RegisterType<Component>();
    Mngr.AddMethod<&Component::CreateBy>("CreateBy");

    Mngr.RegisterType<Transform>();
    Mngr.AddBases<Transform, Component>();

}
} // namespace Engine
} // namespace NLS