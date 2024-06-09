#pragma once

#include "Rendering/Resources/Mesh.h"
#include "Rendering/Data/Material.h"
#include "Rendering/Data/Describable.h"

namespace NLS::Rendering::Entities
{
	/**
	* Drawable entity
	*/
	struct Drawable : public Data::Describable
	{
		NLS::Rendering::Resources::Mesh* mesh;
		NLS::Rendering::Data::Material material;
		NLS::Rendering::Data::StateMask stateMask;
		NLS::Rendering::Settings::EPrimitiveMode primitiveMode = NLS::Rendering::Settings::EPrimitiveMode::TRIANGLES;
	};
}