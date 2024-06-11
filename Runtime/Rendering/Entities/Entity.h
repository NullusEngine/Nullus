
#pragma once

#include <memory>

#include <Math/Transform.h>
#include "RenderDef.h"
namespace NLS::Rendering::Entities
{
	/**
	* Represents an entity with a transformation in space
	*/
	struct NLS_RENDER_API Entity
	{
		NLS::Maths::Transform* transform = nullptr;
	};
}
