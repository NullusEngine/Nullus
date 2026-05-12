#include "UI/Plugins/DragDrop.h"

#include "ImGui/imgui.h"

namespace NLS::UI
{
	namespace
	{
		ImGuiDragDropFlags ToImGuiFlags(const DragDropSourceFlags flags)
		{
			ImGuiDragDropFlags imguiFlags = 0;
			if (HasFlag(flags, DragDropSourceFlags::NoPreviewTooltip))
				imguiFlags |= ImGuiDragDropFlags_SourceNoPreviewTooltip;
			if (HasFlag(flags, DragDropSourceFlags::NoDisableHover))
				imguiFlags |= ImGuiDragDropFlags_SourceNoDisableHover;
			if (HasFlag(flags, DragDropSourceFlags::NoHoldToOpenOthers))
				imguiFlags |= ImGuiDragDropFlags_SourceNoHoldToOpenOthers;
			return imguiFlags;
		}

		ImGuiDragDropFlags ToImGuiFlags(const DragDropTargetFlags flags)
		{
			ImGuiDragDropFlags imguiFlags = 0;
			if ((static_cast<int>(flags) & static_cast<int>(DragDropTargetFlags::AcceptNoDrawDefaultRect)) != 0)
				imguiFlags |= ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			return imguiFlags;
		}
	}

	bool BeginDragDropSource(const DragDropSourceFlags flags)
	{
		return ImGui::BeginDragDropSource(ToImGuiFlags(flags));
	}

	void EndDragDropSource()
	{
		ImGui::EndDragDropSource();
	}

	void DrawDragDropTooltipText(const char* text)
	{
		ImGui::Text("%s", text != nullptr ? text : "");
	}

	bool SetDragDropPayload(const char* type, const void* data, const size_t dataSize)
	{
		return ImGui::SetDragDropPayload(type, data, dataSize);
	}

	bool BeginDragDropTarget()
	{
		return ImGui::BeginDragDropTarget();
	}

	void EndDragDropTarget()
	{
		ImGui::EndDragDropTarget();
	}

	DragDropPayloadView AcceptDragDropPayload(const char* type, const DragDropTargetFlags flags)
	{
		const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type, ToImGuiFlags(flags));
		if (payload == nullptr)
			return {};
		return { payload->Data, static_cast<size_t>(payload->DataSize) };
	}
}
