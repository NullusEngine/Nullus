#pragma once

#include <string>

#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Parsers/EModelParserFlags.h"

namespace NLS::Render::Resources::Parsers
{
	/**
	* Interface for any model parser
	*/
	class IModelParser
	{
	public:
		/**
		* Load meshes from a file
		* Return true on success
		* @param p_filename
		* @param p_meshes
		* @param p_parserFlags
		*/
		virtual bool LoadModel
		(
			const std::string& p_fileName,
			std::vector<Mesh*>& p_meshes,
			std::vector<std::string>& p_materials,
			EModelParserFlags p_parserFlags
		) = 0;
	};
}