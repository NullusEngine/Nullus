#include "EditorTools.h"
#include "Plugins/PluginManager.h"
#include "ImGui/imgui.h"
#include <cstdio>

namespace NLS::Examples
{
class EditorTools final : public Core::Modules::IModule
{
public:
    bool Startup(Core::Modules::ModuleContext& context) override
    {
        auto* registry = &context.extensions;
        m_panel = registry->Register(context.scope, {"org.nullus.sample.editor.status", "editor.panel"},
            [registry](Core::Modules::ExtensionCall&) {
                ImGui::TextUnformatted("Nullus Editor Tools Plugin");
                auto counter = registry->Find("org.nullus.sample.runtime.counter");
                Core::Modules::ExtensionCall query{0};
                if (counter.Invoke(query)) ImGui::Text("Runtime extension ticks: %llu", static_cast<unsigned long long>(query.value));
                else ImGui::TextUnformatted("Runtime extension is unavailable.");
            });
        m_command = registry->Register(context.scope, {"org.nullus.sample.editor.log", "editor.command"},
            [](Core::Modules::ExtensionCall&) { std::fprintf(stderr, "[EditorTools] Plugin command executed.\n"); });
        return true;
    }
    void Shutdown() noexcept override { m_command.Reset(); m_panel.Reset(); }
    bool IsStopped() const noexcept override { return true; }
private:
    Core::Modules::RegistrationToken m_panel, m_command;
};
std::unique_ptr<Core::Modules::IModule> CreateEditorTools()
{
    return std::make_unique<EditorTools>();
}
}
