
#pragma once

#include <Math/Vector3.h>
#include "Reflection/Macros.h"
#include "Geometry/BoundingSphere.generated.h"

namespace NLS::Render::Geometry
{
	/**
	* Data structure that defines a bounding sphere (Position + radius)
	*/
	STRUCT() struct BoundingSphere
	{
        GENERATED_BODY()
        PROPERTY()
		NLS::Maths::Vector3 position;
        PROPERTY()
		float radius;
	};
}
