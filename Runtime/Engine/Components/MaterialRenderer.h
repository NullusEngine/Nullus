#pragma once

#include <array>

#include <Rendering/Resources/Mesh.h>

#include "Rendering/Data/Material.h"
#include "Components/Component.h"
#include "EngineDef.h"
constexpr uint8_t kMaxMaterialCount = 0xFF;

namespace NLS::Engine::Components
{
	/**
	* A component that handle a material list, necessary for model rendering
	*/
	class NLS_ENGINE_API MaterialRenderer : public Component
	{
	public:
		using MaterialList = std::array< NLS::Rendering::Data::Material*, kMaxMaterialCount>;

		/**
		* Constructor
		* @param p_owner
		*/
		MaterialRenderer();

		void OnCreate();

		/**
		* Fill the material renderer with the given material
		* @param p_material
		*/
		void FillWithMaterial(NLS::Rendering::Data::Material& p_material);

		/**
		* Defines the material to use for the given index
		* @param p_index
		* @param p_material
		*/
		void SetMaterialAtIndex(uint8_t p_index, NLS::Rendering::Data::Material& p_material);

		/**
		* Returns the material to use at index
		* @param p_index
		*/
		NLS::Rendering::Data::Material* GetMaterialAtIndex(uint8_t p_index);

		/**
		* Remove the material at index
		* @param p_index
		*/
		void RemoveMaterialAtIndex(uint8_t p_index);

		/**
		* Remove the material by instance
		* @param p_instance
		*/
		void RemoveMaterialByInstance(NLS::Rendering::Data::Material& p_instance);

		/**
		* Remove every materials
		*/
		void RemoveAllMaterials();

		/**
		* Update the material list by fetching model information
		*/
		void UpdateMaterialList();

		/**
		* Defines an element of the user matrix
		* @param p_row
		* @param p_column
		* @param p_value
		*/
		void SetUserMatrixElement(uint32_t p_row, uint32_t p_column, float p_value);

		/**
		* Returns an element of the user matrix
		* @param p_row
		* @param p_column
		*/
		float GetUserMatrixElement(uint32_t p_row, uint32_t p_column) const;

		/**
		* Returns the user matrix
		*/
		const Maths::Matrix4& GetUserMatrix() const;

		/**
		* Returns the materials
		*/
		const MaterialList& GetMaterials() const;

	private:
		MaterialList m_materials;
		std::array<std::string, kMaxMaterialCount> m_materialNames;
		Maths::Matrix4 m_userMatrix;
	};
}