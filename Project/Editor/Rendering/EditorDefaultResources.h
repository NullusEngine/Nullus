#pragma once

#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/Texture2D.h"

namespace NLS::Editor::Rendering
{
	inline NLS::Render::Resources::Texture2D* GetEditorDefaultWhiteTexture()
	{
		static NLS::Render::Resources::Texture2D* texture = []()
		{
			auto* created = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(255, 255, 255, 255);
			if (created != nullptr)
				created->path = ":Editor/DefaultWhiteTexture";
			return created;
		}();
		return texture;
	}
}
