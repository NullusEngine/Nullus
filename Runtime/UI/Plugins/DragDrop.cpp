#include "UI/Plugins/DragDrop.h"

#include "ImGui/imgui.h"

#include <cstring>
#include <string>
#include <vector>

namespace NLS::UI
{
	namespace
	{
		struct CachedDragDropPayload
		{
			std::string type;
			std::vector<std::byte> bytes;
			int frame = -1;
			bool valid = false;
		};

			CachedDragDropPayload g_cachedDragDropPayload;
			constexpr int kCachedPayloadFrameGrace = 1;

#if defined(NLS_ENABLE_TEST_HOOKS)
			struct DragDropTargetPayloadOverride
			{
				DragDropTargetPayloadForTesting payload;
				bool enabled = false;
			};

			DragDropTargetPayloadOverride g_dragDropTargetPayloadOverride;
#endif

		int GetCurrentImGuiFrame()
		{
			return ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : -1;
		}

		bool IsCachedPayloadFresh()
		{
			const int currentFrame = GetCurrentImGuiFrame();
			return g_cachedDragDropPayload.valid &&
				currentFrame >= 0 &&
				g_cachedDragDropPayload.frame >= 0 &&
				currentFrame <= g_cachedDragDropPayload.frame + kCachedPayloadFrameGrace;
		}

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
			if ((static_cast<int>(flags) & static_cast<int>(DragDropTargetFlags::AcceptBeforeDelivery)) != 0)
				imguiFlags |= ImGuiDragDropFlags_AcceptBeforeDelivery;
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
		const bool payloadAcceptedByTarget = ImGui::SetDragDropPayload(type, data, dataSize);
		if (type != nullptr && (data != nullptr || dataSize == 0u))
		{
			g_cachedDragDropPayload.type = type;
			g_cachedDragDropPayload.bytes.resize(dataSize);
			g_cachedDragDropPayload.frame = GetCurrentImGuiFrame();
			g_cachedDragDropPayload.valid = true;
			if (data != nullptr && dataSize > 0u)
				std::memcpy(g_cachedDragDropPayload.bytes.data(), data, dataSize);
		}
		return payloadAcceptedByTarget;
	}

	void ClearCachedDragDropPayload()
	{
		g_cachedDragDropPayload = {};
	}

		bool BeginDragDropTarget()
		{
#if defined(NLS_ENABLE_TEST_HOOKS)
			if (g_dragDropTargetPayloadOverride.enabled)
				return g_dragDropTargetPayloadOverride.payload.targetActive;
#endif
			return ImGui::BeginDragDropTarget();
		}

		void EndDragDropTarget()
		{
#if defined(NLS_ENABLE_TEST_HOOKS)
			if (g_dragDropTargetPayloadOverride.enabled)
				return;
#endif
			ImGui::EndDragDropTarget();
		}

		DragDropPayloadView AcceptDragDropPayload(const char* type, const DragDropTargetFlags flags)
		{
#if defined(NLS_ENABLE_TEST_HOOKS)
			if (g_dragDropTargetPayloadOverride.enabled)
			{
				const auto& overridePayload = g_dragDropTargetPayloadOverride.payload;
				if (!overridePayload.targetActive || type == nullptr ||
					overridePayload.type != type || overridePayload.bytes.empty())
					return {};
				return {
					overridePayload.bytes.data(),
					overridePayload.bytes.size(),
					overridePayload.delivered };
			}
#endif
			const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type, ToImGuiFlags(flags));
			if (payload == nullptr)
				return {};
			return { payload->Data, static_cast<size_t>(payload->DataSize), payload->IsDelivery() };
		}

	DragDropPayloadView PeekDragDropPayload(const char* type)
	{
		const ImGuiPayload* payload = ImGui::GetDragDropPayload();
		if (payload != nullptr && payload->IsDataType(type))
		{
			return { payload->Data, static_cast<size_t>(payload->DataSize), payload->IsDelivery() };
		}

		if (payload != nullptr ||
			type == nullptr ||
			!IsCachedPayloadFresh() ||
			g_cachedDragDropPayload.type != type ||
			g_cachedDragDropPayload.bytes.empty())
		{
			return {};
		}

		return {
			g_cachedDragDropPayload.bytes.data(),
			g_cachedDragDropPayload.bytes.size(),
			false };
	}

#if defined(NLS_ENABLE_TEST_HOOKS)
		void SetDragDropTargetPayloadForTesting(const DragDropTargetPayloadForTesting& payload)
		{
			g_dragDropTargetPayloadOverride.payload = payload;
			g_dragDropTargetPayloadOverride.enabled = true;
		}

		void ClearDragDropTargetPayloadForTesting()
		{
			g_dragDropTargetPayloadOverride = {};
		}

		void SetCachedDragDropPayloadForTesting(const char* type, const void* data, const size_t dataSize, const bool fresh)
		{
			g_cachedDragDropPayload = {};
		if (type == nullptr || (data == nullptr && dataSize > 0u))
			return;

		g_cachedDragDropPayload.type = type;
		g_cachedDragDropPayload.bytes.resize(dataSize);
		const int currentFrame = GetCurrentImGuiFrame();
		g_cachedDragDropPayload.frame = currentFrame - (fresh ? 0 : kCachedPayloadFrameGrace + 1);
		g_cachedDragDropPayload.valid = true;
		if (data != nullptr && dataSize > 0u)
			std::memcpy(g_cachedDragDropPayload.bytes.data(), data, dataSize);
	}
#endif
}
