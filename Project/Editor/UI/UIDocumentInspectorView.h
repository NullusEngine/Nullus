#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Inspector/ReflectedEditHost.h"
#include "Properties/PropertyPanel.h"
#include "UI/UIDocumentCanvasRenderer.h"
#include "UI/UIDocumentDesignerModel.h"
#include "UI/UIDocumentNodeEdit.h"

namespace NLS::Editor::UIDocumentTools
{
    // UI document inspector.
    //
    // Standard fields are drawn through the shared reflected property editor
    // (editor-side view models in UIDocumentNodeEdit.h) and applied through the
    // shared IReflectedEditHost transaction bridge. The .ui JSON stays the source
    // of truth: writes go through UIDocumentDesignerModel, which keeps
    // validation, global undo and one-command-per-gesture semantics. The anchor
    // preset and the live rect-derived position remain custom rows because they
    // need the runtime layout result.
    class UIDocumentInspectorView final : public Inspector::IReflectedEditHost
    {
    public:
        UIDocumentInspectorView();
        ~UIDocumentInspectorView();

        void Draw(UIDocumentDesignerModel& model, const UIDocumentCanvasRenderer* renderer);

    private:
        // Inspector::IReflectedEditHost
        [[nodiscard]] bool BeginPropertyEdit(std::string_view label) override;
        [[nodiscard]] bool PreviewPropertyEdit() override;
        [[nodiscard]] bool CommitPropertyEdit() override;
        [[nodiscard]] bool CancelPropertyEdit() override;
        [[nodiscard]] bool IsPropertyEditOpen() const override;

        void DrawElementSection(UIDocumentDesignerModel& model);
        void DrawRectSection(
            UIDocumentDesignerModel& model,
            const UIDocumentCanvasRenderer* renderer);
        void DrawLayoutSection(UIDocumentDesignerModel& model);
        void DrawStyleSection(UIDocumentDesignerModel& model);
        void DrawContentSection(UIDocumentDesignerModel& model);
        void DrawActions(UIDocumentDesignerModel& model);
        void DrawAnchorPreset(
            UIDocumentDesignerModel& model,
            const UIDocumentCanvasRenderer* renderer);
        void DrawPositionRow(
            UIDocumentDesignerModel& model,
            const UIDocumentCanvasRenderer* renderer);

        // Custom rows (anchor preset, live position) use the same host bridge.
        void BeginEdit(UIDocumentDesignerModel& model, const char* label);
        void FinishEdit(UIDocumentDesignerModel& model, bool cancelled);
        [[nodiscard]] std::string SelectionKey(const UIDocumentDesignerModel& model) const;
        [[nodiscard]] bool IsMixed(const UIDocumentDesignerModel& model, const char* path) const;

        template <typename T, typename Apply>
        Inspector::ReflectedPropertyEdit DrawReflectedSection(
            UIDocumentDesignerModel& model,
            const char* id,
            const char* label,
            std::vector<T>& values,
            const Inspector::ReflectedPropertyOptions& options,
            Apply&& apply)
        {
            (void)model;
            return Inspector::DrawReflectedValues(
                m_properties,
                *this,
                id,
                label,
                values,
                options,
                std::forward<Apply>(apply));
        }

        Properties::PropertyPanel m_properties;
        std::shared_ptr<const void> m_lifetime;
        UIDocumentDesignerModel* m_activeModel = nullptr;
        bool m_editOpen = false;
        uint64_t m_scopeGeneration = 0u;
        std::string m_cachedSelectionKey;
    };
}
