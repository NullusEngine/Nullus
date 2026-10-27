#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <VFX/Authoring/VfxAuthoring.h>

namespace NLS::Editor::VFXTools
{
// Process-wide copy buffer shared by every Particle Editor document. The
// payload is the editor-data JSON of the source system plus the selected ids,
// so pasting into another stack or another system uses the same code path and
// the source asset is never referenced after the copy.
class VfxClipboard final
{
public:
    static VfxClipboard& Get();

    void CopyModules(const VFX::Authoring::System& source,const std::vector<std::string>& moduleIds);
    void CopyEmitters(const VFX::Authoring::System& source,const std::vector<std::string>& emitterIds);
    void Clear();

    [[nodiscard]] bool Empty() const { return m_systemJson.empty(); }
    [[nodiscard]] bool HasModules() const { return !Empty() && !m_moduleIds.empty(); }
    [[nodiscard]] bool HasEmitters() const { return !Empty() && !m_emitterIds.empty(); }
    [[nodiscard]] size_t ModuleCount() const { return m_moduleIds.size(); }
    [[nodiscard]] size_t EmitterCount() const { return m_emitterIds.size(); }
    [[nodiscard]] const std::vector<std::string>& CopiedModuleIds() const { return m_moduleIds; }

    // Appends copies of the selected modules to the destination stack. Ids and
    // embedded dynamic-input graphs are regenerated. Returns the new ids.
    bool PasteModules(VFX::Authoring::System& target,std::string_view emitterId,
        std::string_view stackId,std::vector<std::string>& newIds,std::string& error);
    // Appends copies of the selected emitters with regenerated emitter, stack,
    // module and graph ids. Positions are offset so they stay visible.
    bool PasteEmitters(VFX::Authoring::System& target,std::vector<std::string>& newIds,std::string& error);

private:
    std::string m_systemJson;
    std::vector<std::string> m_moduleIds, m_emitterIds;
};
}
