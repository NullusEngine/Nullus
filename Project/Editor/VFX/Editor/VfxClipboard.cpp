#include "VfxClipboard.h"

#include <algorithm>
#include <Guid.h>

namespace NLS::Editor::VFXTools
{
namespace author = NLS::VFX::Authoring;

VfxClipboard& VfxClipboard::Get()
{
    static VfxClipboard clipboard;
    return clipboard;
}
void VfxClipboard::CopyModules(const author::System& source,const std::vector<std::string>& moduleIds)
{
    m_systemJson = author::Serialize(source,true);
    m_moduleIds = moduleIds;
    m_emitterIds.clear();
}
void VfxClipboard::CopyEmitters(const author::System& source,const std::vector<std::string>& emitterIds)
{
    m_systemJson = author::Serialize(source,true);
    m_emitterIds = emitterIds;
    m_moduleIds.clear();
}
void VfxClipboard::Clear()
{
    m_systemJson.clear();
    m_moduleIds.clear();
    m_emitterIds.clear();
}
bool VfxClipboard::PasteModules(author::System& target,const std::string_view emitterId,
    const std::string_view stackId,std::vector<std::string>& newIds,std::string& error)
{
    newIds.clear();
    if (!HasModules()) { error = "The clipboard does not contain modules"; return false; }
    author::System source;
    if (!author::Deserialize(m_systemJson,source,error)) return false;
    author::Stack* destination = nullptr;
    for (auto& emitter : target.emitters) if (emitter.settings.id == emitterId)
        for (auto& stack : emitter.stacks)
        {
            if (!stack.inheritedId.empty()) continue;
            if (stack.id == stackId) destination = &stack;
        }
    if (!destination) { error = "Select a local destination stage before pasting"; return false; }
    std::vector<author::Module> copies;
    for (const auto& id : m_moduleIds)
    {
        bool found = false;
        for (const auto& emitter : source.emitters) for (const auto& stack : emitter.stacks) for (const auto& module : stack.modules)
            if (module.id == id) { copies.push_back(module); found = true; break; }
        if (!found) { error = "A copied module no longer exists in its source system"; return false; }
    }
    for (auto& copy : copies)
    {
        copy.id = Guid::New().ToString();
        copy.inheritedId.clear();
        for (auto& [name,input] : copy.inputs) if (input.mode == author::InputMode::DynamicInput)
            input.expression.id = Guid::New().ToString();
        newIds.push_back(copy.id);
        destination->modules.push_back(std::move(copy));
    }
    error.clear();
    return true;
}
bool VfxClipboard::PasteEmitters(author::System& target,std::vector<std::string>& newIds,std::string& error)
{
    newIds.clear();
    if (!HasEmitters()) { error = "The clipboard does not contain emitters"; return false; }
    author::System source;
    if (!author::Deserialize(m_systemJson,source,error)) return false;
    std::vector<author::Emitter> copies;
    for (const auto& id : m_emitterIds)
    {
        const auto found = std::find_if(source.emitters.begin(),source.emitters.end(),[&](const auto& emitter) { return emitter.settings.id == id; });
        if (found == source.emitters.end()) { error = "A copied emitter no longer exists in its source system"; return false; }
        copies.push_back(*found);
    }
    for (auto& copy : copies)
    {
        copy.settings.id = Guid::New().ToString();
        copy.settings.spawn.id = Guid::New().ToString();
        copy.settings.update.id = Guid::New().ToString();
        copy.position[0] += 330.0f*(static_cast<float>(target.emitters.size())+1.0f);
        for (auto& stack : copy.stacks)
        {
            stack.id = Guid::New().ToString();
            stack.inheritedId.clear();
            for (auto& module : stack.modules)
            {
                module.id = Guid::New().ToString();
                module.inheritedId.clear();
                for (auto& [name,input] : module.inputs) if (input.mode == author::InputMode::DynamicInput)
                    input.expression.id = Guid::New().ToString();
            }
        }
        for (auto& item : copy.overrides) item.module.clear();
        newIds.push_back(copy.settings.id);
        target.emitters.push_back(std::move(copy));
    }
    error.clear();
    return true;
}
}
