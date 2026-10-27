#include "Panels/UIDocumentDesigner.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

#include <Debug/Logger.h>
#include <ServiceLocator.h>
#include <imgui.h>

#include "Settings/EditorSettings.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Panels
{
    namespace
    {
        using Json = NLS::Editor::UIDocumentTools::UIDocumentDesignerModel::Json;
        using UndoService = NLS::Editor::Undo::EditorUndoService;

        constexpr const char* kPaletteTypes[] = {
            "Panel", "Image", "NineSlice", "Text", "RichText", "Button", "Toggle",
            "Radio", "Slider", "ProgressBar", "TextField", "Dropdown", "ScrollView",
            "Scrollbar", "ListView"
        };

        UndoService* TryGetUndoService()
        {
            if (!NLS::Core::ServiceLocator::Contains<UndoService>())
                return nullptr;
            return &NLS::Core::ServiceLocator::Get<UndoService>();
        }

        std::string ToLower(std::string value)
        {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            return value;
        }

        std::string ReadFilename(const std::filesystem::path& path)
        {
            return path.filename().string();
        }
    }

    UIDocumentDesigner::UIDocumentDesigner(
        const std::string& title,
        const bool opened,
        const NLS::UI::PanelWindowSettings& windowSettings)
        : PanelWindow(title, opened, windowSettings)
    {
        minSize = { 900.0f, 560.0f };
    }

    bool UIDocumentDesigner::OwnsShortcuts() const
    {
        return IsOpened() && m_shortcutFocus;
    }

    bool UIDocumentDesigner::OpenDocument(const std::filesystem::path& path)
    {
        Open();
        Focus();

        if (!m_model.IsLoaded())
        {
            LoadDocument(path);
            return m_model.IsLoaded();
        }

        std::error_code currentError;
        std::error_code requestedError;
        const auto current = std::filesystem::weakly_canonical(m_model.GetPath(), currentError);
        const auto requested = std::filesystem::weakly_canonical(path, requestedError);
        if (!currentError && !requestedError && current == requested)
            return true;

        if (m_model.IsDirty())
        {
            m_pending = { PendingAction::OpenPath, path };
            m_openPendingDialog = true;
            m_pendingSaveFailed = false;
            return true;
        }

        return LoadDocument(path);
    }

    bool UIDocumentDesigner::LoadDocument(const std::filesystem::path& path)
    {
        (void)m_model.Load(path);
        std::error_code loadError;
        std::error_code requestedError;
        const auto loaded = std::filesystem::weakly_canonical(m_model.GetPath(), loadError);
        const auto requested = std::filesystem::weakly_canonical(path, requestedError);
        if (!m_model.IsLoaded() || loadError || requestedError || loaded != requested)
        {
            NLS_LOG_ERROR("Failed to open UI document: " + path.string());
            return false;
        }

        m_canvas.GetRenderer().Invalidate();
        m_canvas.RequestFrameAll();
        m_hierarchy.RequestScrollToSelection();
        m_lastDiagnosticRevision = 0u;
        UpdateDiagnosticNodes();

        std::error_code error;
        m_lastWriteTime = std::filesystem::last_write_time(path, error);
        m_hasLastWriteTime = !error;
        m_externalChange = false;
        m_externalConflict = false;
        m_pendingSaveFailed = false;
        return true;
    }

    void UIDocumentDesigner::SaveDocument()
    {
        if (!m_model.IsLoaded())
            return;
        if (!m_model.Save())
        {
            NLS_LOG_ERROR("Failed to save UI document: " + m_model.GetPath().string());
            return;
        }

        std::error_code error;
        m_lastWriteTime = std::filesystem::last_write_time(m_model.GetPath(), error);
        m_hasLastWriteTime = !error;
        m_externalChange = false;
        m_externalConflict = false;
        m_pendingSaveFailed = false;

        // The Asset Browser file watcher picks up the written .ui file; the
        // designer deliberately avoids depending on that panel so the document
        // model can be used by tests and editor scripts without a full editor.
        NLS_LOG_INFO("Saved UI document: " + m_model.GetPath().string());
    }

    void UIDocumentDesigner::ReloadDocument()
    {
        if (!m_model.IsLoaded())
            return;
        if (m_model.IsDirty())
        {
            m_pending = { PendingAction::Reload, m_model.GetPath() };
            m_openPendingDialog = true;
            m_pendingSaveFailed = false;
            return;
        }
        LoadDocument(m_model.GetPath());
    }

    void UIDocumentDesigner::ResolvePending(
        const PendingAction action,
        const std::filesystem::path& path)
    {
        m_pending = {};
        if (action == PendingAction::OpenPath || action == PendingAction::Reload)
            LoadDocument(path);
    }

    bool UIDocumentDesigner::CanSave() const
    {
        return m_model.IsLoaded() && m_model.IsDirty() && !m_model.HasErrors();
    }

    bool UIDocumentDesigner::ExecuteCommand(const std::string& id)
    {
        if (id == "save")
        {
            if (!CanSave())
                return false;
            SaveDocument();
            return true;
        }
        if (id == "revert")
        {
            if (!m_model.IsLoaded())
                return false;
            ReloadDocument();
            return true;
        }
        if (!m_model.IsLoaded())
            return false;

        if (id == "duplicate") return m_canvas.DuplicateSelection(m_model);
        if (id == "delete") return m_canvas.DeleteSelection(m_model);
        if (id == "copy") return m_canvas.CopySelection(m_model);
        if (id == "paste") return m_canvas.PasteClipboard(m_model);
        if (id == "frame-all") { m_canvas.RequestFrameAll(); return true; }
        if (id == "frame-selection") { m_canvas.RequestFrameSelection(); return true; }
        if (id == "zoom-in") { m_canvas.ZoomIn(m_model); return true; }
        if (id == "zoom-out") { m_canvas.ZoomOut(m_model); return true; }
        if (id == "zoom-100") { m_canvas.SetZoom(m_model, 1.0f); return true; }
        return false;
    }

    bool UIDocumentDesigner::IsCommandEnabled(const std::string& id) const
    {
        if (id == "save") return CanSave();
        if (id == "revert") return m_model.IsLoaded();
        if (id == "paste") return NLS::Editor::UIDocumentTools::UIDocumentCanvasView::HasClipboardContent();
        if (id == "duplicate" || id == "delete" || id == "copy")
            return !m_model.GetSelectedNodes().empty();
        if (id == "frame-all" || id == "frame-selection" ||
            id == "zoom-in" || id == "zoom-out" || id == "zoom-100")
        {
            return m_model.IsLoaded();
        }
        return false;
    }

    void UIDocumentDesigner::OnBeforeDrawWidgets()
    {
        m_shortcutFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

        if (!m_model.IsLoaded())
        {
            ImGui::TextDisabled("Open a .ui asset from the Asset Browser.");
            DrawPendingDialog();
            return;
        }

        PollExternalChanges();
        if (m_lastDiagnosticRevision != m_model.GetDocumentRevision())
            UpdateDiagnosticNodes();

        DrawToolbar();
        DrawExternalChangeBanner();
        HandleShortcuts();

        const ImVec2 content = ImGui::GetContentRegionAvail();
        const float diagnosticsHeight = 108.0f;
        const float mainHeight = std::max(
            140.0f,
            content.y - diagnosticsHeight - ImGui::GetStyle().ItemSpacing.y);
        const float maxLeftWidth = std::max(190.0f, content.x * 0.36f);
        const float maxRightWidth = std::max(260.0f, content.x * 0.44f);
        m_leftWidth = std::clamp(m_leftWidth, 190.0f, maxLeftWidth);
        m_rightWidth = std::clamp(m_rightWidth, 260.0f, maxRightWidth);

        auto& io = ImGui::GetIO();

        ImGui::BeginChild("##designerLeft", ImVec2(m_leftWidth, mainHeight), false);
        DrawLeftColumn();
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 2.0f);
        ImGui::InvisibleButton("##designerLeftSplitter", ImVec2(4.0f, mainHeight));
        if (ImGui::IsItemActive())
            m_leftWidth = std::clamp(
                m_leftWidth + io.MouseDelta.x,
                190.0f,
                maxLeftWidth);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SameLine(0.0f, 2.0f);

        const float canvasWidth = std::max(
            120.0f,
            content.x - m_leftWidth - m_rightWidth - 20.0f);
        ImGui::BeginChild(
            "##designerCanvas",
            ImVec2(canvasWidth, mainHeight),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            auto& settings =
                NLS::Editor::Settings::EditorSettings::GetUIDesignerSettingsObject();
            NLS::Editor::UIDocumentTools::UIDocumentCanvasView::Options options;
            options.showGrid = settings.showGrid;
            options.snapToGrid = settings.snapToGrid;
            options.snapToGuides = settings.snapToGuides;
            options.gridSize = settings.gridSize;
            options.wireframe = settings.wireframe;
            options.showHiddenElements = settings.showHiddenElements;
            options.showRulers = settings.showRulers;
            options.errorNodes = m_errorNodes;
            options.warningNodes = m_warningNodes;
            m_canvas.Draw(m_model, options);
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 2.0f);
        ImGui::InvisibleButton("##designerRightSplitter", ImVec2(4.0f, mainHeight));
        if (ImGui::IsItemActive())
        {
            m_rightWidth = std::clamp(
                m_rightWidth - io.MouseDelta.x,
                260.0f,
                maxRightWidth);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SameLine(0.0f, 2.0f);

        ImGui::BeginChild("##designerInspector", ImVec2(m_rightWidth, mainHeight), true);
        m_inspector.Draw(m_model, &m_canvas.GetRenderer());
        ImGui::EndChild();

        DrawDiagnostics();
        DrawPendingDialog();
    }

    void UIDocumentDesigner::DrawToolbar()
    {
        auto* undoService = TryGetUndoService();

        const bool canSave = CanSave();
        if (!canSave)
            ImGui::BeginDisabled();
        if (ImGui::Button("Save"))
            SaveDocument();
        if (!canSave)
            ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canSave)
        {
            ImGui::SetTooltip(
                m_model.HasErrors()
                    ? "Fix validation errors before saving."
                    : "The document has no unsaved changes.");
        }

        ImGui::SameLine();
        if (ImGui::Button("Revert"))
            ReloadDocument();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reload the document from disk.");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        const bool canUndo = undoService != nullptr && undoService->CanUndo() &&
            !undoService->IsPending() && !undoService->IsFrozen() && !undoService->IsPoisoned();
        if (!canUndo)
            ImGui::BeginDisabled();
        if (ImGui::Button("Undo") && undoService != nullptr)
            (void)undoService->RequestUndo();
        if (!canUndo)
            ImGui::EndDisabled();
        if (!canUndo && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                undoService == nullptr
                    ? "The global Undo service is unavailable."
                    : undoService->GetUndoUnavailableReason().c_str());
        }

        ImGui::SameLine();
        const bool canRedo = undoService != nullptr && undoService->CanRedo() &&
            !undoService->IsPending() && !undoService->IsFrozen() && !undoService->IsPoisoned();
        if (!canRedo)
            ImGui::BeginDisabled();
        if (ImGui::Button("Redo") && undoService != nullptr)
            (void)undoService->RequestRedo();
        if (!canRedo)
            ImGui::EndDisabled();
        if (!canRedo && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                undoService == nullptr
                    ? "The global Undo service is unavailable."
                    : undoService->GetRedoUnavailableReason().c_str());
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button("Fit"))
            m_canvas.FrameAll(m_model);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Frame the whole document (F).");
        ImGui::SameLine();
        if (ImGui::Button("-"))
            m_canvas.ZoomOut(m_model);
        ImGui::SameLine();
        ImGui::Text("%.0f%%", m_canvas.GetZoom() * 100.0f);
        ImGui::SameLine();
        if (ImGui::Button("+"))
            m_canvas.ZoomIn(m_model);
        ImGui::SameLine();
        if (ImGui::Button("100%"))
            m_canvas.SetZoom(m_model, 1.0f);

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Device reference resolution: writes the root layout size so the
        // designer and the runtime canvas share one authority.
        float referenceWidth = 0.0f;
        float referenceHeight = 0.0f;
        if (const auto* root = m_model.FindNode(m_model.GetRootGuid()))
        {
            const auto size = root->value("layout", Json::object()).value("size", Json::array());
            if (size.is_array() && size.size() >= 2u &&
                size[0].is_number() && size[1].is_number())
            {
                referenceWidth = size[0].get<float>();
                referenceHeight = size[1].get<float>();
            }
        }
        int presetIndex = -1;
        for (size_t index = 0u;
             index < NLS::Editor::UIDocumentTools::UIDocumentCanvasView::GetDevicePresetCount();
             ++index)
        {
            const auto size =
                NLS::Editor::UIDocumentTools::UIDocumentCanvasView::GetDevicePresetSize(
                    static_cast<int>(index));
            if (std::abs(size.x - referenceWidth) < 0.5f &&
                std::abs(size.y - referenceHeight) < 0.5f)
            {
                presetIndex = static_cast<int>(index);
                break;
            }
        }
        const char* presetLabel = presetIndex >= 0
            ? NLS::Editor::UIDocumentTools::UIDocumentCanvasView::GetDevicePresetName(presetIndex)
            : "Custom";
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo("##devicePreset", presetLabel))
        {
            for (size_t index = 0u;
                 index < NLS::Editor::UIDocumentTools::UIDocumentCanvasView::GetDevicePresetCount();
                 ++index)
            {
                const bool selected = static_cast<int>(index) == presetIndex;
                if (ImGui::Selectable(
                        NLS::Editor::UIDocumentTools::UIDocumentCanvasView::GetDevicePresetName(
                            static_cast<int>(index)),
                        selected))
                {
                    m_canvas.ApplyDevicePreset(m_model, static_cast<int>(index));
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reference resolution stored in the root layout size.");

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        auto& settings =
            NLS::Editor::Settings::EditorSettings::GetUIDesignerSettingsObject();
        ImGui::Checkbox("Grid", &settings.showGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &settings.snapToGrid);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(62.0f);
        ImGui::DragFloat("##gridSize", &settings.gridSize, 0.25f, 1.0f, 128.0f, "%.0f px");
        ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &settings.wireframe);
        ImGui::SameLine();
        ImGui::Checkbox("Hidden", &settings.showHiddenElements);

        const auto filename = ReadFilename(m_model.GetPath());
        ImGui::SameLine();
        ImGui::TextDisabled(
            "%s%s",
            filename.c_str(),
            m_model.IsDirty() ? " *" : "");
    }

    void UIDocumentDesigner::DrawExternalChangeBanner()
    {
        if (!m_externalChange)
            return;

        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            m_externalConflict
                ? ImVec4(0.20f, 0.13f, 0.09f, 1.0f)
                : ImVec4(0.16f, 0.15f, 0.08f, 1.0f));
        ImGui::BeginChild(
            "##externalChange",
            ImVec2(0.0f, m_externalConflict ? 52.0f : 30.0f),
            true);
        if (m_externalConflict)
        {
            ImGui::TextColored(
                ImVec4(0.93f, 0.72f, 0.36f, 1.0f),
                "The file changed on disk while this document has unsaved changes.");
        }
        else
        {
            ImGui::TextColored(
                ImVec4(0.93f, 0.72f, 0.36f, 1.0f),
                "The file changed on disk.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload"))
        {
            LoadDocument(m_model.GetPath());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(m_externalConflict ? "Keep Local" : "Dismiss"))
        {
            m_externalChange = false;
            m_externalConflict = false;
            std::error_code error;
            const auto time = std::filesystem::last_write_time(m_model.GetPath(), error);
            if (!error)
            {
                m_lastWriteTime = time;
                m_hasLastWriteTime = true;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void UIDocumentDesigner::DrawLeftColumn()
    {
        const float totalHeight = ImGui::GetContentRegionAvail().y;
        const float hierarchyHeight = std::max(140.0f, totalHeight * 0.56f);

        ImGui::BeginChild("##hierarchyPanel", ImVec2(0.0f, hierarchyHeight), true);
        ImGui::TextUnformatted("Hierarchy");
        ImGui::Separator();
        m_hierarchy.Draw(m_model);
        ImGui::EndChild();

        ImGui::BeginChild("##palettePanel", ImVec2(0.0f, 0.0f), true);
        DrawPalette();
        ImGui::EndChild();
    }

    void UIDocumentDesigner::DrawPalette()
    {
        ImGui::TextUnformatted("Palette");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint(
            "##paletteFilter",
            "Search controls",
            m_paletteFilter.data(),
            m_paletteFilter.size());
        ImGui::Separator();

        const auto needle = ToLower(m_paletteFilter.data());
        const auto parentGuid = m_model.GetSelectedNodeGuid().empty()
            ? m_model.GetRootGuid()
            : m_model.GetSelectedNodeGuid();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttonWidth = std::max(
            60.0f,
            (ImGui::GetContentRegionAvail().x - spacing) * 0.5f);

        int placed = 0;
        for (const auto* type : kPaletteTypes)
        {
            if (!needle.empty() && ToLower(type).find(needle) == std::string::npos)
                continue;

            ImGui::PushID(type);
            if (ImGui::Button(type, ImVec2(buttonWidth, 0.0f)))
            {
                m_model.AddChild(parentGuid, type);
                m_canvas.RequestFrameSelection();
            }
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(
                    NLS::Editor::UIDocumentTools::kUIDesignerPalettePayload,
                    type,
                    std::strlen(type));
                ImGui::TextUnformatted(type);
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Click: add as a child of the selected element.\n"
                    "Drag onto the canvas: create at the drop position.");
            }
            ImGui::PopID();

            ++placed;
            if ((placed % 2) == 1)
                ImGui::SameLine();
        }

        if (placed == 0)
            ImGui::TextDisabled("No matching controls.");
        else if ((placed % 2) == 1)
            ImGui::NewLine();

        ImGui::Separator();
        ImGui::TextDisabled("Drop a control on the canvas to position it.");
    }

    void UIDocumentDesigner::DrawDiagnostics()
    {
        size_t errorCount = 0u;
        size_t warningCount = 0u;
        for (const auto& diagnostic : m_model.GetDiagnostics())
            (diagnostic.error ? errorCount : warningCount)++;

        ImGui::BeginChild("##designerDiagnostics", ImVec2(0.0f, 0.0f), true);
        ImGui::TextUnformatted("Problems");
        ImGui::SameLine();
        ImGui::TextColored(
            errorCount > 0u ? ImVec4(0.93f, 0.41f, 0.36f, 1.0f) : ImVec4(0.52f, 0.51f, 0.58f, 1.0f),
            "%zu errors",
            errorCount);
        ImGui::SameLine();
        ImGui::TextColored(
            warningCount > 0u ? ImVec4(0.88f, 0.72f, 0.36f, 1.0f) : ImVec4(0.52f, 0.51f, 0.58f, 1.0f),
            "%zu warnings",
            warningCount);
        if (!m_model.GetDiagnostics().empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Click a row to select the element.");
        }
        ImGui::Separator();

        ImGui::BeginChild("##designerDiagnosticRows", ImVec2(0.0f, 0.0f), false);
        if (m_model.GetDiagnostics().empty())
        {
            ImGui::TextDisabled("No validation errors.");
        }
        else
        {
            for (const auto& diagnostic : m_model.GetDiagnostics())
            {
                const auto* node = m_model.FindNodeByDiagnosticPath(diagnostic.path);
                const ImVec4 color = diagnostic.error
                    ? ImVec4(0.93f, 0.41f, 0.36f, 1.0f)
                    : ImVec4(0.88f, 0.72f, 0.36f, 1.0f);
                const auto label = diagnostic.path + "  " + diagnostic.message;
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (node != nullptr)
                {
                    if (ImGui::Selectable(label.c_str()))
                    {
                        m_model.SelectNode(node->value("nodeGuid", std::string {}));
                        m_canvas.RequestFrameSelection();
                    }
                }
                else
                {
                    ImGui::TextUnformatted(label.c_str());
                }
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void UIDocumentDesigner::DrawPendingDialog()
    {
        if (m_openPendingDialog)
        {
            ImGui::OpenPopup("Unsaved UI Document");
            m_openPendingDialog = false;
        }

        if (!ImGui::BeginPopupModal(
                "Unsaved UI Document",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        ImGui::TextUnformatted("This UI document has unsaved changes.");
        ImGui::TextDisabled("%s", m_model.GetPath().generic_string().c_str());
        if (m_pendingSaveFailed)
        {
            ImGui::TextColored(
                ImVec4(0.93f, 0.41f, 0.36f, 1.0f),
                "Save failed. Fix the validation errors in Problems first.");
        }
        ImGui::Separator();

        if (ImGui::Button("Save", ImVec2(110.0f, 0.0f)))
        {
            if (m_model.Save())
            {
                const auto pending = m_pending;
                ImGui::CloseCurrentPopup();
                ResolvePending(pending.action, pending.path);
            }
            else
            {
                m_pendingSaveFailed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(110.0f, 0.0f)))
        {
            const auto pending = m_pending;
            ImGui::CloseCurrentPopup();
            ResolvePending(pending.action, pending.path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
        {
            m_pending = {};
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    void UIDocumentDesigner::HandleShortcuts()
    {
        auto& io = ImGui::GetIO();
        if (!m_shortcutFocus || io.WantTextInput || m_hierarchy.IsRenaming())
            return;
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            return;

        const bool control = io.KeyCtrl;
        const bool shift = io.KeyShift;

        if (!control && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            m_canvas.CancelGesture(m_model);
        if (control && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        {
            if (m_canvas.HasActiveGesture())
            {
                m_canvas.CancelGesture(m_model);
            }
            else if (auto* undo = TryGetUndoService())
            {
                (void)(shift ? undo->RequestRedo() : undo->RequestUndo());
            }
        }

        // While a canvas gesture owns the pointer, save/structural commands and
        // nudges must wait for its commit or cancel.
        if (m_canvas.HasActiveGesture())
            return;

        if (control && ImGui::IsKeyPressed(ImGuiKey_S, false))
            SaveDocument();
        if (control && ImGui::IsKeyPressed(ImGuiKey_Y, false))
        {
            if (auto* undo = TryGetUndoService())
                (void)undo->RequestRedo();
        }
        if (control && ImGui::IsKeyPressed(ImGuiKey_D, false))
            m_canvas.DuplicateSelection(m_model);
        if (control && ImGui::IsKeyPressed(ImGuiKey_C, false))
            m_canvas.CopySelection(m_model);
        if (control && ImGui::IsKeyPressed(ImGuiKey_V, false))
            m_canvas.PasteClipboard(m_model);

        if (!control && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            m_canvas.DeleteSelection(m_model);
        if (!control && ImGui::IsKeyPressed(ImGuiKey_F2, false))
            m_hierarchy.RequestRename(m_model, m_model.GetSelectedNodeGuid());
        if (!control && ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            if (shift)
                m_canvas.RequestFrameSelection();
            else
                m_canvas.RequestFrameAll();
        }

        float deltaX = 0.0f;
        float deltaY = 0.0f;
        if (!control)
        {
            const float step = shift ? 10.0f : 1.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                deltaX -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                deltaX += step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                deltaY -= step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                deltaY += step;
        }
        if (std::abs(deltaX) > 0.0f || std::abs(deltaY) > 0.0f)
            m_canvas.NudgeSelection(m_model, deltaX, deltaY);

        if (control &&
            (ImGui::IsKeyPressed(ImGuiKey_Equal, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)))
        {
            m_canvas.ZoomIn(m_model);
        }
        if (control &&
            (ImGui::IsKeyPressed(ImGuiKey_Minus, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)))
        {
            m_canvas.ZoomOut(m_model);
        }
        if (control && ImGui::IsKeyPressed(ImGuiKey_0, false))
            m_canvas.SetZoom(m_model, 1.0f);
    }

    void UIDocumentDesigner::PollExternalChanges()
    {
        if (!m_model.IsLoaded() || !m_hasLastWriteTime)
            return;
        if (ImGui::GetTime() < m_nextExternalCheckTime)
            return;
        m_nextExternalCheckTime = ImGui::GetTime() + 0.5;

        std::error_code error;
        const auto time = std::filesystem::last_write_time(m_model.GetPath(), error);
        if (error)
            return;
        if (time != m_lastWriteTime)
        {
            m_externalChange = true;
            m_externalConflict = m_model.IsDirty();
        }
    }

    void UIDocumentDesigner::UpdateDiagnosticNodes()
    {
        m_lastDiagnosticRevision = m_model.GetDocumentRevision();
        m_errorNodes.clear();
        m_warningNodes.clear();
        for (const auto& diagnostic : m_model.GetDiagnostics())
        {
            const auto* node = m_model.FindNodeByDiagnosticPath(diagnostic.path);
            if (node == nullptr)
                continue;
            const auto guid = node->value("nodeGuid", std::string {});
            if (guid.empty())
                continue;
            auto& target = diagnostic.error ? m_errorNodes : m_warningNodes;
            if (std::find(target.begin(), target.end(), guid) == target.end())
                target.push_back(guid);
        }
    }
}
