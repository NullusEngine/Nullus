#include "UI/UIDocumentDesignerModel.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include <Guid.h>
#include <ServiceLocator.h>

namespace NLS::Editor::UIDocumentTools
{
    namespace
    {
        using State = Detail::UIDocumentDesignerState;
        using EditState = Detail::UIDocumentDesignerEditState;
        using UndoService = NLS::Editor::Undo::EditorUndoService;
        using UndoScopeId = NLS::Editor::Undo::UndoScopeId;

        UndoService* TryGetUndoService()
        {
            if (!NLS::Core::ServiceLocator::Contains<UndoService>())
                return nullptr;
            return &NLS::Core::ServiceLocator::Get<UndoService>();
        }

        std::string CanonicalPathKey(const std::filesystem::path& path)
        {
            std::error_code error;
            auto canonical = std::filesystem::weakly_canonical(path, error);
            if (error)
            {
                error.clear();
                canonical = std::filesystem::absolute(path, error);
            }
            if (error)
                canonical = path;
            return canonical.lexically_normal().generic_string();
        }

        UndoScopeId MakeScope(const State& state)
        {
            return {
                NLS::Editor::Undo::UndoScopeKind::UIDocument,
                state.canonicalPath,
                state.generation
            };
        }

        bool ContainsNode(const nlohmann::json& node, const std::string_view nodeGuid)
        {
            if (node.value("nodeGuid", std::string {}) == nodeGuid)
                return true;
            const auto children = node.find("children");
            if (children == node.end() || !children->is_array())
                return false;
            return std::any_of(
                children->begin(),
                children->end(),
                [nodeGuid](const nlohmann::json& child) { return ContainsNode(child, nodeGuid); });
        }

        void RevalidateState(State& state)
        {
            state.diagnostics.clear();
            std::vector<NLS::Engine::UI::UIDocumentDiagnostic> runtimeDiagnostics;
            (void)NLS::Engine::UI::UIDocument::LoadFromJson(state.document.dump(), &runtimeDiagnostics);
            for (const auto& diagnostic : runtimeDiagnostics)
                state.diagnostics.push_back({ diagnostic.path, diagnostic.message, diagnostic.error });
        }

        void RestoreState(
            State& state,
            nlohmann::json document,
            std::string selectedNodeGuid,
            const bool localDirty)
        {
            state.document = std::move(document);
            const auto root = state.document.find("root");
            if (root == state.document.end() ||
                !ContainsNode(*root, selectedNodeGuid))
            {
                selectedNodeGuid = root == state.document.end()
                    ? std::string {}
                    : root->value("nodeGuid", std::string {});
            }
            state.selectedNodeGuid = std::move(selectedNodeGuid);
            state.localDirty = localDirty;
            ++state.documentRevision;
            RevalidateState(state);
        }

        // A committed designer command owns its large JSON payload through the
        // editor's blob store. The active edit keeps its live JSON in memory,
        // but once it reaches history this releases the duplicated snapshot
        // memory without capturing a raw Editor or model pointer.
        class UIDocumentSnapshotStorage final
        {
        public:
            UIDocumentSnapshotStorage() = default;
            ~UIDocumentSnapshotStorage()
            {
                ReleaseBlob();
            }

            UIDocumentSnapshotStorage(const UIDocumentSnapshotStorage&) = delete;
            UIDocumentSnapshotStorage& operator=(const UIDocumentSnapshotStorage&) = delete;

            UIDocumentSnapshotStorage(UIDocumentSnapshotStorage&& other) noexcept
                : m_inlineDocument(std::move(other.m_inlineDocument))
                , m_inlineBytes(other.m_inlineBytes)
                , m_store(std::move(other.m_store))
                , m_blob(std::move(other.m_blob))
            {
                other.m_inlineBytes = 0u;
                other.m_blob = {};
            }

            UIDocumentSnapshotStorage& operator=(UIDocumentSnapshotStorage&& other) noexcept
            {
                if (this == &other)
                    return *this;

                ReleaseBlob();
                m_inlineDocument = std::move(other.m_inlineDocument);
                m_inlineBytes = other.m_inlineBytes;
                m_store = std::move(other.m_store);
                m_blob = std::move(other.m_blob);
                other.m_inlineBytes = 0u;
                other.m_blob = {};
                return *this;
            }

            [[nodiscard]] static UIDocumentSnapshotStorage Create(nlohmann::json document)
            {
                UIDocumentSnapshotStorage result;
                const auto serialized = document.dump();
                if (auto* undo = TryGetUndoService())
                {
                    if (auto store = undo->GetSnapshotBlobStore();
                        store && store->ShouldStoreOutOfLine(serialized.size()))
                    {
                        const std::vector<uint8_t> bytes(serialized.begin(), serialized.end());
                        const auto stored = store->Store(bytes);
                        if (stored.Succeeded())
                        {
                            result.m_store = std::move(store);
                            result.m_blob = stored.reference;
                            return result;
                        }
                    }
                }

                result.m_inlineBytes = serialized.size();
                result.m_inlineDocument.emplace(std::move(document));
                return result;
            }

            [[nodiscard]] bool Materialize(nlohmann::json& document, std::string& error) const
            {
                if (m_inlineDocument.has_value())
                {
                    document = *m_inlineDocument;
                    return true;
                }
                if (!m_store || !m_blob.IsValid())
                {
                    error = "The UI document snapshot is unavailable.";
                    return false;
                }

                const auto loaded = m_store->Load(m_blob);
                if (!loaded.Succeeded())
                {
                    error = loaded.message.empty()
                        ? "The UI document snapshot could not be loaded."
                        : loaded.message;
                    return false;
                }

                const std::string serialized(loaded.bytes.begin(), loaded.bytes.end());
                auto parsed = nlohmann::json::parse(serialized, nullptr, false);
                if (parsed.is_discarded())
                {
                    error = "The UI document snapshot is invalid.";
                    return false;
                }
                document = std::move(parsed);
                return true;
            }

            [[nodiscard]] std::optional<UIDocumentSnapshotStorage> Clone() const
            {
                nlohmann::json document;
                std::string error;
                if (!Materialize(document, error))
                    return std::nullopt;
                return Create(std::move(document));
            }

            [[nodiscard]] size_t GetMemoryBytes() const noexcept
            {
                return m_inlineDocument.has_value()
                    ? m_inlineBytes
                    : sizeof(NLS::Editor::Undo::UndoBlobReference);
            }

            [[nodiscard]] size_t GetBlobBytes() const noexcept
            {
                return m_blob.IsValid() ? static_cast<size_t>(m_blob.GetByteCount()) : 0u;
            }

        private:
            void ReleaseBlob()
            {
                if (m_store && m_blob.IsValid())
                    static_cast<void>(m_store->Release(m_blob));
                m_blob = {};
                m_store.reset();
            }

            std::optional<nlohmann::json> m_inlineDocument;
            size_t m_inlineBytes = 0u;
            std::shared_ptr<NLS::Editor::Undo::UndoBlobStore> m_store;
            NLS::Editor::Undo::UndoBlobReference m_blob;
        };

        class UIDocumentSnapshotUndoCommand final : public NLS::Editor::Undo::IEditorUndoCommand
        {
        public:
            UIDocumentSnapshotUndoCommand(
                std::weak_ptr<State> state,
                NLS::Editor::Undo::UndoCommandInfo info,
                nlohmann::json beforeDocument,
                std::string beforeSelection,
                const bool beforeLocalDirty,
                nlohmann::json afterDocument,
                std::string afterSelection,
                const bool afterLocalDirty)
                : m_state(std::move(state))
                , m_info(std::move(info))
                , m_beforeDocument(UIDocumentSnapshotStorage::Create(std::move(beforeDocument)))
                , m_beforeSelection(std::move(beforeSelection))
                , m_beforeLocalDirty(beforeLocalDirty)
                , m_afterDocument(UIDocumentSnapshotStorage::Create(std::move(afterDocument)))
                , m_afterSelection(std::move(afterSelection))
                , m_afterLocalDirty(afterLocalDirty)
            {
                RefreshEstimatedSizes();
            }

            [[nodiscard]] NLS::Editor::Undo::UndoCommandInfo GetInfo() const override
            {
                return m_info;
            }

            [[nodiscard]] NLS::Editor::Undo::UndoValidationResult Validate(
                const NLS::Editor::Undo::UndoDirection,
                const NLS::Editor::Undo::UndoExecutionContext&) const override
            {
                const auto state = m_state.lock();
                if (!state || state->path.empty())
                    return NLS::Editor::Undo::UndoValidationResult::Stale("UI document is no longer open");
                if (m_info.scopes.empty() || MakeScope(*state) != m_info.scopes.front())
                    return NLS::Editor::Undo::UndoValidationResult::Stale("UI document was reloaded");
                return NLS::Editor::Undo::UndoValidationResult::Ready();
            }

            NLS::Editor::Undo::UndoStartResult Apply(
                const NLS::Editor::Undo::UndoDirection direction,
                const NLS::Editor::Undo::UndoExecutionContext&,
                NLS::Editor::Undo::UndoCompletion) override
            {
                const auto state = m_state.lock();
                if (!state)
                    return NLS::Editor::Undo::UndoStartResult::Failed("UI document is no longer open");

                if (direction == NLS::Editor::Undo::UndoDirection::Undo)
                {
                    nlohmann::json document;
                    std::string error;
                    if (!m_beforeDocument.Materialize(document, error))
                        return NLS::Editor::Undo::UndoStartResult::Failed(std::move(error));
                    RestoreState(*state, std::move(document), m_beforeSelection, m_beforeLocalDirty);
                }
                else
                {
                    nlohmann::json document;
                    std::string error;
                    if (!m_afterDocument.Materialize(document, error))
                        return NLS::Editor::Undo::UndoStartResult::Failed(std::move(error));
                    RestoreState(*state, std::move(document), m_afterSelection, m_afterLocalDirty);
                }
                return NLS::Editor::Undo::UndoStartResult::Completed();
            }

            bool TryMergeFrom(const NLS::Editor::Undo::IEditorUndoCommand& newer) override
            {
                const auto* documentCommand = dynamic_cast<const UIDocumentSnapshotUndoCommand*>(&newer);
                if (documentCommand == nullptr ||
                    documentCommand->m_info.scopes != m_info.scopes ||
                    documentCommand->m_info.mergeKey != m_info.mergeKey)
                {
                    return false;
                }

                auto replacement = documentCommand->m_afterDocument.Clone();
                if (!replacement.has_value())
                    return false;
                m_afterDocument = std::move(*replacement);
                m_afterSelection = documentCommand->m_afterSelection;
                m_afterLocalDirty = documentCommand->m_afterLocalDirty;
                RefreshEstimatedSizes();
                return true;
            }

        private:
            void RefreshEstimatedSizes()
            {
                m_info.estimatedBytes = sizeof(UIDocumentSnapshotUndoCommand) +
                    m_beforeDocument.GetMemoryBytes() + m_afterDocument.GetMemoryBytes() +
                    m_beforeSelection.size() + m_afterSelection.size();
                m_info.estimatedBlobBytes =
                    m_beforeDocument.GetBlobBytes() + m_afterDocument.GetBlobBytes();
            }

            std::weak_ptr<State> m_state;
            NLS::Editor::Undo::UndoCommandInfo m_info;
            UIDocumentSnapshotStorage m_beforeDocument;
            std::string m_beforeSelection;
            bool m_beforeLocalDirty = false;
            UIDocumentSnapshotStorage m_afterDocument;
            std::string m_afterSelection;
            bool m_afterLocalDirty = false;
        };

        class UIDocumentContinuousEdit final : public NLS::Editor::Undo::IEditorUndoEdit
        {
        public:
            UIDocumentContinuousEdit(
                std::shared_ptr<EditState> state,
                NLS::Editor::Undo::UndoCommandInfo info)
                : m_state(std::move(state))
                , m_info(std::move(info))
            {
            }

            [[nodiscard]] NLS::Editor::Undo::UndoValidationResult Validate(
                const NLS::Editor::Undo::UndoExecutionContext&) const override
            {
                if (!m_state)
                    return NLS::Editor::Undo::UndoValidationResult::Stale("UI document edit no longer exists");
                const auto target = m_state->target.lock();
                if (!target || target->path.empty())
                    return NLS::Editor::Undo::UndoValidationResult::Stale("UI document is no longer open");
                if (MakeScope(*target) != m_state->scope)
                    return NLS::Editor::Undo::UndoValidationResult::Stale("UI document was reloaded");
                return NLS::Editor::Undo::UndoValidationResult::Ready();
            }

            NLS::Editor::Undo::UndoStartResult Preview(
                const NLS::Editor::Undo::UndoExecutionContext&,
                NLS::Editor::Undo::UndoCompletion) override
            {
                if (!m_state)
                    return NLS::Editor::Undo::UndoStartResult::Failed("UI document edit no longer exists");
                const auto target = m_state->target.lock();
                if (!target || MakeScope(*target) != m_state->scope)
                    return NLS::Editor::Undo::UndoStartResult::Failed("UI document was reloaded");

                // A one-shot UI command restores its before snapshot before
                // beginning the service session.  Applying after here makes
                // Preview the sole mutation point; continuous edits reapply
                // their current state here under the same context.
                RestoreState(
                    *target,
                    m_state->afterDocument,
                    m_state->afterSelection,
                    m_state->afterLocalDirty);
                return NLS::Editor::Undo::UndoStartResult::Completed();
            }

            NLS::Editor::Undo::UndoStartResult Cancel(
                const NLS::Editor::Undo::UndoExecutionContext&,
                NLS::Editor::Undo::UndoCompletion) override
            {
                if (!m_state)
                    return NLS::Editor::Undo::UndoStartResult::Failed("UI document edit no longer exists");
                const auto target = m_state->target.lock();
                if (!target)
                    return NLS::Editor::Undo::UndoStartResult::Failed("UI document is no longer open");
                RestoreState(
                    *target,
                    m_state->beforeDocument,
                    m_state->beforeSelection,
                    m_state->beforeLocalDirty);
                return NLS::Editor::Undo::UndoStartResult::Completed();
            }

            [[nodiscard]] bool HasChanges() const override
            {
                return m_state && m_state->beforeDocument != m_state->afterDocument;
            }

            [[nodiscard]] NLS::Editor::Undo::UndoCommandPtr BuildCommand() override
            {
                if (!HasChanges())
                    return {};
                return std::make_unique<UIDocumentSnapshotUndoCommand>(
                    m_state->target,
                    m_info,
                    m_state->beforeDocument,
                    m_state->beforeSelection,
                    m_state->beforeLocalDirty,
                    m_state->afterDocument,
                    m_state->afterSelection,
                    m_state->afterLocalDirty);
            }

        private:
            std::shared_ptr<EditState> m_state;
            NLS::Editor::Undo::UndoCommandInfo m_info;
        };

        std::string DefaultNodeName(const std::string_view type)
        {
            if (type == "Panel") return "Panel";
            if (type == "Image") return "Image";
            if (type == "NineSlice") return "Nine Slice";
            if (type == "Text") return "Text";
            if (type == "RichText") return "Rich Text";
            if (type == "Button") return "Button";
            if (type == "Toggle") return "Toggle";
            if (type == "Radio") return "Radio";
            if (type == "Slider") return "Slider";
            if (type == "ProgressBar") return "Progress Bar";
            if (type == "TextField") return "Text Field";
            if (type == "Dropdown") return "Dropdown";
            if (type == "ScrollView") return "Scroll View";
            if (type == "Scrollbar") return "Scrollbar";
            if (type == "ListView") return "List View";
            return "Element";
        }

        nlohmann::json Color(float red, float green, float blue, float alpha)
        {
            return nlohmann::json::array({ red, green, blue, alpha });
        }
    }

    UIDocumentDesignerModel::UIDocumentDesignerModel()
        : m_state(std::make_shared<State>())
    {
    }

    UIDocumentDesignerModel::~UIDocumentDesignerModel()
    {
        CancelActiveTransaction();
        const auto scope = GetUndoScope();
        if (scope.IsValid())
        {
            if (auto* undo = TryGetUndoService())
                undo->InvalidateScope(scope, NLS::Editor::Undo::UndoInvalidationReason::ScopeClosed);
        }
    }

    std::string UIDocumentDesignerModel::CreateDocumentSource(const std::string_view documentName)
    {
        return CreateEmptyDocument(documentName).dump(2) + "\n";
    }

    bool UIDocumentDesignerModel::Load(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            m_state->diagnostics = {{ path.string(), "could not open UI document", true }};
            return false;
        }

        std::ostringstream source;
        source << input.rdbuf();
        try
        {
            auto document = Json::parse(source.str());
            if (!document.is_object())
            {
                m_state->diagnostics = {{ "$", "document root must be an object", true }};
                return false;
            }

            CancelActiveTransaction();
            const auto previousScope = GetUndoScope();
            if (previousScope.IsValid())
            {
                if (auto* undo = TryGetUndoService())
                    undo->InvalidateScope(previousScope, NLS::Editor::Undo::UndoInvalidationReason::Reloaded);
            }

            m_state->path = path;
            m_state->canonicalPath = CanonicalPathKey(path);
            m_state->document = std::move(document);
            ++m_state->generation;
            m_state->requiresSave = EnsureStableIdentifiers(m_state->document);
            m_state->localDirty = m_state->requiresSave;
            m_state->selectedNodeGuid = m_state->document["root"].value("nodeGuid", std::string {});
            ++m_state->documentRevision;
            Revalidate();
            if (auto* undo = TryGetUndoService())
                undo->MarkSaved(GetUndoScope());
            return std::none_of(
                m_state->diagnostics.begin(),
                m_state->diagnostics.end(),
                [](const UIDocumentDesignerDiagnostic& diagnostic) { return diagnostic.error; });
        }
        catch (const std::exception& exception)
        {
            m_state->diagnostics = {{ "$", std::string("invalid JSON: ") + exception.what(), true }};
            return false;
        }
    }

    bool UIDocumentDesignerModel::Save()
    {
        if (!IsLoaded())
            return false;

        // Saving while a text/drag edit is active must first make its already
        // applied preview part of the same global history timeline.
        EndTransaction();
        Revalidate();
        if (std::any_of(
                m_state->diagnostics.begin(),
                m_state->diagnostics.end(),
                [](const UIDocumentDesignerDiagnostic& diagnostic) { return diagnostic.error; }))
            return false;

        if (!WriteTextFileAtomically(m_state->path, m_state->document.dump(2) + "\n"))
        {
            m_state->diagnostics.push_back({ m_state->path.string(), "could not save UI document", true });
            return false;
        }

        m_state->localDirty = false;
        m_state->requiresSave = false;
        if (auto* undo = TryGetUndoService())
            undo->MarkSaved(GetUndoScope());
        return true;
    }

    bool UIDocumentDesignerModel::Reload()
    {
        if (!IsLoaded())
            return false;
        return Load(m_state->path);
    }

    bool UIDocumentDesignerModel::IsDirty() const
    {
        const auto scope = GetUndoScope();
        if (scope.IsValid())
        {
            if (auto* undo = TryGetUndoService())
                return m_state->requiresSave || undo->GetScopeState(scope).IsDirty();
        }
        return m_state->localDirty || m_state->requiresSave;
    }

    NLS::Editor::Undo::UndoScopeId UIDocumentDesignerModel::GetUndoScope() const
    {
        return IsLoaded() ? MakeScope(*m_state) : NLS::Editor::Undo::UndoScopeId {};
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNode(
        const std::string_view nodeGuid) const
    {
        const auto root = m_state->document.find("root");
        return root == m_state->document.end() ? nullptr : FindNodeRecursive(*root, nodeGuid);
    }

    UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNode(const std::string_view nodeGuid)
    {
        const auto root = m_state->document.find("root");
        return root == m_state->document.end() ? nullptr : FindNodeRecursive(*root, nodeGuid);
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::GetSelectedNode() const
    {
        return FindNode(m_state->selectedNodeGuid);
    }

    void UIDocumentDesignerModel::SelectNode(std::string nodeGuid)
    {
        if (FindNode(nodeGuid) != nullptr)
            m_state->selectedNodeGuid = std::move(nodeGuid);
    }

    bool UIDocumentDesignerModel::AddChild(const std::string_view parentNodeGuid, const std::string_view type)
    {
        if (!IsSupportedElementType(type))
            return false;
        auto* parent = FindNode(parentNodeGuid);
        if (parent == nullptr)
            return false;

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        auto node = CreateNode(type, DefaultNodeName(type));
        const auto nodeGuid = node.value("nodeGuid", std::string {});
        (*parent)["children"].push_back(std::move(node));
        m_state->selectedNodeGuid = nodeGuid;
        return FinishChange(before, mergeStartedAt, true, "Add UI Element", nodeGuid);
    }

    bool UIDocumentDesignerModel::RemoveNode(const std::string_view nodeGuid)
    {
        const auto root = m_state->document.find("root");
        if (root == m_state->document.end() || root->value("nodeGuid", std::string {}) == nodeGuid)
            return false;

        auto* parent = FindParentRecursive(*root, nodeGuid);
        if (parent == nullptr || !parent->contains("children") || !(*parent)["children"].is_array())
            return false;

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        auto& children = (*parent)["children"];
        children.erase(
            std::remove_if(
                children.begin(),
                children.end(),
                [nodeGuid](const Json& child)
                {
                    return child.value("nodeGuid", std::string {}) == nodeGuid;
                }),
            children.end());
        m_state->selectedNodeGuid = parent->value("nodeGuid", std::string {});
        return FinishChange(before, mergeStartedAt, true, "Delete UI Element", std::string(nodeGuid));
    }

    bool UIDocumentDesignerModel::SetNodeName(
        const std::string_view nodeGuid,
        std::string value,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr || node->value("name", std::string {}) == value)
            return false;
        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["name"] = std::move(value);
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Rename UI Element",
            std::string(nodeGuid),
            "UIDocument.Name:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeText(
        const std::string_view nodeGuid,
        std::string value,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr || node->value("text", std::string {}) == value)
            return false;
        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["text"] = std::move(value);
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Edit UI Text",
            std::string(nodeGuid),
            "UIDocument.Text:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeLayoutSize(
        const std::string_view nodeGuid,
        const float width,
        const float height,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;

        const auto value = Json::array({ std::max(0.0f, width), std::max(0.0f, height) });
        if ((*node)["layout"].value("size", Json::array()) == value)
            return false;
        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["layout"]["size"] = value;
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Edit UI Size",
            std::string(nodeGuid),
            "UIDocument.Size:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeLayoutMode(
        const std::string_view nodeGuid,
        const std::string_view mode,
        const bool recordUndo)
    {
        if (!IsSupportedLayoutMode(mode))
            return false;
        auto* node = FindNode(nodeGuid);
        if (node == nullptr || (*node)["layout"].value("mode", std::string("Absolute")) == mode)
            return false;
        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["layout"]["mode"] = std::string(mode);
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Edit UI Layout Mode",
            std::string(nodeGuid),
            "UIDocument.LayoutMode:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeSpacing(
        const std::string_view nodeGuid,
        const float spacing,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;
        const auto value = std::max(0.0f, spacing);
        if (std::abs((*node)["layout"].value("spacing", 0.0f) - value) <= 0.0001f)
            return false;
        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["layout"]["spacing"] = value;
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Edit UI Spacing",
            std::string(nodeGuid),
            "UIDocument.Spacing:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeBackgroundColor(
        const std::string_view nodeGuid,
        const float red,
        const float green,
        const float blue,
        const float alpha,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;
        const auto value = Color(
            std::clamp(red, 0.0f, 1.0f),
            std::clamp(green, 0.0f, 1.0f),
            std::clamp(blue, 0.0f, 1.0f),
            std::clamp(alpha, 0.0f, 1.0f));
        if ((*node)["style"].value("backgroundColor", Json::array()) == value)
            return false;
        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["style"]["backgroundColor"] = value;
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Edit UI Background",
            std::string(nodeGuid),
            "UIDocument.Background:" + std::string(nodeGuid),
            true);
    }

    void UIDocumentDesignerModel::BeginTransaction(std::string label)
    {
        if (m_activeTransaction.has_value() || !IsLoaded())
            return;

        auto* undo = TryGetUndoService();
        if (undo == nullptr)
            return;

        const auto before = CaptureSnapshot();
        auto edit = std::make_shared<EditState>();
        edit->target = m_state;
        edit->scope = GetUndoScope();
        edit->beforeDocument = before.document;
        edit->beforeSelection = before.selectedNodeGuid;
        edit->beforeLocalDirty = before.localDirty;
        edit->afterDocument = before.document;
        edit->afterSelection = before.selectedNodeGuid;
        edit->afterLocalDirty = before.localDirty;

        ActiveTransaction active { edit, std::nullopt };
        NLS::Editor::Undo::UndoCommandInfo info;
        info.label = std::move(label);
        info.sourceModule = "UIDocumentDesigner";
        info.scopes = { edit->scope };
        if (!before.selectedNodeGuid.empty())
            info.affected = { before.selectedNodeGuid };

        NLS::Editor::Undo::UndoEditDesc description;
        description.info = info;
        description.edit = std::make_unique<UIDocumentContinuousEdit>(edit, std::move(info));
        auto begun = undo->BeginEditSession(std::move(description));
        if (begun.result.Succeeded() && begun.session.IsValid())
            active.session.emplace(std::move(begun.session));
        m_activeTransaction.emplace(std::move(active));
    }

    void UIDocumentDesignerModel::EndTransaction()
    {
        if (!m_activeTransaction.has_value())
            return;

        if (m_activeTransaction->edit &&
            m_state->document == m_activeTransaction->edit->beforeDocument)
        {
            // Even a no-op preview can have changed the selected node. Let the
            // session's cancel path restore the complete before snapshot.
            CancelActiveTransaction();
            return;
        }

        auto active = std::move(*m_activeTransaction);
        m_activeTransaction.reset();
        if (!active.edit)
            return;

        if (!active.session.has_value())
        {
            RestoreState(
                *m_state,
                active.edit->beforeDocument,
                active.edit->beforeSelection,
                active.edit->beforeLocalDirty);
            return;
        }

        if (auto* undo = TryGetUndoService())
        {
            const auto committed = undo->CommitEditSession(*active.session);
            if (committed.Succeeded())
            {
                m_state->localDirty = false;
                return;
            }

            // A failed commit must not leave the already-applied preview in
            // the document. Cancel can still succeed while history is frozen.
            const bool cancelled = active.session->IsValid() &&
                undo->CancelEditSession(*active.session).Succeeded();
            if (!cancelled)
            {
                RestoreState(
                    *m_state,
                    active.edit->beforeDocument,
                    active.edit->beforeSelection,
                    active.edit->beforeLocalDirty);
            }
        }
        else
        {
            RestoreState(
                *m_state,
                active.edit->beforeDocument,
                active.edit->beforeSelection,
                active.edit->beforeLocalDirty);
        }
    }

    bool UIDocumentDesignerModel::BuildRuntimeDocument(
        NLS::Engine::UI::UIDocument& document,
        std::vector<NLS::Engine::UI::UIDocumentDiagnostic>* diagnostics) const
    {
        auto parsed = NLS::Engine::UI::UIDocument::LoadFromJson(m_state->document.dump(), diagnostics);
        if (!parsed.has_value())
            return false;
        document = std::move(*parsed);
        return true;
    }

    UIDocumentDesignerModel::Json UIDocumentDesignerModel::CreateNode(
        const std::string_view type,
        const std::string_view name)
    {
        Json node {
            { "nodeGuid", NLS::Guid::New().ToString() },
            { "type", std::string(type) },
            { "name", std::string(name) },
            { "layout", {
                { "mode", "Absolute" },
                { "size", Json::array({ 160.0f, 32.0f }) }
            } }
        };
        if (type == "Text" || type == "RichText" || type == "Button" || type == "TextField")
            node["text"] = DefaultNodeName(type);
        if (type == "Panel")
            node["style"] = {{ "backgroundColor", Color(0.12f, 0.13f, 0.16f, 1.0f) }};
        return node;
    }

    UIDocumentDesignerModel::Json UIDocumentDesignerModel::CreateEmptyDocument(
        const std::string_view documentName)
    {
        auto root = CreateNode("Panel", documentName.empty() ? "Root" : documentName);
        root["layout"] = {
            { "mode", "Overlay" },
            { "size", Json::array({ 1920.0f, 1080.0f }) }
        };
        root["style"] = {{ "backgroundColor", Color(0.0f, 0.0f, 0.0f, 0.0f) }};

        return {
            { "version", 1u },
            { "documentId", NLS::Guid::New().ToString() },
            { "root", std::move(root) }
        };
    }

    UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNodeRecursive(
        Json& node,
        const std::string_view nodeGuid)
    {
        if (node.value("nodeGuid", std::string {}) == nodeGuid)
            return &node;
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return nullptr;
        for (auto& child : *children)
        {
            if (auto* found = FindNodeRecursive(child, nodeGuid))
                return found;
        }
        return nullptr;
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNodeRecursive(
        const Json& node,
        const std::string_view nodeGuid)
    {
        if (node.value("nodeGuid", std::string {}) == nodeGuid)
            return &node;
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return nullptr;
        for (const auto& child : *children)
        {
            if (const auto* found = FindNodeRecursive(child, nodeGuid))
                return found;
        }
        return nullptr;
    }

    UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindParentRecursive(
        Json& node,
        const std::string_view childNodeGuid)
    {
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return nullptr;
        for (auto& child : *children)
        {
            if (child.value("nodeGuid", std::string {}) == childNodeGuid)
                return &node;
            if (auto* found = FindParentRecursive(child, childNodeGuid))
                return found;
        }
        return nullptr;
    }

    bool UIDocumentDesignerModel::EnsureStableIdentifiers(Json& document)
    {
        bool changed = false;
        if (!document.contains("version"))
        {
            document["version"] = 1u;
            changed = true;
        }
        if (!document.contains("documentId") || !document["documentId"].is_string() ||
            !NLS::Guid::TryParse(document.value("documentId", std::string {})).has_value())
        {
            document["documentId"] = NLS::Guid::New().ToString();
            changed = true;
        }
        if (!document.contains("root") || !document["root"].is_object())
        {
            document["root"] = CreateNode("Panel", "Root");
            changed = true;
        }

        const auto ensureNode = [&changed](auto&& self, Json& node) -> void
        {
            if (!node.contains("nodeGuid") || !node["nodeGuid"].is_string() ||
                !NLS::Guid::TryParse(node.value("nodeGuid", std::string {})).has_value())
            {
                node["nodeGuid"] = NLS::Guid::New().ToString();
                changed = true;
            }
            if (!node.contains("type") || !node["type"].is_string())
            {
                node["type"] = "Panel";
                changed = true;
            }
            if (node.contains("children") && node["children"].is_array())
            {
                for (auto& child : node["children"])
                    self(self, child);
            }
        };
        ensureNode(ensureNode, document["root"]);
        return changed;
    }

    bool UIDocumentDesignerModel::IsSupportedElementType(const std::string_view type)
    {
        return type == "Panel" || type == "Image" || type == "NineSlice" || type == "Text" ||
            type == "RichText" || type == "Button" || type == "Toggle" || type == "Radio" ||
            type == "Slider" || type == "ProgressBar" || type == "TextField" ||
            type == "Dropdown" || type == "ScrollView" || type == "Scrollbar" || type == "ListView";
    }

    bool UIDocumentDesignerModel::IsSupportedLayoutMode(const std::string_view mode)
    {
        return mode == "Absolute" || mode == "Stack" || mode == "Flex" || mode == "Grid" ||
            mode == "Overlay" || mode == "Scroll";
    }

    bool UIDocumentDesignerModel::WriteTextFileAtomically(
        const std::filesystem::path& path,
        const std::string& text)
    {
        std::error_code error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
                return false;
        }

        const auto temporaryPath = path.string() + ".tmp";
        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output)
                return false;
            output << text;
            if (!output.good())
                return false;
        }

        std::filesystem::rename(temporaryPath, path, error);
        if (!error)
            return true;
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporaryPath, path, error);
        if (!error)
            return true;
        std::filesystem::remove(temporaryPath, error);
        return false;
    }

    UIDocumentDesignerModel::DocumentSnapshot UIDocumentDesignerModel::CaptureSnapshot() const
    {
        return {
            m_state->document,
            m_state->selectedNodeGuid,
            m_state->localDirty,
            m_state->documentRevision
        };
    }

    bool UIDocumentDesignerModel::FinishChange(
        DocumentSnapshot before,
        const std::chrono::steady_clock::time_point mergeStartedAt,
        const bool recordUndo,
        std::string label,
        std::string affected,
        std::string mergeKey,
        const bool allowMerge)
    {
        m_state->localDirty = true;
        ++m_state->documentRevision;
        Revalidate();
        if (m_activeTransaction.has_value())
            return UpdateActiveEdit();
        if (!recordUndo)
        {
            // A recordUndo=false call is valid only inside an active edit
            // session. Keeping it outside one would bypass global history.
            RestoreSnapshot(before);
            return false;
        }

        if (TryGetUndoService() == nullptr)
        {
            RestoreSnapshot(before);
            return false;
        }

        if (CommitSnapshotChange(
                before,
                CaptureSnapshot(),
                std::move(label),
                std::move(affected),
                std::move(mergeKey),
                allowMerge,
                mergeStartedAt))
        {
            return true;
        }

        RestoreSnapshot(before);
        return false;
    }

    bool UIDocumentDesignerModel::CommitSnapshotChange(
        DocumentSnapshot before,
        DocumentSnapshot after,
        std::string label,
        std::string affected,
        std::string mergeKey,
        const bool allowMerge,
        const std::chrono::steady_clock::time_point mergeStartedAt)
    {
        const auto scope = GetUndoScope();
        auto* undo = TryGetUndoService();
        if (!scope.IsValid() || undo == nullptr || before.document == after.document)
            return false;

        NLS::Editor::Undo::UndoCommandInfo info;
        info.label = std::move(label);
        info.sourceModule = "UIDocumentDesigner";
        info.scopes = { scope };
        if (!affected.empty())
            info.affected = { std::move(affected) };
        info.mergeKey = std::move(mergeKey);
        info.allowMerge = allowMerge;
        info.estimatedBytes = before.document.dump().size() + after.document.dump().size();

        auto edit = std::make_shared<EditState>();
        edit->target = m_state;
        edit->scope = scope;
        edit->beforeDocument = before.document;
        edit->beforeSelection = before.selectedNodeGuid;
        edit->beforeLocalDirty = before.localDirty;
        edit->afterDocument = after.document;
        edit->afterSelection = after.selectedNodeGuid;
        edit->afterLocalDirty = after.localDirty;

        // This helper is reached after an immediate command updated the model
        // optimistically.  Stage it back to before, then let Preview apply the
        // durable after snapshot under the global Undo mutation context.
        RestoreSnapshot(before);
        auto commandInfo = info;
        NLS::Editor::Undo::UndoEditDesc description;
        description.info = std::move(info);
        description.edit = std::make_unique<UIDocumentContinuousEdit>(std::move(edit), std::move(commandInfo));
        description.mergeStartedAt = mergeStartedAt;
        auto begun = undo->BeginEditSession(std::move(description));
        if (!begun.result.Succeeded() || !begun.session.IsValid())
            return false;

        auto session = begun.session;
        if (!undo->PreviewEditSession(session).Succeeded())
        {
            if (session.IsValid())
                (void)undo->CancelEditSession(session);
            return false;
        }
        if (!undo->CommitEditSession(session).Succeeded())
        {
            if (session.IsValid())
                (void)undo->CancelEditSession(session);
            return false;
        }

        // The service owns dirty tracking for this scope after accepting the command.
        m_state->localDirty = false;
        return true;
    }

    bool UIDocumentDesignerModel::UpdateActiveEdit()
    {
        if (!m_activeTransaction.has_value() || !m_activeTransaction->edit)
            return false;

        auto& edit = *m_activeTransaction->edit;
        const auto after = CaptureSnapshot();
        edit.afterDocument = after.document;
        edit.afterSelection = after.selectedNodeGuid;
        edit.afterLocalDirty = after.localDirty;

        if (!m_activeTransaction->session.has_value())
        {
            CancelActiveTransaction();
            return false;
        }
        auto* undo = TryGetUndoService();
        if (undo == nullptr)
        {
            CancelActiveTransaction();
            return false;
        }

        const auto previewed = undo->PreviewEditSession(*m_activeTransaction->session);
        if (!previewed.Succeeded())
        {
            CancelActiveTransaction();
            return false;
        }
        return true;
    }

    void UIDocumentDesignerModel::CancelActiveTransaction()
    {
        if (!m_activeTransaction.has_value())
            return;

        auto active = std::move(*m_activeTransaction);
        m_activeTransaction.reset();
        if (!active.edit)
            return;

        bool cancelledByService = false;
        if (active.session.has_value())
        {
            if (auto* undo = TryGetUndoService())
                cancelledByService = undo->CancelEditSession(*active.session).Succeeded();
        }
        if (cancelledByService)
            return;

        if (const auto target = active.edit->target.lock())
        {
            if (MakeScope(*target) == active.edit->scope)
            {
                RestoreState(
                    *target,
                    active.edit->beforeDocument,
                    active.edit->beforeSelection,
                    active.edit->beforeLocalDirty);
            }
        }
    }

    void UIDocumentDesignerModel::RestoreSnapshot(const DocumentSnapshot& snapshot)
    {
        RestoreState(
            *m_state,
            snapshot.document,
            snapshot.selectedNodeGuid,
            snapshot.localDirty);
        m_state->documentRevision = snapshot.documentRevision;
    }

    void UIDocumentDesignerModel::Revalidate()
    {
        RevalidateState(*m_state);
    }
}
