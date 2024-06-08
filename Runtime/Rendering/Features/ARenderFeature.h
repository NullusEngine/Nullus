#pragma once

#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Drawable.h"

namespace Rendering::Core
{
	class CompositeRenderer;
}

namespace Rendering::Features
{
	/**
	* Extension of the composite renderer that provides new rendering capabilities
	*/
	class ARenderFeature
	{
	public:
		/**
		* Constructor
		* @param p_renderer
		*/
		ARenderFeature(Core::CompositeRenderer& p_renderer);

		/**
		* Destructor
		*/
		virtual ~ARenderFeature() = default;

		/**
		* Enable (or disable) the render pass.
		*/
		void SetEnabled(bool p_enabled);

		/**
		* Returns true if the render pass is enabled
		*/
		bool IsEnabled() const;

	protected:
		/**
		* Invoked when the BeginFrame is called on the associated base renderer
		* @param p_frameDescriptor
		*/
		virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor);

		/**
		* Invoked when the EndFrame is called on the associated base renderer
		*/
		virtual void OnEndFrame();

		/**
		* Invoked before drawing a drawable entity
		* @param p_drawable
		*/
		virtual void OnBeforeDraw(Data::PipelineState& p_pso, const Entities::Drawable& p_drawable);

		/**
		* Invoked after drawing a drawable entity
		* @param p_drawable
		*/
		virtual void OnAfterDraw(const Entities::Drawable& p_drawable);

	protected:
		Core::CompositeRenderer& m_renderer;
		bool m_enabled = true;

		friend class Core::CompositeRenderer;
	};
}