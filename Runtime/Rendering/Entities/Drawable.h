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
		NLS::Render::Resources::Mesh* mesh;
		NLS::Render::Resources::Material* material;
		NLS::Render::Data::StateMask stateMask;
		NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
	};
}