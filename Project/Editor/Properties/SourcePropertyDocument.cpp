#include "Properties/SourcePropertyDocument.h"
#include <fstream>
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

#include <Guid.h>

namespace NLS::Editor::Properties
{
namespace Detail
{
struct EditState
{
    std::weak_ptr<DocumentState> target;
    Undo::UndoCommandInfo info;
    std::string before;
    std::string desired;
    std::string applied;
};
} // namespace Detail

namespace
{
using namespace Undo;
using Detail::DocumentState;
using Detail::EditState;
std::string Serialize(const std::string& source)
{
    return source;
}
bool Deserialize(const std::string& source, std::string& result, std::string& error)
{
    result = source;
    error.clear();
    return true;
}
std::string HashFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(bytes);
}

UndoValidationResult ValidateState(const std::weak_ptr<DocumentState>& target, const UndoScopeId& scope,
                                   const std::string& expected)
{
    const auto state = target.lock();
    if (!state || state->scope != scope)
        return UndoValidationResult::Stale("Property document was closed or replaced");
    if (state->source != expected)
        return UndoValidationResult::Conflict("Property document changed outside this edit");
    return UndoValidationResult::Ready();
}

UndoStartResult Restore(DocumentState& state, const std::string& source)
{
    std::string candidate;
    std::string error;
    if (!Deserialize(source, candidate, error))
        return UndoStartResult::Failed(error);
    state.asset = std::move(candidate);
    state.source = source;
    ++state.revision;
    return UndoStartResult::Completed();
}

class DocumentCommand final : public IEditorUndoCommand
{
public:
    DocumentCommand(std::weak_ptr<DocumentState> target, UndoCommandInfo info, std::string before, std::string after)
        : m_target(std::move(target)), m_info(std::move(info)), m_before(std::move(before)), m_after(std::move(after))
    {
        m_info.estimatedBytes = m_before.size() + m_after.size();
    }

    UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }
    UndoValidationResult Validate(UndoDirection direction, const UndoExecutionContext&) const override
    {
        const auto state = m_target.lock();
        // RecordApplied validates a continuous edit in the already-previewed state.
        if (direction == UndoDirection::Redo && state && state->scope == m_info.scopes[0] && state->source == m_after)
            return UndoValidationResult::Ready();
        return ValidateState(m_target, m_info.scopes[0], direction == UndoDirection::Undo ? m_after : m_before);
    }
    UndoStartResult Apply(UndoDirection direction, const UndoExecutionContext&, UndoCompletion) override
    {
        const auto state = m_target.lock();
        if (!state)
            return UndoStartResult::Failed("Property document is closed");
        return Restore(*state, direction == UndoDirection::Undo ? m_before : m_after);
    }
    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    std::weak_ptr<DocumentState> m_target;
    UndoCommandInfo m_info;
    std::string m_before;
    std::string m_after;
};

class DocumentEdit final : public IEditorUndoEdit
{
public:
    explicit DocumentEdit(std::shared_ptr<EditState> edit)
        : m_edit(std::move(edit))
    {
    }
    UndoValidationResult Validate(const UndoExecutionContext&) const override
    {
        return ValidateState(m_edit->target, m_edit->info.scopes[0], m_edit->applied);
    }
    UndoStartResult Preview(const UndoExecutionContext&, UndoCompletion) override
    {
        const auto state = m_edit->target.lock();
        if (!state)
            return UndoStartResult::Failed("Property document is closed");
        const auto result = Restore(*state, m_edit->desired);
        if (result.state == UndoStartState::Completed)
            m_edit->applied = m_edit->desired;
        return result;
    }
    UndoStartResult Cancel(const UndoExecutionContext&, UndoCompletion) override
    {
        const auto state = m_edit->target.lock();
        if (!state)
            return UndoStartResult::Failed("Property document is closed");
        const auto result = Restore(*state, m_edit->before);
        if (result.state == UndoStartState::Completed)
            m_edit->applied = m_edit->before;
        return result;
    }
    bool HasChanges() const override
    {
        return m_edit->before != m_edit->applied;
    }
    UndoCommandPtr BuildCommand() override
    {
        if (!HasChanges())
            return nullptr;
        return std::make_unique<DocumentCommand>(m_edit->target, m_edit->info, m_edit->before, m_edit->applied);
    }

private:
    std::shared_ptr<EditState> m_edit;
};

UndoCommandInfo Info(const DocumentState& state, std::string label)
{
    UndoCommandInfo info;
    info.label = std::move(label);
    info.sourceModule = "Property document";
    info.scopes = {state.scope};
    info.affected = {state.id};
    return info;
}
} // namespace

SourcePropertyDocument::SourcePropertyDocument(Undo::EditorUndoService* undo)
    : m_undo(undo)
{
    New({});
}
SourcePropertyDocument::~SourcePropertyDocument()
{
    (void)EndEdit(false);
}

void SourcePropertyDocument::Replace(std::string asset, std::filesystem::path path)
{
    (void)EndEdit(false);
    if (m_undo && m_state)
        m_undo->InvalidateScope(m_state->scope, UndoInvalidationReason::Reloaded);
    m_state = std::make_shared<DocumentState>();
    m_state->asset = std::move(asset);
    m_state->id = NLS::Guid::New().ToString();
    m_state->path = std::move(path);
    m_state->source = Serialize(m_state->asset);
    m_state->savedSource = m_state->path.empty() ? std::string{} : m_state->source;
    static std::atomic<uint64_t> generation{1};
    m_state->scope = {UndoScopeKind::Asset, "material-source:" + m_state->id, generation.fetch_add(1)};
    if (m_undo && !m_state->path.empty())
        m_undo->MarkSaved(m_state->scope, m_state->source);
    m_error.clear();
}

void SourcePropertyDocument::New(std::string source)
{
    Replace(std::move(source), {});
}

bool SourcePropertyDocument::Load(const std::filesystem::path& path)
{
    std::string asset;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        m_error = "Cannot open material source: " + path.string();
        return false;
    }
    asset.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    Replace(std::move(asset), path);
    m_state->savedFileHash = HashFile(path);
    return true;
}

bool SourcePropertyDocument::Save(const std::filesystem::path& path)
{
    if (!EndEdit())
        return false;
    const auto destination = path.empty() ? m_state->path : path;
    if (destination.empty())
    {
        m_error = "Select a destination for the material source";
        return false;
    }
    if (std::filesystem::exists(destination) && (destination != m_state->path || HashFile(destination) != m_state->savedFileHash))
    {
        m_error = "Material source changed outside the editor";
        return false;
    }
    if (!NLS::Render::ShaderCompiler::WriteShaderArtifactTextAtomic(destination.generic_string(), m_state->source, &m_error))
        return false;
    m_state->path = destination;
    m_state->savedSource = m_state->source;
    m_state->savedFileHash = HashFile(destination);
    if (m_undo)
        m_undo->MarkSaved(m_state->scope, m_state->source);
    return true;
}

bool SourcePropertyDocument::Apply(std::string candidate, std::string label)
{
    if (IsEditing())
        return PreviewEdit(std::move(candidate));
    const auto source = Serialize(candidate);
    if (source == m_state->source)
        return true;
    std::string validated;
    if (!Deserialize(source, validated, m_error))
        return false;
    if (!m_undo)
        return Restore(*m_state, source).state == UndoStartState::Completed;
    const auto result = m_undo->Execute(
        std::make_unique<DocumentCommand>(m_state, Info(*m_state, std::move(label)), m_state->source, source));
    m_error = result.message;
    return result.Succeeded();
}

bool SourcePropertyDocument::BeginEdit(std::string label)
{
    if (IsEditing())
        return true;
    auto edit = std::make_shared<EditState>();
    edit->target = m_state;
    edit->info = Info(*m_state, std::move(label));
    edit->before = edit->applied = edit->desired = m_state->source;
    if (m_undo)
    {
        UndoEditDesc desc;
        desc.info = edit->info;
        desc.edit = std::make_unique<DocumentEdit>(edit);
        const auto result = m_undo->BeginEditSession(std::move(desc));
        if (!result.result.Succeeded())
        {
            m_error = result.result.message;
            return false;
        }
        m_session = result.session;
    }
    m_edit = std::move(edit);
    return true;
}

bool SourcePropertyDocument::PreviewEdit(std::string candidate)
{
    if (!m_edit)
        return false;
    const auto source = Serialize(candidate);
    std::string validated;
    if (!Deserialize(source, validated, m_error))
        return false;
    m_edit->desired = source;
    if (m_undo)
    {
        const auto result = m_undo->PreviewEditSession(*m_session);
        m_error = result.message;
        return result.Succeeded();
    }
    const auto result = Restore(*m_state, source);
    m_edit->applied = source;
    return result.state == UndoStartState::Completed;
}

bool SourcePropertyDocument::EndEdit(bool commit)
{
    if (!m_edit)
        return true;
    bool succeeded = true;
    if (m_undo && m_session && m_undo->IsEditSessionActive(*m_session))
    {
        const auto result = commit ? m_undo->CommitEditSession(*m_session) : m_undo->CancelEditSession(*m_session);
        succeeded = result.Succeeded();
        m_error = result.message;
    }
    else if (!m_undo && !commit)
        succeeded = Restore(*m_state, m_edit->before).state == UndoStartState::Completed;
    if (!succeeded)
        return false;
    m_edit.reset();
    m_session.reset();
    return succeeded;
}
} // namespace NLS::Editor::Properties

namespace NLS::Editor::Properties
{
void SourcePropertyDocument::MarkSaved()
{
    m_state->savedSource = m_state->source;
    if (m_undo)
        m_undo->MarkSaved(m_state->scope, m_state->source);
}
} // namespace NLS::Editor::Properties
