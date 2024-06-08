#pragma once

#include "Rendering/Resources/Mesh.h"
#include "Rendering/Data/Material.h"
#include "Rendering/Data/Describable.h"

namespace Rendering::Entities
{
	/**
	* Drawable entity
	*/
	struct Drawable : public Data::Describable
	{
		Rendering::Resources::Mesh* mesh;
		Rendering::Data::Material material;
		Rendering::Data::StateMask stateMask;
		Rendering::Settings::EPrimitiveMode primitiveMode = Rendering::Settings::EPrimitiveMode::TRIANGLES;
	};
}