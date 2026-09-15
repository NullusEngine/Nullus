#include "Inspector/ScriptedComponentEditor.h"

#include <exception>
#include <utility>

#include "Inspector/InspectorRegistry.h"

namespace NLS::Editor::Inspector
{
namespace
{
// A scripted editor that throws on every frame must not spam the console or
// stall the Inspector.  After this many consecutive failures the editor is
// retired for the session and the user is told why.
constexpr int32_t kFailureThreshold = 3;
} // namespace

ScriptedComponentEditor::ScriptedComponentEditor(std::string editorTypeName, std::string targetTypeName)
    : m_editorTypeName(std::move(editorTypeName))
    , m_targetTypeName(std::move(targetTypeName))
{
}

ScriptedComponentEditor::~ScriptedComponentEditor()
{
    ReleaseManagedInstance();
}

const InspectorTargetSet* ScriptedComponentEditor::GetScriptTargets() const
{
    return &targets();
}

void ScriptedComponentEditor::EnsureManagedInstance()
{
    if (m_handle != 0u || m_disabled)
        return;

    auto& host = Scripting::EditorScriptHost::Get();
    const auto* api = host.GetManagedApi();
    if (api == nullptr || api->createEditor == nullptr)
        return;

    const auto handle = host.RegisterTarget(*this);
    try
    {
        api->createEditor(handle, m_editorTypeName.c_str());
        if (api->onEnable != nullptr)
            api->onEnable(handle);
    }
    catch (const std::exception& exception)
    {
        host.UnregisterTarget(handle);
        m_lastError = std::string("Scripted editor creation failed: ") + exception.what();
        host.LogError(m_lastError);
        m_disabled = true;
        return;
    }
    catch (...)
    {
        host.UnregisterTarget(handle);
        m_lastError = "Scripted editor creation failed with an unknown error.";
        host.LogError(m_lastError);
        m_disabled = true;
        return;
    }

    m_handle = handle;
}

void ScriptedComponentEditor::ReleaseManagedInstance()
{
    if (m_handle == 0u)
        return;

    auto& host = Scripting::EditorScriptHost::Get();
    const auto* api = host.GetManagedApi();
    if (api != nullptr)
    {
        try
        {
            if (api->onDisable != nullptr)
                api->onDisable(m_handle);
            if (api->destroyEditor != nullptr)
                api->destroyEditor(m_handle);
        }
        catch (...)
        {
            // A failing teardown must still release the native slot.
        }
    }
    host.UnregisterTarget(m_handle);
    m_handle = 0u;
}

void ScriptedComponentEditor::OnEnable()
{
    EnsureManagedInstance();
}

void ScriptedComponentEditor::OnDisable()
{
    ReleaseManagedInstance();
}

void ScriptedComponentEditor::DrawUnavailableMessage(std::string_view reason)
{
    std::string text = "Scripted editor '";
    text += m_editorTypeName;
    text += "' is unavailable";
    if (!reason.empty())
    {
        text += ": ";
        text += std::string(reason);
    }
    else
    {
        text += ". The editor-scripts assembly is not loaded.";
    }
    HelpBox(std::move(text), ComponentEditorMessageKind::Error);
}

void ScriptedComponentEditor::OnInspectorGUI()
{
    if (m_disabled)
    {
        DrawUnavailableMessage(m_lastError);
        DrawDefaultInspector();
        return;
    }

    EnsureManagedInstance();
    const auto* api = Scripting::EditorScriptHost::Get().GetManagedApi();
    if (api == nullptr || api->onInspectorGUI == nullptr || m_handle == 0u)
    {
        DrawUnavailableMessage({});
        DrawDefaultInspector();
        return;
    }

    bool succeeded = false;
    try
    {
        succeeded = api->onInspectorGUI(m_handle) != 0u;
    }
    catch (const std::exception& exception)
    {
        m_lastError = exception.what();
        succeeded = false;
    }
    catch (...)
    {
        m_lastError = "Unknown managed editor exception.";
        succeeded = false;
    }

    if (succeeded)
    {
        m_failureCount = 0;
        return;
    }

    ++m_failureCount;
    if (m_failureCount < kFailureThreshold)
    {
        DrawUnavailableMessage(m_lastError);
        DrawDefaultInspector();
        return;
    }

    m_disabled = true;
    Scripting::EditorScriptHost::Get().LogError(
        "Scripted editor '" + m_editorTypeName + "' was disabled after repeated failures: " + m_lastError);
    DrawUnavailableMessage(m_lastError);
    DrawDefaultInspector();
}

bool ScriptedComponentEditor::ScriptPropertyField(
    std::string_view propertyPath,
    const std::string& label,
    const std::string& tooltip,
    bool includeChildren)
{
    ComponentEditorPropertyOptions options;
    options.label = label;
    options.tooltip = tooltip;
    options.includeChildren = includeChildren;
    return PropertyField(propertyPath, std::move(options));
}

bool ScriptedComponentEditor::ScriptDrawDefaultInspector()
{
    return DrawDefaultInspector();
}

void ScriptedComponentEditor::ReportScriptError(std::string_view message)
{
    m_lastError = std::string(message);
    if (m_errorCount == 0)
        Scripting::EditorScriptHost::Get().LogError("Scripted editor '" + m_editorTypeName + "': " + m_lastError);
    ++m_errorCount;
}
} // namespace NLS::Editor::Inspector
