#pragma once

namespace NLS::Cursor
{
	/**
	* 一些光标形状。
	* 它们指定鼠标指针的外观
	*/
	enum class ECursorShape
	{
		ARROW		= 0x00036001,
		IBEAM		= 0x00036002,
		CROSSHAIR	= 0x00036003,
		HAND		= 0x00036004,
		HRESIZE		= 0x00036005,
		VRESIZE		= 0x00036006
	};
}