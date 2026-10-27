#pragma once

// Shared host bridge between Inspector::ReflectedPropertyEditor and a document
// editing model. Editors implement IReflectedEditHost over their own transaction
// boundary (snapshot JSON, VFX Working/Preview, ...); the helpers below map the
// editor's changed/completed/canceled signals onto Begin/Preview/Commit/Cancel
// exactly once instead of every editor re-implementing the state machine.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Inspector/ReflectedPropertyEditor.h"

namespace NLS::Editor::Inspector
{
    class IReflectedEditHost
    {
    public:
        virtual ~IReflectedEditHost() = default;

        // Opens a continuous edit. Return true when the host now owns an open
        // transaction. Called once per gesture, before the first preview.
        [[nodiscard]] virtual bool BeginPropertyEdit(std::string_view label) = 0;

        // Publishes the host's current candidate. Called after the caller has
        // written the changed fields into the host document/candidate.
        [[nodiscard]] virtual bool PreviewPropertyEdit() = 0;

        [[nodiscard]] virtual bool CommitPropertyEdit() = 0;
        [[nodiscard]] virtual bool CancelPropertyEdit() = 0;
        [[nodiscard]] virtual bool IsPropertyEditOpen() const = 0;

        // Human-readable failure from the last Begin/Preview/Commit. Optional.
        [[nodiscard]] virtual std::string_view GetPropertyEditError() const { return {}; }
    };

    // Maps an already drawn edit onto a host transaction. Editors that build a
    // candidate and validate it themselves call this after mutating it:
    //
    //   auto edit = editor.Draw(id, label, value, options);
    //   if (edit.changed) { candidate = Build(value); }
    //   ApplyReflectedEdit(host, edit, label);
    [[nodiscard]] inline bool ApplyReflectedEdit(
        IReflectedEditHost& host,
        const ReflectedPropertyEdit& edit,
        const std::string_view label)
    {
        if (edit.canceled)
            return host.IsPropertyEditOpen() ? host.CancelPropertyEdit() : true;

        bool succeeded = true;
        if (edit.changed)
        {
            if (!host.IsPropertyEditOpen())
                succeeded = host.BeginPropertyEdit(label);
            if (succeeded)
                succeeded = host.PreviewPropertyEdit();
            if (!succeeded)
            {
                if (host.IsPropertyEditOpen())
                    (void)host.CancelPropertyEdit();
                return false;
            }
        }
        if (edit.completed && host.IsPropertyEditOpen())
            succeeded = host.CommitPropertyEdit() && succeeded;
        return succeeded;
    }

    // Draws one reflected value and applies it through the host transaction.
    // `apply` receives the edited value and the changed field paths so the host
    // can write only the affected properties.
    template <typename Editor, typename T, typename Apply>
    ReflectedPropertyEdit DrawReflectedValue(
        Editor& editor,
        IReflectedEditHost& host,
        const std::string_view id,
        const std::string_view label,
        const T& value,
        const ReflectedPropertyOptions& options,
        Apply&& apply)
    {
        T copy = value;
        meta::Variant variant(copy);
        auto edit = editor.Draw(id, label, variant, options);
        if (edit.changed && !edit.canceled)
        {
            if (!host.IsPropertyEditOpen() && !host.BeginPropertyEdit(label))
                return edit;
            apply(variant.GetValue<T>(), edit.paths);
        }
        (void)ApplyReflectedEdit(host, edit, label);
        return edit;
    }

    // Draws a set of same-typed reflected values through the host transaction.
    // Mixed fields are displayed by the shared Inspector. `apply` receives the
    // edited primary value and changed paths; document hosts apply the common
    // value to every selected target.
    template <typename Editor, typename T, typename Apply>
    ReflectedPropertyEdit DrawReflectedValues(
        Editor& editor,
        IReflectedEditHost& host,
        const std::string_view id,
        const std::string_view label,
        std::vector<T>& values,
        const ReflectedPropertyOptions& options,
        Apply&& apply)
    {
        auto edit = editor.DrawValues(id, label, values, options);
        if (edit.changed && !edit.canceled && !values.empty())
        {
            if (!host.IsPropertyEditOpen() && !host.BeginPropertyEdit(label))
                return edit;
            apply(values.front(), edit.paths);
        }
        (void)ApplyReflectedEdit(host, edit, label);
        return edit;
    }

    // End-of-frame backstop. Flushes the editor's internal pending edits and
    // commits an open host transaction once no ImGui item or popup owns the
    // pointer. Returns true when the host committed.
    template <typename Editor>
    [[nodiscard]] inline bool FinishReflectedEdits(
        Editor& editor,
        IReflectedEditHost& host)
    {
        (void)editor.FinishFrame();
        if constexpr(requires {editor.TakeCanceledEdits();})
        {
            if(editor.TakeCanceledEdits())
            {if(host.IsPropertyEditOpen()) (void)host.CancelPropertyEdit();return false;}
        }
        if (!host.IsPropertyEditOpen())
            return false;
        if (editor.HasActiveControls() || ImGui::IsAnyItemActive())
            return false;
        // Canvas gestures can share this document's transaction with property
        // controls. The backstop must not finish an active graph/gizmo edit.
        if(ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))
            return false;
        return host.CommitPropertyEdit();
    }
}
