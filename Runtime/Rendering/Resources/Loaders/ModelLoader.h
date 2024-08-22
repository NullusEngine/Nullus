#pragma once

#include <string>

#include "Rendering/Resources/Model.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"

namespace NLS::Render::Resources::Loaders
{
	/**
	* Handle the Model creation and destruction
	*/
	class NLS_RENDER_API ModelLoader
	{
	public:
		/**
		* Disabled constructor
		*/
		ModelLoader() = delete;

		/**
		* Create a model
		* @param p_filepath
		* @param p_parserFlags
		*/
		static Model* Create(const std::string& p_filepath, Parsers::EModelParserFlags p_parserFlags = Parsers::EModelParserFlags::NONE);

		static Model* Create(const std::vector<NLS::Render::Resources::Mesh*>& meshes);

		/**
		* Reload a model from file
		* @param p_model
		* @param p_filePath
		* @param p_parserFlags
		*/
		static void Reload(Model& p_model, const std::string& p_filePath, Parsers::EModelParserFlags p_parserFlags = Parsers::EModelParserFlags::NONE);

		/**
		* Disabled constructor
		* @param p_modelInstance
		*/
		static bool Destroy(Model*& p_modelInstance);

	private:
		static Parsers::AssimpParser __ASSIMP;
	};
}