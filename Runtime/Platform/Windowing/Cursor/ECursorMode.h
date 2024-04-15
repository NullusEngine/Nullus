#pragma once

namespace NLS::Cursor
{
	/**
	* 光标模式。
	* 它们定义鼠标指针是否可见、锁定或正常
	*/
	enum class ECursorMode
	{
		NORMAL		= 0x00034001,
		DISABLED	= 0x00034003,
		HIDDEN		= 0x00034002
	};
}