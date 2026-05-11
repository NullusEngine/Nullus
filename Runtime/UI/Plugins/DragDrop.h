#pragma once

#include <cstdint>
#include <cstddef>

#include "UI/UIDef.h"

namespace NLS::UI
{
	struct NLS_UI_API DragDropPayloadView
	{
		const void* data = nullptr;
		size_t dataSize = 0;
	};

	enum class DragDropSourceFlags : int
	{
		None = 0,
		NoPreviewTooltip = 1 << 0,
		NoDisableHover = 1 << 1,
		NoHoldToOpenOthers = 1 << 2
	};

	enum class DragDropTargetFlags : int
	{
		None = 0,
		AcceptNoDrawDefaultRect = 1 << 0
	};

	constexpr DragDropSourceFlags operator|(DragDropSourceFlags lhs, DragDropSourceFlags rhs)
	{
		return static_cast<DragDropSourceFlags>(static_cast<int>(lhs) | static_cast<int>(rhs));
	}

	constexpr DragDropSourceFlags& operator|=(DragDropSourceFlags& lhs, DragDropSourceFlags rhs)
	{
		lhs = lhs | rhs;
		return lhs;
	}

	constexpr DragDropTargetFlags operator|(DragDropTargetFlags lhs, DragDropTargetFlags rhs)
	{
		return static_cast<DragDropTargetFlags>(static_cast<int>(lhs) | static_cast<int>(rhs));
	}

	constexpr DragDropTargetFlags& operator|=(DragDropTargetFlags& lhs, DragDropTargetFlags rhs)
	{
		lhs = lhs | rhs;
		return lhs;
	}

	constexpr bool HasFlag(DragDropSourceFlags flags, DragDropSourceFlags flag)
	{
		return (static_cast<int>(flags) & static_cast<int>(flag)) != 0;
	}

	NLS_UI_API bool BeginDragDropSource(DragDropSourceFlags flags);
	NLS_UI_API void EndDragDropSource();
	NLS_UI_API void DrawDragDropTooltipText(const char* text);
	NLS_UI_API bool SetDragDropPayload(const char* type, const void* data, size_t dataSize);
	NLS_UI_API bool BeginDragDropTarget();
	NLS_UI_API void EndDragDropTarget();
	NLS_UI_API DragDropPayloadView AcceptDragDropPayload(const char* type, DragDropTargetFlags flags);
}
