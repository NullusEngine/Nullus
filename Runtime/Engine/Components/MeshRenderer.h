#pragma once

#include <Rendering/Geometry/Vertex.h>
#include <Rendering/Resources/Model.h>
#include <string>

#include "Components/Component.h"
#include "Eventing/Event.h"
#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Components/MeshRenderer.generated.h"
namespace NLS::Engine::Components
{
	/**
	* A ModelRenderer is necessary in combination with a MaterialRenderer to render a model in the world
	*/
	CLASS() class NLS_ENGINE_API MeshRenderer : public Component
	{
    public:
		GENERATED_BODY()
        using Model = Render::Resources::Model;

		/**
		* Defines how the model renderer bounding sphere should be interpreted
		*/
        ENUM() enum class EFrustumBehaviour
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

        PROPERTY(name = model, getter = GetModelPath, setter = SetModelPath)

		/**
		* Defines the model to use
		* @param p_model
		*/
        FUNCTION()
		void SetModel(Model* p_model);

		/**
		* Returns the current model
		*/
        FUNCTION()
		Model* GetModel() const;

        FUNCTION()
        std::string GetModelPath() const;

        FUNCTION()
        void SetModelPath(const std::string& p_path);

		/**
		* Sets a bounding mode
		* @param p_boundingMode
		*/
        FUNCTION()
		void SetFrustumBehaviour(EFrustumBehaviour p_boundingMode);

		/**
		* Returns the current bounding mode
		*/
        FUNCTION()
		EFrustumBehaviour GetFrustumBehaviour() const;

		/**
		* Returns the custom bounding sphere
		*/
        FUNCTION()
        const Render::Geometry::BoundingSphere& GetCustomBoundingSphere() const;

		/**
		* Sets the custom bounding sphere
		* @param p_boundingSphere
		*/
        FUNCTION()
        void SetCustomBoundingSphere(const Render::Geometry::BoundingSphere& p_boundingSphere);



	private:
		Model* m_model = nullptr;
		Event<> m_modelChangedEvent;
        Render::Geometry::BoundingSphere m_customBoundingSphere = {{}, 1.0f};
		EFrustumBehaviour m_frustumBehaviour = EFrustumBehaviour::CULL_MODEL;
	};
}
