
#pragma once

#include <Math/Vector3.h>

namespace Rendering::Geometry
{
	/**
	* Data structure that defines a bounding sphere (Position + radius)
	*/
	struct BoundingSphere
	{
		NLS::Maths::Vector3 position;
		float radius;
	};
}