#pragma once
#include "Core/CoreDef.h"
namespace NLS::UI::API
{
	/**
	* Interface for any drawable class
	*/
	class NLS_CORE_API IDrawable
	{
	public:
		virtual void Draw() = 0;

	protected:
		virtual ~IDrawable() = default;
	};
}
