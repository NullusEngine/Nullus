#include "Component.h"
#include "GameObject.h"
namespace NLS
{
namespace Engine
{
void Component::CreateBy(GameObject* owner)
{
    mOwner = owner;
    OnCreate();
}
} // namespace Engine
} // namespace NLS
