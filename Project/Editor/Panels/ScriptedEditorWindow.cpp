#include "Panels/ScriptedEditorWindow.h"

#include <exception>
#include <utility>

#include <imgui.h>

namespace NLS::Editor::Panels
{
namespace
{
constexpr int32_t kFailureThreshold = 3;
} // namespace

ScriptedEditorWindow::ScriptedEditorWindow(
    const std::string& p_title,
    std::string editorTypeName,
    const UI::PanelWindowSettings& p_windowSettings)
    : UI::PanelWindow(p_title, true, p_windowSettings)
    , m_editorTypeName(std::move(editorTypeName))
{
}

ScriptedEditorWindow::~ScriptedEditorWindow()
{
    ReleaseManagedInstance();
}

void ScriptedEditorWindow::EnsureManagedInstance()
{
    if (m_handle != 0u || m_disabled)
        return;

    auto& host = Scripting::EditorScriptHost::Get();
    const auto* api = host.GetManagedApi();
    if (api == nullptr || api->createWindow == nullptr)
        return;

    const auto handle = host.RegisterTarget(*this);
    try
    {
        api->createWindow(handle, m_editorTypeName.c_str());
        if (api->onWindowEnable != nullptr)
            api->onWindowEnable(handle);
    }
    catch (const std::exception& exception)
    {
        host.UnregisterTarget(handle);
        m_lastError = exception.what();
        m_disabled = true;
        return;
    }
    catch (...)
    {
        host.UnregisterTarget(handle);
        m_lastError = "Unknown managed editor window error.";
        m_disabled = true;
        return;
    }
    m_handle = handle;
}

void ScriptedEditorWindow::ReleaseManagedInstance()
{
    if (m_handle == 0u)
        return;

    auto& host = Scripting::EditorScriptHost::Get();
    const auto* api = host.GetManagedApi();
    if (api != nullptr)
    {
        try
        {
            if (api->onWindowDisable != nullptr)
                api->onWindowDisable(m_handle);
            if (api->destroyWindow != nullptr)
                api->destroyWindow(m_handle);
        }
        catch (...)
        {
            // Teardown must always release the native slot.
        }
    }
    host.UnregisterTarget(m_handle);
    m_handle = 0u;
}

void ScriptedEditorWindow::ReportScriptError(std::string_view message)
{
    m_lastError = std::string(message);
}

void ScriptedEditorWindow::OnBeforeDrawWidgets()
{
    if (!m_disabled)
        EnsureManagedInstance();

    ImGui::PushID(static_cast<int>(m_handle & 0xFFFFFFFFu));

    if (m_disabled)
    {
        ImGui::TextWrapped(
            "Scripted editor window '%s' is unavailable: %s",
            m_editorTypeName.c_str(),
            m_lastError.empty() ? "the editor-scripts assembly is not loaded." : m_lastError.c_str());
        ImGui::PopID();
        return;
    }

    const auto* api = Scripting::EditorScriptHost::Get().GetManagedApi();
    if (api == nullptr || api->onWindowGUI == nullptr || m_handle == 0u)
    {
        ImGui::TextWrapped(
            "Scripted editor window '%s' is unavailable: the editor-scripts assembly is not loaded.",
            m_editorTypeName.c_str());
        ImGui::PopID();
        return;
    }

    bool succeeded = false;
    try
    {
        succeeded = api->onWindowGUI(m_handle) != 0u;
    }
    catch (const std::exception& exception)
    {
        m_lastError = exception.what();
        succeeded = false;
    }
    catch (...)
    {
        m_lastError = "Unknown managed editor window exception.";
        succeeded = false;
    }

    if (succeeded)
    {
        m_failureCount = 0;
    }
    else
    {
        ++m_failureCount;
        ImGui::TextColored(ImVec4(0.90f, 0.38f, 0.34f, 1.0f), "%s", m_lastError.c_str());
        if (m_failureCount >= kFailureThreshold)
            m_disabled = true;
    }

    ImGui::PopID();
}

void ScriptedEditorWindow::OnAfterDrawWidgets()
{
}
} // namespace NLS::Editor::Panels
