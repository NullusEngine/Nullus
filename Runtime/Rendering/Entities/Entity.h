
#pragma once

#include <memory>

#include <Math/Transform.h>

namespace NLS::Rendering::Entities
{
	/**
	* Represents an entity with a transformation in space
	*/
	struct Entity
	{
		NLS::Maths::Transform transform;
	};
}
