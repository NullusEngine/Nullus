#pragma once

#include "UI/UIDef.h"

namespace NLS::UI
{
	/**
	* Interface for any drawable class
	*/
	class NLS_UI_API IDrawable
	{
	public:
		virtual void Draw() = 0;

	protected:
		virtual ~IDrawable() = default;
	};
}
