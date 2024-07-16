#pragma once

#include <Rendering/Geometry/Vertex.h>
#include <Rendering/Resources/Model.h>

#include "Components/Component.h"
#include "Eventing/Event.h"
#include "EngineDef.h"
namespace NLS::Engine::Components
{
	/**
	* A ModelRenderer is necessary in combination with a MaterialRenderer to render a model in the world
	*/
	class NLS_ENGINE_API MeshRenderer : public Component
	{
	public:
		/**
		* Defines how the model renderer bounding sphere should be interpreted
		*/
		enum class EFrustumBehaviour
		{
			DISABLED = 0,
			CULL_MODEL = 1,
			CULL_MESHES = 2,
			CULL_CUSTOM = 3
		};

		/**
		* Constructor
		* @param p_owner
		*/
		MeshRenderer();

		/**
		* Defines the model to use
		* @param p_model
		*/
		void SetModel(NLS::Rendering::Resources::Model* p_model);

		/**
		* Returns the current model
		*/
		NLS::Rendering::Resources::Model* GetModel() const;

		/**
		* Sets a bounding mode
		* @param p_boundingMode
		*/
		void SetFrustumBehaviour(EFrustumBehaviour p_boundingMode);

		/**
		* Returns the current bounding mode
		*/
		EFrustumBehaviour GetFrustumBehaviour() const;

		/**
		* Returns the custom bounding sphere
		*/
        const NLS::Rendering::Geometry::BoundingSphere GetCustomBoundingSphere() const;

		/**
		* Sets the custom bounding sphere
		* @param p_boundingSphere
		*/
        void SetCustomBoundingSphere(const NLS::Rendering::Geometry::BoundingSphere& p_boundingSphere);



	private:
		NLS::Rendering::Resources::Model* m_model = nullptr;
		Event<> m_modelChangedEvent;
        NLS::Rendering::Geometry::BoundingSphere m_customBoundingSphere = {{}, 1.0f};
		EFrustumBehaviour m_frustumBehaviour = EFrustumBehaviour::CULL_MODEL;
	};
}