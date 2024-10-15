#include "AssemblyMath.h"
#include "Transform.h"
namespace NLS
{
void AssemblyMath::Initialize()
{
    Maths::Transform::Bind();
    Maths::Vector3::Bind();
}
}