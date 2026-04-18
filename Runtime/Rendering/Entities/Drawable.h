#pragma once

#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Data/Describable.h"

namespace NLS::Render::Entities
{
	/**
	* Drawable entity
	*/
	struct NLS_RENDER_API Drawable : public Data::Describable
	{
        using Mesh = Resources::Mesh;
        using Material = Resources::Material;

		Mesh* mesh = nullptr;
		Material* material = nullptr;
		Data::StateMask stateMask;
		Settings::EPrimitiveMode primitiveMode = Settings::EPrimitiveMode::TRIANGLES;
	};
}
