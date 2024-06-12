#pragma once

#include <typeindex>
#include <memory>

#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Core/ARenderPass.h"
#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Data/Describable.h"
#include "Eventing/Event.h"
#include "RenderDef.h"
namespace NLS::Rendering::Core
{
	/**
	* A renderer relying on composition to define rendering logic.
	*/
	class NLS_RENDER_API CompositeRenderer : public ABaseRenderer, public Data::Describable
	{
	public:
		NLS::Event<NLS::Rendering::Data::PipelineState&, const Entities::Drawable&> preDrawEntityEvent;
		NLS::Event<const Entities::Drawable&> postDrawEntityEvent;

		/**
		* Constructor of the base renderer
		* @param p_driver
		*/
		CompositeRenderer(Context::Driver& p_driver);

		CompositeRenderer(const CompositeRenderer&) = delete;

		CompositeRenderer &operator=(const CompositeRenderer&) = delete;
		/**
		* Begin Frame
		* @param p_frameDescriptor
		*/
		virtual void BeginFrame(const Data::FrameDescriptor& p_frameDescriptor);

		/**
		* Handle the drawing logic of render pass, invoking DrawPass on the renderer and its
		* associated render features.
		*/
		virtual void DrawFrame() final;

		/**
		* End Frame
		*/
		virtual void EndFrame() override;

		/**
		* Draw a drawable entity
		* @param p_pso
		* @param p_drawable
		*/
		virtual void DrawEntity(
			NLS::Rendering::Data::PipelineState p_pso,
			const Entities::Drawable& p_drawable
		) override;

		/**
		* Add a render feature to the renderer
		* @param p_args (Parameter pack forwared to the render feature constructor)
		*/
		template<typename T, typename ... Args>
		T& AddFeature(Args&&... p_args);

		/**
		* Remove the given render feature
		*/
		template<typename T>
		bool RemoveFeature();

		/**
		* Retrieve the render feature matching the given type
		* @note Fails if the feature doesn't exist
		*/
		template<typename T>
		T& GetFeature() const;

		/**
		* Return true if the a feature matching the given type has been found
		*/
		template<typename T>
		bool HasFeature() const;

		/**
		* Add a render pass to the renderer
		* @param p_name
		* @param p_order
		* @param p_args (Parameter pack forwared to the render pass constructor)
		*/
		template<typename T, typename ... Args>
		T& AddPass(const std::string& p_name, uint32_t p_order, Args&&... p_args);

		/**
		* Retrieve the render passing matching the given pass name
		* @param p_name
		*/
		template<typename T>
		T& GetPass(const std::string& p_name) const;

	protected:
		std::unordered_map<std::type_index, std::unique_ptr<Features::ARenderFeature>> m_features;
		std::multimap<uint32_t, std::pair<std::string, std::unique_ptr<Core::ARenderPass>>> m_passes;
	};
}

#include "Rendering/Core/CompositeRenderer.inl"
