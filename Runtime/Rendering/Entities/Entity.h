
#pragma once

#include <memory>

#include <Math/Transform.h>

namespace Rendering::Entities
{
	/**
	* Represents an entity with a transformation in space
	*/
	struct Entity
	{
		NLS::Maths::Transform transform;
	};
}
