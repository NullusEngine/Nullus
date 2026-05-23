#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Parsers/EModelParserFlags.h"

namespace NLS::Render::Resources::Parsers
{
	struct ParsedMeshData
	{
		std::vector<Geometry::Vertex> vertices;
		std::vector<uint32_t> indices;
		uint32_t materialIndex = 0u;
		uint32_t sourceMeshIndex = std::numeric_limits<uint32_t>::max();
		std::string sourceKey;
	};

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
