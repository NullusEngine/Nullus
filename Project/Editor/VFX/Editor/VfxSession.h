#pragma once
#include "VfxDocument.h"
#include <VFX/SystemInstance.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include <utility>

namespace NLS::Editor::VFXTools
{
enum class VfxSelection { System, Emitter, Stack, Module };
// A document owns an independent compile generation, selection and transport.
// Rebuild is transactional: a failed edit never replaces the running program.
class VfxSession final
{
public:
    explicit VfxSession(Undo::EditorUndoService* undo = nullptr) : document(undo) {}
    bool Rebuild(bool force = false);
    void SetLibrary(NLS::VFX::Authoring::SourceLibrary library);
    void SetLibraryError(std::string error);
    const NLS::VFX::Authoring::SourceLibrary& Library() const { return m_library; }
    const NLS::VFX::Authoring::System& Resolved() const { return m_resolved; }
    const std::vector<NLS::VFX::Authoring::SourceDependency>& Dependencies() const { return m_dependencies; }
    bool Tick(double deltaTime);
    // Single-frame stepping and scrubbing: advances by the exact delta without
    // the playback speed factor and while paused.
    bool Advance(double deltaTime);
    bool Seek(double age);
    bool RequestSeek(double age);
    // Publishes the editor viewport camera so camera-relative modules and
    // billboards read the same snapshot the preview renderer uses.
    void SetCameraContext(const NLS::VFX::CameraContext& camera);
    bool IsSeeking() const { return m_seekTarget.has_value(); }
    void Reset();
    bool SetParameter(const std::string& name, NLS::VFX::Value value);
    void SelectEmitter(std::string id);
    void SelectSystem();
    void SelectStack(std::string emitter, std::string stack);
    void SelectModule(std::string emitter, std::string stack, std::string module);
    // Additive selection (Ctrl/Shift click) toggles the item in the multi
    // selection while the primary selection follows the last click.
    void SelectEmitter(std::string id, bool additive);
    void SelectModule(std::string emitter, std::string stack, std::string module, bool additive);
    void ClearSelection();
    [[nodiscard]] bool IsEmitterSelected(const std::string& emitterId) const;
    [[nodiscard]] bool IsModuleSelected(const std::string& moduleId) const;
    [[nodiscard]] const std::vector<std::string>& SelectedEmitters() const { return m_selectedEmitters; }
    [[nodiscard]] const std::vector<std::string>& SelectedModules() const { return m_selectedModules; }
    // Batch operations apply to the whole selection in one document undo step.
    bool SetModulesEnabled(const std::vector<std::string>& moduleIds, bool enabled);
    bool SetEmittersEnabled(const std::vector<std::string>& emitterIds, bool enabled);
    bool DeleteModules(const std::vector<std::string>& moduleIds);
    bool DeleteEmitters(const std::vector<std::string>& emitterIds);
    std::vector<std::string> DuplicateModules(const std::vector<std::string>& moduleIds);
    // Selects the ids as a multi-selection and makes the first one primary.
    void SelectModules(const std::vector<std::string>& moduleIds);
    const NLS::VFX::Authoring::System& EditingSystem();
    void ReconcileSelection();
    bool AddModule(const std::string& emitter, const std::string& stack, const std::string& script, bool asset = false);
    bool SetModuleEnabled(const std::string& emitter, const std::string& module, bool enabled);
    bool SetEmitterEnabled(const std::string& emitter, bool enabled);
    const std::string& ActionError() const { return m_actionError; }
    const std::shared_ptr<const NLS::VFX::CompiledSystem>& Program() const { return m_program; }
    const NLS::VFX::SystemInstance& Instance() const { return m_instance; }
    uint64_t ProgramGeneration() const { return m_generation; }
    // True when the document changed after the last accepted compile attempt,
    // for example while Auto Compile is disabled or a rebuild is only queued.
    bool IsOutOfDate() const { return m_revision != document.Revision(); }
    const std::vector<NLS::VFX::Authoring::Issue>& Issues() const { return m_issues; }
    const std::string& AttemptedSource() const { return m_attemptedSource; }
    const std::unordered_map<std::string,NLS::VFX::Value>& Parameters() const { return m_parameters; }
    std::vector<std::shared_ptr<const NLS::VFX::SystemFrame>> TakeFrames() { return std::exchange(m_frames,{}); }
    VfxDocument document;
    std::string selectedEmitter, selectedStack, selectedModule;
    VfxSelection selection = VfxSelection::Emitter;
    bool playing = true, autoCompile = true, resetOnChange = true, resimulatePaused = true;
    double playbackSpeed = 1, rangeEnd = 5;
private:
    std::vector<std::string> m_selectedEmitters, m_selectedModules;
    uint64_t m_revision = 0, m_generation = 0;
    std::string m_attemptedSource;
    std::shared_ptr<const NLS::VFX::CompiledSystem> m_program;
    NLS::VFX::SystemInstance m_instance;
    std::vector<NLS::VFX::Authoring::Issue> m_issues;
    std::unordered_map<std::string,NLS::VFX::Value> m_parameters;
    std::vector<std::shared_ptr<const NLS::VFX::SystemFrame>> m_frames;
    std::optional<double> m_seekTarget;
    NLS::VFX::CameraContext m_camera;
    double m_seekElapsed = 0;
    NLS::VFX::Authoring::SourceLibrary m_library;
    NLS::VFX::Authoring::System m_resolved;
    std::vector<NLS::VFX::Authoring::SourceDependency> m_dependencies;
    std::string m_librarySource;
    std::string m_libraryError;
    NLS::VFX::Authoring::System m_editingSystem;
    uint64_t m_editingRevision = 0;
    std::weak_ptr<const void> m_editingLifetime;
    std::string m_actionError;
    bool PumpSeek(uint32_t steps);
};
}
