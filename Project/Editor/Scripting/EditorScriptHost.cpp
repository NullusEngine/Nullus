#include "Scripting/EditorScriptHost.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <imgui.h>

#include <Debug/Logger.h>
#include <Reflection/TypeConfig.h>

#include <Core/ServiceLocator.h>

#include "Core/EditorActions.h"
#include "Core/PanelsManager.h"
#include "Inspector/InspectorRegistry.h"
#include "Inspector/ScriptedComponentEditor.h"
#include "Panels/EditorTopBar.h"
#include "Scripting/EditorScriptWindowService.h"

namespace NLS::Editor::Scripting
{
namespace
{
constexpr const char* kScriptEditorIdentifierPrefix = "script:";

template <typename T>
uint32_t CopyUtf8(const T& value, char* output, uint32_t capacity)
{
    const auto size = static_cast<uint32_t>(value.size());
    if (output != nullptr && capacity > 0u)
    {
        const auto copy = std::min(size, capacity - 1u);
        std::memcpy(output, value.data(), copy);
        output[copy] = '\0';
    }
    return size + 1u;
}

std::string_view SafeString(const char* value)
{
    return value != nullptr ? std::string_view(value) : std::string_view();
}

// --- Host table entry points.  Each one validates the handle before touching
// the target so a stale managed handle after a domain reload cannot crash the
// editor.

IEditorScriptTarget* TargetFor(EditorScriptHandle handle)
{
    return EditorScriptHost::Get().ResolveTarget(handle);
}

uint8_t HostRegisterComponentEditor(
    const char* editorTypeName,
    const char* targetTypeName,
    uint8_t editorForChildClasses,
    uint8_t supportsMultiObject,
    int32_t priority)
{
    EditorScriptDescriptor descriptor;
    descriptor.editorTypeName = std::string(SafeString(editorTypeName));
    descriptor.targetTypeName = std::string(SafeString(targetTypeName));
    descriptor.editorForChildClasses = editorForChildClasses != 0u;
    descriptor.supportsMultiObject = supportsMultiObject != 0u;
    descriptor.priority = priority;

    std::string error;
    if (!EditorScriptHost::Get().RegisterComponentEditor(descriptor, error))
    {
        EditorScriptHost::Get().LogError(std::move(error));
        return 0u;
    }
    return 1u;
}

uint8_t HostRegisterMenuItem(const char* menuPath, const char* editorTypeName)
{
    EditorScriptMenuItem item;
    item.menuPath = std::string(SafeString(menuPath));
    item.editorTypeName = std::string(SafeString(editorTypeName));

    std::string error;
    if (!EditorScriptHost::Get().RegisterMenuItem(item, error))
    {
        EditorScriptHost::Get().LogError(std::move(error));
        return 0u;
    }
    return 1u;
}

void HostClearRegistrations()
{
    EditorScriptHost::Get().ClearRegistrations();
}

uint8_t HostIsHandleValid(EditorScriptHandle handle)
{
    return EditorScriptHost::Get().ResolveTarget(handle) != nullptr ? 1u : 0u;
}

uint32_t HostGetTargetCount(EditorScriptHandle handle)
{
    const auto* target = TargetFor(handle);
    const auto* targets = target != nullptr ? target->GetScriptTargets() : nullptr;
    return targets != nullptr ? static_cast<uint32_t>(targets->targets.size()) : 0u;
}

uint32_t HostGetTargetDisplayName(EditorScriptHandle handle, uint32_t index, char* output, uint32_t capacity)
{
    const auto* target = TargetFor(handle);
    const auto* targets = target != nullptr ? target->GetScriptTargets() : nullptr;
    if (targets == nullptr || index >= targets->targets.size())
        return CopyUtf8(std::string_view(), output, capacity);

    const auto& entry = targets->targets[index];
    const auto& text = !entry.resourcePath.empty() ? entry.resourcePath : entry.typeName;
    return CopyUtf8(text, output, capacity);
}

uint32_t HostGetSearchText(EditorScriptHandle handle, char* output, uint32_t capacity)
{
    const auto* target = TargetFor(handle);
    return CopyUtf8(target != nullptr ? target->GetScriptSearchText() : std::string_view(), output, capacity);
}

int32_t HostGetTargetInstanceId(EditorScriptHandle handle, uint32_t index)
{
    const auto* target = TargetFor(handle);
    const auto* targets = target != nullptr ? target->GetScriptTargets() : nullptr;
    if (targets == nullptr || index >= targets->targets.size())
        return 0;
    return static_cast<int32_t>(targets->targets[index].instanceId);
}

uint8_t HostIsMultiObject(EditorScriptHandle handle)
{
    const auto* target = TargetFor(handle);
    const auto* targets = target != nullptr ? target->GetScriptTargets() : nullptr;
    return targets != nullptr && targets->targets.size() > 1u ? 1u : 0u;
}

void HostReportError(EditorScriptHandle handle, const char* message)
{
    auto* target = TargetFor(handle);
    if (target != nullptr)
        target->ReportScriptError(SafeString(message));
    EditorScriptHost::Get().LogError(std::string(SafeString(message)));
}

uint8_t HostPropertyField(
    EditorScriptHandle handle,
    const char* propertyPath,
    const char* label,
    const char* tooltip,
    uint8_t includeChildren)
{
    auto* target = TargetFor(handle);
    if (target == nullptr || !target->IsInspectorTarget())
        return 0u;
    return target->ScriptPropertyField(
        SafeString(propertyPath),
        std::string(SafeString(label)),
        std::string(SafeString(tooltip)),
        includeChildren != 0u) ? 1u : 0u;
}

uint8_t HostDrawDefaultInspector(EditorScriptHandle handle)
{
    auto* target = TargetFor(handle);
    if (target == nullptr || !target->IsInspectorTarget())
        return 0u;
    return target->ScriptDrawDefaultInspector() ? 1u : 0u;
}

void HostLabel(EditorScriptHandle handle, const char* text, const char* tooltip)
{
    if (TargetFor(handle) == nullptr)
        return;
    ImGui::TextUnformatted(text != nullptr ? text : "");
    if (tooltip != nullptr && *tooltip != '\0' && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
}

void HostSpace(EditorScriptHandle handle, float pixels)
{
    if (TargetFor(handle) == nullptr)
        return;
    const float scale = ImGui::GetIO().FontGlobalScale > 0.0f ? ImGui::GetIO().FontGlobalScale : 1.0f;
    ImGui::Dummy(ImVec2(0.0f, pixels * scale));
}

uint8_t HostToggle(EditorScriptHandle handle, const char* label, uint8_t value, uint8_t* output)
{
    if (TargetFor(handle) == nullptr)
        return 0u;
    bool current = value != 0u;
    const bool changed = ImGui::Checkbox(label != nullptr ? label : "", &current);
    if (changed && output != nullptr)
        *output = current ? 1u : 0u;
    return changed ? 1u : 0u;
}

uint8_t HostIntField(EditorScriptHandle handle, const char* label, int32_t value, int32_t* output)
{
    if (TargetFor(handle) == nullptr)
        return 0u;
    int current = value;
    const bool changed = ImGui::DragInt(label != nullptr ? label : "", &current);
    if (changed && output != nullptr)
        *output = current;
    return changed ? 1u : 0u;
}

uint8_t HostFloatField(EditorScriptHandle handle, const char* label, float value, float* output)
{
    if (TargetFor(handle) == nullptr)
        return 0u;
    float current = value;
    const bool changed = ImGui::DragFloat(label != nullptr ? label : "", &current);
    if (changed && output != nullptr)
        *output = current;
    return changed ? 1u : 0u;
}

uint8_t HostSlider(
    EditorScriptHandle handle,
    const char* label,
    float value,
    float minimum,
    float maximum,
    float* output)
{
    if (TargetFor(handle) == nullptr)
        return 0u;
    float current = value;
    const bool changed = ImGui::SliderFloat(label != nullptr ? label : "", &current, minimum, maximum);
    if (changed && output != nullptr)
        *output = current;
    return changed ? 1u : 0u;
}

uint8_t HostTextField(
    EditorScriptHandle handle,
    const char* label,
    const char* value,
    char* output,
    uint32_t capacity)
{
    if (TargetFor(handle) == nullptr)
        return 0u;
    if (output == nullptr || capacity == 0u)
        return 0u;
    const auto current = std::string(SafeString(value));
    const auto copy = std::min(current.size(), static_cast<size_t>(capacity) - 1u);
    std::memcpy(output, current.data(), copy);
    output[copy] = '\0';
    const bool changed = ImGui::InputText(label != nullptr ? label : "", output, capacity);
    return changed ? 1u : 0u;
}

uint8_t HostButton(EditorScriptHandle handle, const char* label, const char* tooltip, uint8_t enabled)
{
    if (TargetFor(handle) == nullptr)
        return 0u;
    bool clicked = false;
    if (enabled == 0u)
        ImGui::BeginDisabled();
    clicked = ImGui::Button(label != nullptr ? label : "");
    if (enabled == 0u)
        ImGui::EndDisabled();
    if (tooltip != nullptr && *tooltip != '\0' && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return clicked ? 1u : 0u;
}

void HostHelpBox(EditorScriptHandle handle, const char* text, int32_t kind)
{
    if (TargetFor(handle) == nullptr)
        return;
    const ImVec4 color = kind == static_cast<int32_t>(EditorScriptMessageKind::Error)
        ? ImVec4(0.90f, 0.38f, 0.34f, 1.0f)
        : kind == static_cast<int32_t>(EditorScriptMessageKind::Warning)
            ? ImVec4(0.95f, 0.72f, 0.28f, 1.0f)
            : ImVec4(0.60f, 0.75f, 0.90f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text != nullptr ? text : "");
    ImGui::PopStyleColor();
}

void HostBeginHorizontal(EditorScriptHandle handle)
{
    if (TargetFor(handle) == nullptr)
        return;
    ImGui::BeginGroup();
}

void HostEndHorizontal(EditorScriptHandle handle)
{
    if (TargetFor(handle) == nullptr)
        return;
    ImGui::EndGroup();
    ImGui::SameLine();
}

void HostBeginVertical(EditorScriptHandle handle)
{
    if (TargetFor(handle) == nullptr)
        return;
    ImGui::BeginGroup();
}

void HostEndVertical(EditorScriptHandle handle)
{
    if (TargetFor(handle) == nullptr)
        return;
    ImGui::EndGroup();
}

void HostOpenWindow(const char* editorTypeName)
{
    EditorScriptHost::Get().OpenScriptWindow(SafeString(editorTypeName));
}

void HostCloseWindow(const char* editorTypeName)
{
    EditorScriptHost::Get().CloseScriptWindow(SafeString(editorTypeName));
}

const EditorScriptHostApiTable& BuildHostApiTable()
{
    static const EditorScriptHostApiTable table = []
    {
        EditorScriptHostApiTable value;
        value.header.size = sizeof(EditorScriptHostApiTable);
        value.header.abiVersion = 1u;
        value.header.schemaHash = 0u;
        value.registerComponentEditor = &HostRegisterComponentEditor;
        value.registerMenuItem = &HostRegisterMenuItem;
        value.clearRegistrations = &HostClearRegistrations;
        value.isHandleValid = &HostIsHandleValid;
        value.getTargetCount = &HostGetTargetCount;
        value.getTargetDisplayName = &HostGetTargetDisplayName;
        value.getTargetInstanceId = &HostGetTargetInstanceId;
        value.getSearchText = &HostGetSearchText;
        value.isMultiObject = &HostIsMultiObject;
        value.reportError = &HostReportError;
        value.propertyField = &HostPropertyField;
        value.drawDefaultInspector = &HostDrawDefaultInspector;
        value.label = &HostLabel;
        value.space = &HostSpace;
        value.toggle = &HostToggle;
        value.intField = &HostIntField;
        value.floatField = &HostFloatField;
        value.slider = &HostSlider;
        value.textField = &HostTextField;
        value.button = &HostButton;
        value.helpBox = &HostHelpBox;
        value.beginHorizontal = &HostBeginHorizontal;
        value.endHorizontal = &HostEndHorizontal;
        value.beginVertical = &HostBeginVertical;
        value.endVertical = &HostEndVertical;
        value.openWindow = &HostOpenWindow;
        value.closeWindow = &HostCloseWindow;
        return value;
    }();
    return table;
}
} // namespace

const EditorScriptHostApiTable& GetEditorScriptHostApiTable()
{
    return BuildHostApiTable();
}

EditorScriptHost& EditorScriptHost::Get()
{
    static EditorScriptHost host;
    return host;
}

void EditorScriptHost::ClearRegistrations()
{
    m_descriptors.clear();
    m_menuItems.clear();
}

bool EditorScriptHost::RegisterComponentEditor(const EditorScriptDescriptor& descriptor, std::string& error)
{
    if (descriptor.editorTypeName.empty() || descriptor.targetTypeName.empty())
    {
        error = "A scripted component editor descriptor must name both the editor and its target type.";
        return false;
    }

    const auto duplicate = std::find_if(m_descriptors.begin(), m_descriptors.end(), [&](const auto& existing)
    {
        return existing.targetTypeName == descriptor.targetTypeName &&
            existing.editorTypeName == descriptor.editorTypeName;
    });
    if (duplicate != m_descriptors.end())
    {
        error = "Duplicate scripted component editor for target '" + descriptor.targetTypeName + "'.";
        return false;
    }

    m_descriptors.push_back(descriptor);
    return true;
}

bool EditorScriptHost::RegisterMenuItem(const EditorScriptMenuItem& item, std::string& error)
{
    if (item.menuPath.empty())
    {
        error = "A scripted editor menu item must declare a menu path.";
        return false;
    }

    const auto duplicate = std::find_if(m_menuItems.begin(), m_menuItems.end(), [&](const auto& existing)
    {
        return existing.menuPath == item.menuPath;
    });
    if (duplicate != m_menuItems.end())
    {
        error = "Duplicate scripted editor menu path '" + item.menuPath + "'.";
        return false;
    }

    m_menuItems.push_back(item);
    return true;
}

void EditorScriptHost::ApplyComponentEditorRegistrations()
{
    auto& registry = Inspector::InspectorRegistry::Get();
    registry.UnregisterComponentEditorsByIdentifierPrefix(kScriptEditorIdentifierPrefix);
    ++m_registrationGeneration;

    int32_t fieldCount = 0;
    for (const auto& descriptor : m_descriptors)
    {
        const auto inspectedType = meta::ResolveTypeByName(descriptor.targetTypeName.c_str(), false);
        // A reflected engine component registers against its reflection type. A
        // C#/Lua Behaviour has no distinct reflection type - every script
        // component shares ScriptComponent - so it registers against the
        // script's own type name and is resolved through
        // ResolveScriptComponentEditor.
        const bool isScriptTarget = !inspectedType.IsValid();
        if (isScriptTarget && descriptor.targetTypeName.empty())
        {
            LogError("Scripted editor '" + descriptor.editorTypeName + "' targets an empty type name.");
            continue;
        }

        const auto editorTypeName = descriptor.editorTypeName;
        const auto targetTypeName = descriptor.targetTypeName;
        const auto scriptComponentType = meta::ResolveTypeByName("NLS::Scripting::ScriptComponent", false);
        if (isScriptTarget && !scriptComponentType.IsValid())
        {
            LogError("Scripted editor '" + descriptor.editorTypeName +
                "' targets script component '" + targetTypeName +
                "' but the ScriptComponent reflection type is unavailable.");
            continue;
        }

        Inspector::ComponentEditorRegistration registration;
        registration.inspectedType = isScriptTarget ? scriptComponentType : inspectedType;
        registration.identifier = std::string(kScriptEditorIdentifierPrefix) +
            std::to_string(m_registrationGeneration) + ":" + editorTypeName;
        registration.editorForChildClasses = !isScriptTarget && descriptor.editorForChildClasses;
        registration.isFallback = false;
        registration.supportsMultiObject = descriptor.supportsMultiObject;
        registration.priority = descriptor.priority;
        registration.scriptTypeName = isScriptTarget ? targetTypeName : std::string {};
        registration.create = [editorTypeName, targetTypeName]
        {
            return std::make_unique<Inspector::ScriptedComponentEditor>(editorTypeName, targetTypeName);
        };
        registry.RegisterComponentEditor(std::move(registration));
        ++fieldCount;
    }

    if (fieldCount > 0)
    {
        NLS::Debug::Logger::Log(
            "[Editor Scripts] Registered " + std::to_string(fieldCount) + " scripted component editor(s).",
            NLS::Debug::ELogLevel::LOG_INFO,
            NLS::Debug::ELogMode::CONSOLE);
    }
}

EditorScriptHandle EditorScriptHost::RegisterTarget(IEditorScriptTarget& target)
{
    uint32_t slot = 0;
    if (!m_freeSlots.empty())
    {
        slot = m_freeSlots.back();
        m_freeSlots.pop_back();
    }
    else
    {
        m_slots.push_back({});
        slot = static_cast<uint32_t>(m_slots.size() - 1u);
    }

    auto& entry = m_slots[slot];
    entry.target = &target;
    entry.alive = true;
    entry.generation = static_cast<uint16_t>(entry.generation + 1u);
    if (entry.generation == 0u)
        entry.generation = 1u;

    return (static_cast<uint64_t>(entry.generation) << 32u) | static_cast<uint64_t>(slot + 1u);
}

void EditorScriptHost::UnregisterTarget(EditorScriptHandle handle)
{
    const auto slot = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    const auto generation = static_cast<uint16_t>(handle >> 32u);
    if (slot == 0u || slot > m_slots.size())
        return;

    auto& entry = m_slots[slot - 1u];
    if (!entry.alive || entry.generation != generation)
        return;

    entry.target = nullptr;
    entry.alive = false;
    m_freeSlots.push_back(slot - 1u);
}

IEditorScriptTarget* EditorScriptHost::ResolveTarget(EditorScriptHandle handle) const
{
    const auto slot = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
    const auto generation = static_cast<uint16_t>(handle >> 32u);
    if (slot == 0u || slot > m_slots.size())
        return nullptr;

    const auto& entry = m_slots[slot - 1u];
    if (!entry.alive || entry.generation != generation)
        return nullptr;
    return entry.target;
}

bool EditorScriptHost::IsHandleValid(EditorScriptHandle handle) const
{
    return ResolveTarget(handle) != nullptr;
}

void EditorScriptHost::SetManagedApi(const ManagedEditorScriptApiTable* api)
{
    m_managed = api;
}

void EditorScriptHost::ResetManagedApi()
{
    m_managed = nullptr;
    ClearRegistrations();
}

void EditorScriptHost::RescanManagedRegistrations()
{
    if (m_managed == nullptr || m_managed->scan == nullptr)
        return;

    ClearErrors();
    ClearRegistrations();
    try
    {
        m_managed->scan();
    }
    catch (const std::exception& exception)
    {
        LogError(std::string("Scripted editor scan failed: ") + exception.what());
        return;
    }
    catch (...)
    {
        LogError("Scripted editor scan failed with an unknown exception.");
        return;
    }

    // Registration and refresh are independent steps: a failure in one must not
    // leave the other half-applied, and none of them may escape into editor
    // startup or a frame.
    try
    {
        ApplyComponentEditorRegistrations();
    }
    catch (const std::exception& exception)
    {
        LogError(std::string("Scripted editor registration failed: ") + exception.what());
    }
    catch (...)
    {
        LogError("Scripted editor registration failed with an unknown exception.");
    }

    // The reloaded domain owns new instances, so drop the managed objects the
    // previous domain created and let the native adapters rebuild them. Native
    // handles are slot+generation values, so a stale handle left behind by an
    // editor that is still alive resolves to null instead of dereferencing.
    try
    {
        if (m_managed != nullptr)
            EditorScriptWindowService::Get().ReleaseManagedInstances();
        // A reload can add or remove menu paths; publish any new entries.
        InstallScriptMenuEntries();
    }
    catch (const std::exception& exception)
    {
        LogError(std::string("Scripted editor window/menu refresh failed: ") + exception.what());
    }
    catch (...)
    {
        LogError("Scripted editor window/menu refresh failed with an unknown exception.");
    }
}

void EditorScriptHost::OpenScriptWindow(std::string_view editorTypeName)
{
    EditorScriptWindowService::Get().OpenWindow(editorTypeName);
}

void EditorScriptHost::CloseScriptWindow(std::string_view editorTypeName)
{
    EditorScriptWindowService::Get().CloseWindow(editorTypeName);
}

void EditorScriptHost::LogError(std::string message)
{
    if (message.empty())
        return;
    if (std::find(m_errors.begin(), m_errors.end(), message) != m_errors.end())
        return;
    m_errors.push_back(message);
    NLS::Debug::Logger::Log(
        "[Editor Scripts] " + message,
        NLS::Debug::ELogLevel::LOG_ERROR,
        NLS::Debug::ELogMode::CONSOLE);
}

void InstallScriptMenuEntries()
{
    static std::vector<std::string> installedPaths;

    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
        return;

    auto& manager = NLS::Core::ServiceLocator::Get<NLS::Editor::Core::EditorActions>().GetPanelsManager();
    // The top bar only exists once Editor construction has created the panels.
    if (!manager.HasPanel("Editor Top Bar"))
        return;

    auto& topBar = manager.GetPanelAs<NLS::Editor::Panels::EditorTopBar>("Editor Top Bar");
    for (const auto& item : EditorScriptHost::Get().GetMenuItems())
    {
        if (std::find(installedPaths.begin(), installedPaths.end(), item.menuPath) != installedPaths.end())
            continue;

        const auto editorTypeName = item.editorTypeName;
        const auto menuPath = item.menuPath;
        try
        {
            const auto registered = topBar.RegisterCommand(menuPath, [editorTypeName, menuPath]
            {
                auto& host = EditorScriptHost::Get();
                const auto* api = host.GetManagedApi();
                if (api != nullptr && api->invokeMenuItem != nullptr)
                {
                    api->invokeMenuItem(menuPath.c_str());
                    return;
                }
                // Without a live managed side the only meaningful action left is
                // to show the window shell so the user sees why it is empty.
                host.OpenScriptWindow(editorTypeName);
            });
            if (!registered)
                continue;
            installedPaths.push_back(menuPath);
        }
        catch (const std::exception& exception)
        {
            EditorScriptHost::Get().LogError(
                "Scripted editor menu '" + menuPath + "' could not be registered: " + exception.what());
        }
        catch (...)
        {
            EditorScriptHost::Get().LogError(
                "Scripted editor menu '" + menuPath + "' could not be registered: unknown exception.");
        }
    }
}
} // namespace NLS::Editor::Scripting
