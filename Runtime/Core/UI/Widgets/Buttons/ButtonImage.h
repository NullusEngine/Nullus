#pragma once

#include <Vector2.h>

#include "UI/Internal/TextureID.h"
#include "UI/Types/Color.h"
#include "UI/Widgets/Buttons/AButton.h"

namespace NLS::UI::Widgets::Buttons
{
	/**
	* Button widget with an image
	*/
	class ButtonImage : public AButton
	{
	public:
		/**
		* Constructor
		* @param p_textureID
		* @param p_size
		*/
		ButtonImage(uint32_t p_textureID, const Maths::Vector2& p_size);

	protected:
		void _Draw_Impl() override;

	public:
		bool disabled = false;

		UI::Types::Color background = { 0, 0, 0, 0 };
		UI::Types::Color tint = { 1, 1, 1, 1 };

		Internal::TextureID textureID;
		Maths::Vector2 size;
	};
}