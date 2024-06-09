#pragma once

#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Data/FrameInfo.h"

namespace NLS::Rendering::Features
{
	class FrameInfoRenderFeature : public NLS::Rendering::Features::ARenderFeature
	{
	public:
		/**
		* Constructor
		* @param p_renderer
		*/
		FrameInfoRenderFeature(NLS::Rendering::Core::CompositeRenderer& p_renderer);

		/**
		* Destructor
		*/
		virtual ~FrameInfoRenderFeature();

		/**
		* Return a reference to the last frame info
		* @note Will throw an error if called during the rendering of a frame
		*/
		const NLS::Rendering::Data::FrameInfo& GetFrameInfo() const;

	protected:
		virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor) override;
		virtual void OnEndFrame() override;
		virtual void OnAfterDraw(const NLS::Rendering::Entities::Drawable& p_drawable) override;

	private:
		bool m_isFrameInfoDataValid;
		NLS::Rendering::Data::FrameInfo m_frameInfo;
		NLS::ListenerID m_postDrawListener;
	};
}
