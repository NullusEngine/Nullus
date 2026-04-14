#pragma once

#include <memory>

#include <Vector2.h>

#include "Color.h"
#include "UI/Widgets/Buttons/AButton.h"

namespace NLS::Render::RHI
{
    class RHITextureView;
}

namespace NLS::UI::Widgets
{
	/**
	* Button widget with an image
	*/
	class NLS_UI_API ButtonImage : public AButton
	{
	public:
		/**
		* Constructor
		* @param p_textureView
		* @param p_size
		*/
		ButtonImage(std::shared_ptr<NLS::Render::RHI::RHITextureView> p_textureView, const Maths::Vector2& p_size);

	protected:
		void _Draw_Impl() override;

	public:
		bool disabled = false;

		Maths::Color background = { 0, 0, 0, 0 };
		Maths::Color tint = { 1, 1, 1, 1 };

		std::shared_ptr<NLS::Render::RHI::RHITextureView> textureView;
		Maths::Vector2 size;
	};
}
