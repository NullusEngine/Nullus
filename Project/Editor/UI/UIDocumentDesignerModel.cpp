#include "UI/UIDocumentDesignerModel.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
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

        // Keeps the primary selection (first entry) and the selection list in
        // sync, dropping entries whose node disappeared from the document.
        void FilterSelection(State& state, std::vector<std::string> requested)
        {
            const auto root = state.document.find("root");
            const std::string rootGuid = root == state.document.end()
                ? std::string {}
                : root->value("nodeGuid", std::string {});

            std::vector<std::string> filtered;
            filtered.reserve(requested.size());
            for (auto& guid : requested)
            {
                if (guid.empty() || std::find(filtered.begin(), filtered.end(), guid) != filtered.end())
                    continue;
                if (ContainsNode(*root, guid))
                    filtered.push_back(std::move(guid));
            }
            if (filtered.empty() && !rootGuid.empty())
                filtered.push_back(rootGuid);
            state.selectedNodeGuids = std::move(filtered);
            state.selectedNodeGuid = state.selectedNodeGuids.empty()
                ? std::string {}
                : state.selectedNodeGuids.front();
        }

        void RestoreState(
            State& state,
            nlohmann::json document,
            std::vector<std::string> selection,
            const bool localDirty)
        {
            state.document = std::move(document);
            FilterSelection(state, std::move(selection));
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
                std::vector<std::string> beforeSelection,
                const bool beforeLocalDirty,
                nlohmann::json afterDocument,
                std::vector<std::string> afterSelection,
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
                    m_beforeSelection.size() * sizeof(std::string) +
                    m_afterSelection.size() * sizeof(std::string);
                m_info.estimatedBlobBytes =
                    m_beforeDocument.GetBlobBytes() + m_afterDocument.GetBlobBytes();
            }

            std::weak_ptr<State> m_state;
            NLS::Editor::Undo::UndoCommandInfo m_info;
            UIDocumentSnapshotStorage m_beforeDocument;
            std::vector<std::string> m_beforeSelection;
            bool m_beforeLocalDirty = false;
            UIDocumentSnapshotStorage m_afterDocument;
            std::vector<std::string> m_afterSelection;
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

        // ------------------------------------------------------------------
        // Property path schema
        // ------------------------------------------------------------------
        enum class PropertyKind : uint8_t
        {
            String,
            Bool,
            Vector2,
            NonNegativeFloat,
            UnitFloat,
            UInt,
            UInt64,
            PositiveUInt,
            Color,
            LayoutMode,
            Direction,
            Align
        };

        struct PropertySpec
        {
            std::string_view path;
            PropertyKind kind;
        };

        constexpr PropertySpec kPropertySpecs[] = {
            { "name", PropertyKind::String },
            { "className", PropertyKind::String },
            { "text", PropertyKind::String },
            { "password", PropertyKind::Bool },
            { "requiresRenderTarget", PropertyKind::Bool },
            { "fontAssetId", PropertyKind::UInt64 },
            { "fontPixelSize", PropertyKind::PositiveUInt },
            { "layout.mode", PropertyKind::LayoutMode },
            { "layout.direction", PropertyKind::Direction },
            { "layout.align", PropertyKind::Align },
            { "layout.anchorMin", PropertyKind::Vector2 },
            { "layout.anchorMax", PropertyKind::Vector2 },
            { "layout.pivot", PropertyKind::Vector2 },
            { "layout.offsetMin", PropertyKind::Vector2 },
            { "layout.offsetMax", PropertyKind::Vector2 },
            { "layout.size", PropertyKind::Vector2 },
            { "layout.minSize", PropertyKind::Vector2 },
            { "layout.maxSize", PropertyKind::Vector2 },
            { "layout.margin", PropertyKind::Vector2 },
            { "layout.padding", PropertyKind::Vector2 },
            { "layout.aspectRatio", PropertyKind::NonNegativeFloat },
            { "layout.spacing", PropertyKind::NonNegativeFloat },
            { "layout.flexGrow", PropertyKind::NonNegativeFloat },
            { "layout.gridColumns", PropertyKind::PositiveUInt },
            { "layout.clipChildren", PropertyKind::Bool },
            { "style.backgroundColor", PropertyKind::Color },
            { "style.color", PropertyKind::Color },
            { "style.opacity", PropertyKind::UnitFloat },
            { "style.visible", PropertyKind::Bool },
            { "style.enabled", PropertyKind::Bool },
            { "style.interactive", PropertyKind::Bool },
            { "texture.assetId", PropertyKind::String },
            { "texture.subAssetKey", PropertyKind::String },
            { "texture.path", PropertyKind::String },
            { "font.assetId", PropertyKind::String },
            { "font.subAssetKey", PropertyKind::String },
            { "font.path", PropertyKind::String },
            { "nineSlice.left", PropertyKind::NonNegativeFloat },
            { "nineSlice.top", PropertyKind::NonNegativeFloat },
            { "nineSlice.right", PropertyKind::NonNegativeFloat },
            { "nineSlice.bottom", PropertyKind::NonNegativeFloat },
        };

        const PropertySpec* FindPropertySpec(const std::string_view path)
        {
            const auto iterator = std::find_if(
                std::begin(kPropertySpecs),
                std::end(kPropertySpecs),
                [path](const PropertySpec& spec) { return spec.path == path; });
            return iterator == std::end(kPropertySpecs) ? nullptr : &*iterator;
        }

        bool ReadVector2Value(const nlohmann::json& value, nlohmann::json& normalized)
        {
            if (!value.is_array() || value.size() < 2u ||
                !value[0].is_number() || !value[1].is_number())
            {
                return false;
            }
            normalized = nlohmann::json::array({
                value[0].get<float>(),
                value[1].get<float>() });
            return true;
        }

        bool ReadColorValue(const nlohmann::json& value, nlohmann::json& normalized)
        {
            if (!value.is_array() || value.size() < 3u)
                return false;
            for (size_t index = 0u; index < value.size() && index < 4u; ++index)
            {
                if (!value[index].is_number())
                    return false;
            }
            const auto component = [&value](const size_t index, const float fallback)
            {
                return value.size() > index && value[index].is_number()
                    ? std::clamp(value[index].get<float>(), 0.0f, 1.0f)
                    : fallback;
            };
            normalized = nlohmann::json::array({
                component(0u, 0.0f),
                component(1u, 0.0f),
                component(2u, 0.0f),
                component(3u, 1.0f) });
            return true;
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
            FilterSelection(
                *m_state,
                { m_state->document["root"].value("nodeGuid", std::string {}) });
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
        if (HasErrors())
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

    bool UIDocumentDesignerModel::HasErrors() const
    {
        return std::any_of(
            m_state->diagnostics.begin(),
            m_state->diagnostics.end(),
            [](const UIDocumentDesignerDiagnostic& diagnostic) { return diagnostic.error; });
    }

    bool UIDocumentDesignerModel::HasWarnings() const
    {
        return std::any_of(
            m_state->diagnostics.begin(),
            m_state->diagnostics.end(),
            [](const UIDocumentDesignerDiagnostic& diagnostic) { return !diagnostic.error; });
    }

    NLS::Editor::Undo::UndoScopeId UIDocumentDesignerModel::GetUndoScope() const
    {
        return IsLoaded() ? MakeScope(*m_state) : NLS::Editor::Undo::UndoScopeId {};
    }

    std::string UIDocumentDesignerModel::GetRootGuid() const
    {
        const auto root = m_state->document.find("root");
        return root == m_state->document.end() ? std::string {} : root->value("nodeGuid", std::string {});
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

    bool UIDocumentDesignerModel::IsRootNode(const std::string_view nodeGuid) const
    {
        return !nodeGuid.empty() && GetRootGuid() == nodeGuid;
    }

    std::string UIDocumentDesignerModel::GetNodeParentGuid(const std::string_view nodeGuid) const
    {
        std::string parent;
        size_t index = 0u;
        return TryGetNodeLocation(nodeGuid, parent, index) ? parent : std::string {};
    }

    bool UIDocumentDesignerModel::TryGetNodeLocation(
        const std::string_view nodeGuid,
        std::string& parentNodeGuid,
        size_t& index) const
    {
        const auto root = m_state->document.find("root");
        if (root == m_state->document.end() || nodeGuid.empty())
            return false;
        if (root->value("nodeGuid", std::string {}) == nodeGuid)
        {
            parentNodeGuid.clear();
            index = 0u;
            return false;
        }

        const auto* parent = FindParentRecursive(*root, nodeGuid);
        if (parent == nullptr)
            return false;
        parentNodeGuid = parent->value("nodeGuid", std::string {});
        return TryGetNodeIndexInParent(*parent, nodeGuid, index);
    }

    bool UIDocumentDesignerModel::IsNodeInSubtree(
        const std::string_view nodeGuid,
        const std::string_view candidateDescendant) const
    {
        const auto* node = FindNode(nodeGuid);
        return node != nullptr && !candidateDescendant.empty() &&
            ContainsNode(*node, candidateDescendant);
    }

    size_t UIDocumentDesignerModel::GetChildCount(const std::string_view nodeGuid) const
    {
        const auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return 0u;
        const auto children = node->find("children");
        return children != node->end() && children->is_array() ? children->size() : 0u;
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindNodeByDiagnosticPath(
        const std::string_view diagnosticPath) const
    {
        if (diagnosticPath.empty())
            return nullptr;

        const auto* current = &m_state->document;
        const Json* lastNode = nullptr;
        size_t offset = 0u;
        while (offset < diagnosticPath.size())
        {
            // Skip separators.
            while (offset < diagnosticPath.size() && diagnosticPath[offset] == '.')
            {
                if (offset > 0u && diagnosticPath[offset - 1u] == '.')
                    return lastNode;  // ".." is not a valid document path
                ++offset;
            }
            if (offset >= diagnosticPath.size())
                break;

            const auto segmentBegin = offset;
            while (offset < diagnosticPath.size() &&
                   diagnosticPath[offset] != '.' &&
                   diagnosticPath[offset] != '[')
            {
                ++offset;
            }
            const auto key = diagnosticPath.substr(segmentBegin, offset - segmentBegin);
            if (key.empty() || !current->is_object())
                return lastNode;
            if (key == "$")
                continue;  // document root marker
            const auto member = current->find(std::string(key));
            if (member == current->end())
                return lastNode;
            current = &*member;
            if (current->is_object() && current->contains("nodeGuid"))
                lastNode = current;

            while (offset < diagnosticPath.size() && diagnosticPath[offset] == '[')
            {
                const auto close = diagnosticPath.find(']', offset);
                if (close == std::string_view::npos)
                    return lastNode;
                const auto indexText = diagnosticPath.substr(offset + 1u, close - offset - 1u);
                if (indexText.empty() ||
                    !std::all_of(indexText.begin(), indexText.end(), [](const char character)
                    {
                        return character >= '0' && character <= '9';
                    }))
                {
                    return lastNode;
                }
                const auto index = static_cast<size_t>(std::stoull(std::string(indexText)));
                if (!current->is_array() || index >= current->size())
                    return lastNode;
                current = &(*current)[index];
                if (current->is_object() && current->contains("nodeGuid"))
                    lastNode = current;
                offset = close + 1u;
            }
        }
        return lastNode;
    }

    std::string UIDocumentDesignerModel::BuildDiagnosticPath(const std::string_view nodeGuid) const
    {
        std::string path;
        const auto root = m_state->document.find("root");
        if (root == m_state->document.end())
            return path;

        const std::function<bool(const Json&, const std::string&)> capture =
            [&capture, &path, nodeGuid](const Json& node, const std::string& current) -> bool
            {
                if (node.value("nodeGuid", std::string {}) == nodeGuid)
                {
                    path = current;
                    return true;
                }
                const auto children = node.find("children");
                if (children == node.end() || !children->is_array())
                    return false;
                for (size_t index = 0u; index < children->size(); ++index)
                {
                    if (capture(
                            (*children)[index],
                            current + ".children[" + std::to_string(index) + "]"))
                    {
                        return true;
                    }
                }
                return false;
            };
        (void)capture(*root, "$.root");
        return path;
    }

    void UIDocumentDesignerModel::SelectNode(std::string nodeGuid)
    {
        SetSelection({ std::move(nodeGuid) });
    }

    void UIDocumentDesignerModel::AddToSelection(std::string nodeGuid)
    {
        if (FindNode(nodeGuid) == nullptr)
            return;
        auto& selection = m_state->selectedNodeGuids;
        if (std::find(selection.begin(), selection.end(), nodeGuid) != selection.end())
        {
            SyncPrimarySelection(*m_state);
            return;
        }
        selection.push_back(std::move(nodeGuid));
        SyncPrimarySelection(*m_state);
    }

    void UIDocumentDesignerModel::ToggleSelection(std::string nodeGuid)
    {
        if (FindNode(nodeGuid) == nullptr)
            return;
        auto& selection = m_state->selectedNodeGuids;
        const auto iterator = std::find(selection.begin(), selection.end(), nodeGuid);
        if (iterator != selection.end())
            selection.erase(iterator);
        else
            selection.push_back(std::move(nodeGuid));
        if (selection.empty())
            selection.push_back(GetRootGuid());
        SyncPrimarySelection(*m_state);
    }

    void UIDocumentDesignerModel::SetSelection(std::vector<std::string> nodeGuids)
    {
        FilterSelection(*m_state, std::move(nodeGuids));
    }

    void UIDocumentDesignerModel::ClearSelection()
    {
        SetSelection({});
    }

    bool UIDocumentDesignerModel::IsSupportedPropertyPath(const std::string_view path)
    {
        return FindPropertySpec(path) != nullptr;
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::GetNodeProperty(
        const std::string_view nodeGuid,
        const std::string_view path) const
    {
        const auto* node = FindNode(nodeGuid);
        return node == nullptr ? nullptr : ReadNodeProperty(*node, path);
    }

    bool UIDocumentDesignerModel::SetNodeProperty(
        const std::string_view nodeGuid,
        const std::string_view path,
        Json value,
        const bool recordUndo,
        std::string label,
        std::string mergeKey,
        const bool allowMerge)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr || !node->is_object())
            return false;

        Json normalized;
        if (!NormalizePropertyValue(path, value, normalized))
            return false;

        const auto* current = ReadNodeProperty(*node, path);
        if (current != nullptr && *current == normalized)
            return false;

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        if (!WriteNodeProperty(*node, path, normalized))
            return false;

        if (label.empty())
            label = DefaultPropertyLabel(path);
        if (mergeKey.empty() && allowMerge)
            mergeKey = "UIDocument.Property:" + std::string(path) + ":" + std::string(nodeGuid);
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            std::move(label),
            std::string(nodeGuid),
            std::move(mergeKey),
            allowMerge);
    }

    bool UIDocumentDesignerModel::SetNodePropertyOnSelection(
        const std::string_view path,
        Json value,
        const bool recordUndo,
        std::string label,
        std::string mergeKey,
        const bool allowMerge)
    {
        if (!IsLoaded())
            return false;
        if (m_state->selectedNodeGuids.size() <= 1u)
        {
            return SetNodeProperty(
                m_state->selectedNodeGuid,
                path,
                std::move(value),
                recordUndo,
                std::move(label),
                std::move(mergeKey),
                allowMerge);
        }

        const bool ownTransaction = recordUndo && !HasActiveTransaction();
        if (ownTransaction)
            BeginTransaction(label.empty() ? "Edit UI Elements" : label, std::move(mergeKey));

        // UpdateActiveEdit()/RestoreState() replaces the selection vector during
        // each preview, so iterate over a stable copy.
        const auto selection = m_state->selectedNodeGuids;
        bool changed = false;
        for (const auto& nodeGuid : selection)
        {
            if (SetNodeProperty(nodeGuid, path, value, false, {}, {}, false))
                changed = true;
        }

        if (ownTransaction)
            EndTransaction();
        return changed;
    }

    bool UIDocumentDesignerModel::SetNodeVector2(
        const std::string_view nodeGuid,
        const std::string_view path,
        const float x,
        const float y,
        const bool recordUndo)
    {
        return SetNodeProperty(nodeGuid, path, Json::array({ x, y }), recordUndo);
    }

    bool UIDocumentDesignerModel::SetNodeFloat(
        const std::string_view nodeGuid,
        const std::string_view path,
        const float value,
        const bool recordUndo)
    {
        return SetNodeProperty(nodeGuid, path, value, recordUndo);
    }

    bool UIDocumentDesignerModel::SetNodeUInt(
        const std::string_view nodeGuid,
        const std::string_view path,
        const uint32_t value,
        const bool recordUndo)
    {
        return SetNodeProperty(nodeGuid, path, value, recordUndo);
    }

    bool UIDocumentDesignerModel::SetNodeBool(
        const std::string_view nodeGuid,
        const std::string_view path,
        const bool value,
        const bool recordUndo)
    {
        return SetNodeProperty(nodeGuid, path, value, recordUndo);
    }

    bool UIDocumentDesignerModel::SetNodeColor(
        const std::string_view nodeGuid,
        const std::string_view path,
        const float red,
        const float green,
        const float blue,
        const float alpha,
        const bool recordUndo)
    {
        return SetNodeProperty(
            nodeGuid,
            path,
            Color(red, green, blue, alpha),
            recordUndo);
    }

    bool UIDocumentDesignerModel::SetNodeString(
        const std::string_view nodeGuid,
        const std::string_view path,
        std::string value,
        const bool recordUndo)
    {
        return SetNodeProperty(nodeGuid, path, std::move(value), recordUndo);
    }

    bool UIDocumentDesignerModel::SetNodeFont(
        const std::string_view nodeGuid,
        const uint64_t fontAssetId,
        const uint16_t pixelSize,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;
        const auto normalizedSize = static_cast<uint32_t>(pixelSize == 0u ? 1u : pixelSize);
        const auto currentAsset = node->value("fontAssetId", 0ull);
        const auto currentSize = node->value("fontPixelSize", 16u);
        if (currentAsset == fontAssetId && currentSize == normalizedSize)
            return false;

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["fontAssetId"] = fontAssetId;
        (*node)["fontPixelSize"] = normalizedSize;
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Edit UI Font",
            std::string(nodeGuid),
            "UIDocument.Font:" + std::string(nodeGuid),
            true);
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
        SetSelection({ nodeGuid });
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
        const std::string parentGuid = parent->value("nodeGuid", std::string {});
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
        // Keep the surviving selection when a batch delete removed one node,
        // otherwise fall back to the parent.
        std::vector<std::string> surviving;
        for (const auto& selected : m_state->selectedNodeGuids)
        {
            if (selected != nodeGuid && FindNode(selected) != nullptr)
                surviving.push_back(selected);
        }
        if (surviving.empty())
            surviving.push_back(parentGuid);
        SetSelection(std::move(surviving));
        return FinishChange(before, mergeStartedAt, true, "Delete UI Element", std::string(nodeGuid));
    }

    bool UIDocumentDesignerModel::SetNodeName(
        const std::string_view nodeGuid,
        std::string value,
        const bool recordUndo)
    {
        return SetNodeProperty(
            nodeGuid,
            "name",
            std::move(value),
            recordUndo,
            "Rename UI Element",
            "UIDocument.Name:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeText(
        const std::string_view nodeGuid,
        std::string value,
        const bool recordUndo)
    {
        return SetNodeProperty(
            nodeGuid,
            "text",
            std::move(value),
            recordUndo,
            "Edit UI Text",
            "UIDocument.Text:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeLayoutSize(
        const std::string_view nodeGuid,
        const float width,
        const float height,
        const bool recordUndo)
    {
        return SetNodeProperty(
            nodeGuid,
            "layout.size",
            Json::array({ std::max(0.0f, width), std::max(0.0f, height) }),
            recordUndo,
            "Edit UI Size",
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
        return SetNodeProperty(
            nodeGuid,
            "layout.mode",
            std::string(mode),
            recordUndo,
            "Edit UI Layout Mode",
            "UIDocument.LayoutMode:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeSpacing(
        const std::string_view nodeGuid,
        const float spacing,
        const bool recordUndo)
    {
        return SetNodeProperty(
            nodeGuid,
            "layout.spacing",
            std::max(0.0f, spacing),
            recordUndo,
            "Edit UI Spacing",
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
        return SetNodeProperty(
            nodeGuid,
            "style.backgroundColor",
            Color(red, green, blue, alpha),
            recordUndo,
            "Edit UI Background",
            "UIDocument.Background:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::AddChildAt(
        const std::string_view parentNodeGuid,
        const std::string_view type,
        const float localX,
        const float localY)
    {
        if (!IsSupportedElementType(type))
            return false;
        auto* parent = FindNode(parentNodeGuid);
        if (parent == nullptr)
            return false;

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        auto node = CreateNode(type, DefaultNodeName(type));
        node["layout"]["offsetMin"] = Json::array({ localX, localY });
        const auto nodeGuid = node.value("nodeGuid", std::string {});
        (*parent)["children"].push_back(std::move(node));
        SetSelection({ nodeGuid });
        return FinishChange(before, mergeStartedAt, true, "Add UI Element", nodeGuid);
    }

    bool UIDocumentDesignerModel::DuplicateNode(
        const std::string_view nodeGuid,
        const bool selectCopy)
    {
        if (!IsLoaded() || IsRootNode(nodeGuid))
            return false;
        std::string parentGuid;
        size_t index = 0u;
        if (!TryGetNodeLocation(nodeGuid, parentGuid, index))
            return false;
        auto* parent = FindNode(parentGuid);
        const auto* source = FindNode(nodeGuid);
        if (parent == nullptr || source == nullptr)
            return false;

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();

        Json copy = *source;
        AssignFreshIdentifiers(copy);
        const auto copyGuid = copy.value("nodeGuid", std::string {});

        auto& children = (*parent)["children"];
        const auto insertPosition = children.begin() + static_cast<std::ptrdiff_t>(
            std::min(index + 1u, children.size()));
        children.insert(insertPosition, std::move(copy));
        if (selectCopy)
            SetSelection({ copyGuid });
        return FinishChange(before, mergeStartedAt, true, "Duplicate UI Element", copyGuid);
    }

    bool UIDocumentDesignerModel::MoveNode(
        const std::string_view nodeGuid,
        const std::string_view parentNodeGuid,
        const size_t index)
    {
        if (!IsLoaded() || IsRootNode(nodeGuid) || nodeGuid.empty())
            return false;
        auto* newParent = FindNode(parentNodeGuid);
        if (newParent == nullptr)
            return false;
        // Moving a node inside its own subtree would detach the tree.
        if (IsNodeInSubtree(nodeGuid, parentNodeGuid))
            return false;

        std::string oldParentGuid;
        size_t oldIndex = 0u;
        if (!TryGetNodeLocation(nodeGuid, oldParentGuid, oldIndex))
            return false;

        auto* oldParent = FindNode(oldParentGuid);
        const auto* source = FindNode(nodeGuid);
        if (oldParent == nullptr || source == nullptr)
            return false;

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();

        Json extracted = *source;
        auto& oldChildren = (*oldParent)["children"];
        oldChildren.erase(
            std::remove_if(
                oldChildren.begin(),
                oldChildren.end(),
                [nodeGuid](const Json& child)
                {
                    return child.value("nodeGuid", std::string {}) == nodeGuid;
                }),
            oldChildren.end());

        // Re-resolve the parent: erasing may have invalidated references if the
        // parent lives in the same children array.
        newParent = FindNode(parentNodeGuid);
        if (newParent == nullptr)
        {
            RestoreSnapshot(before);
            return false;
        }

        size_t targetIndex = index;
        if (oldParentGuid == parentNodeGuid && oldIndex < targetIndex)
            --targetIndex;
        auto& newChildren = (*newParent)["children"];
        if (!newChildren.is_array())
            newChildren = Json::array();
        targetIndex = std::min(targetIndex, newChildren.size());
        newChildren.insert(
            newChildren.begin() + static_cast<std::ptrdiff_t>(targetIndex),
            std::move(extracted));

        SetSelection({ std::string(nodeGuid) });
        return FinishChange(before, mergeStartedAt, true, "Move UI Element", std::string(nodeGuid));
    }

    bool UIDocumentDesignerModel::NudgeNode(
        const std::string_view nodeGuid,
        const float deltaX,
        const float deltaY)
    {
        if (std::abs(deltaX) <= 0.0001f && std::abs(deltaY) <= 0.0001f)
            return false;
        const auto* node = FindNode(nodeGuid);
        if (node == nullptr || IsRootNode(nodeGuid))
            return false;
        const auto parentGuid = GetNodeParentGuid(nodeGuid);
        const auto* parent = FindNode(parentGuid);
        if (parent == nullptr)
            return false;
        const auto parentMode = parent->value("layout", Json::object()).value("mode", std::string("Absolute"));
        if (parentMode != "Absolute" && parentMode != "Overlay" && parentMode != "Scroll")
            return false;

        const auto layout = node->value("layout", Json::object());
        const auto anchorMin = layout.value("anchorMin", Json::array({ 0.0f, 0.0f }));
        const auto anchorMax = layout.value("anchorMax", Json::array({ 0.0f, 0.0f }));
        const auto readComponent = [](const Json& array, const size_t index) -> float
        {
            return array.is_array() && array.size() > index && array[index].is_number()
                ? array[index].get<float>()
                : 0.0f;
        };
        const bool stretchedX = std::abs(
            readComponent(anchorMax, 0u) - readComponent(anchorMin, 0u)) > 0.0001f;
        const bool stretchedY = std::abs(
            readComponent(anchorMax, 1u) - readComponent(anchorMin, 1u)) > 0.0001f;

        auto offsetMin = layout.value("offsetMin", Json::array({ 0.0f, 0.0f }));
        auto offsetMax = layout.value("offsetMax", Json::array({ 0.0f, 0.0f }));
        const auto shifted = [](const Json& array, const float delta, const size_t index) -> Json
        {
            Json result = array.is_array() && array.size() >= 2u ? array : Json::array({ 0.0f, 0.0f });
            result[index] = (result[index].is_number() ? result[index].get<float>() : 0.0f) + delta;
            return result;
        };

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        offsetMin = shifted(offsetMin, deltaX, 0u);
        offsetMin = shifted(offsetMin, deltaY, 1u);
        if (stretchedX)
            offsetMax = shifted(offsetMax, deltaX, 0u);
        if (stretchedY)
            offsetMax = shifted(offsetMax, deltaY, 1u);

        auto* mutableNode = FindNode(nodeGuid);
        if (mutableNode == nullptr)
            return false;
        (*mutableNode)["layout"]["offsetMin"] = std::move(offsetMin);
        if (stretchedX || stretchedY)
            (*mutableNode)["layout"]["offsetMax"] = std::move(offsetMax);
        return FinishChange(
            before,
            mergeStartedAt,
            true,
            "Nudge UI Element",
            std::string(nodeGuid),
            "UIDocument.Nudge:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::BringNodeForward(const std::string_view nodeGuid)
    {
        std::string parentGuid;
        size_t index = 0u;
        if (!IsLoaded() || !TryGetNodeLocation(nodeGuid, parentGuid, index))
            return false;
        const auto* parent = FindNode(parentGuid);
        if (parent == nullptr)
            return false;
        const auto count = GetChildCount(parentGuid);
        if (index + 1u >= count)
            return false;
        return MoveNode(nodeGuid, parentGuid, index + 2u);
    }

    bool UIDocumentDesignerModel::SendNodeBackward(const std::string_view nodeGuid)
    {
        std::string parentGuid;
        size_t index = 0u;
        if (!IsLoaded() || !TryGetNodeLocation(nodeGuid, parentGuid, index))
            return false;
        if (index == 0u)
            return false;
        return MoveNode(nodeGuid, parentGuid, index - 1u);
    }

    bool UIDocumentDesignerModel::BringNodeToFront(const std::string_view nodeGuid)
    {
        std::string parentGuid;
        size_t index = 0u;
        if (!IsLoaded() || !TryGetNodeLocation(nodeGuid, parentGuid, index))
            return false;
        const auto count = GetChildCount(parentGuid);
        if (index + 1u >= count)
            return false;
        return MoveNode(nodeGuid, parentGuid, count);
    }

    bool UIDocumentDesignerModel::SendNodeToBack(const std::string_view nodeGuid)
    {
        std::string parentGuid;
        size_t index = 0u;
        if (!IsLoaded() || !TryGetNodeLocation(nodeGuid, parentGuid, index))
            return false;
        if (index == 0u)
            return false;
        return MoveNode(nodeGuid, parentGuid, 0u);
    }

    bool UIDocumentDesignerModel::SetNodeVisible(
        const std::string_view nodeGuid,
        const bool visible)
    {
        const auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;
        const auto current = node->value("style", Json::object()).value("visible", true);
        if (current == visible)
            return false;
        return SetNodeProperty(
            nodeGuid,
            "style.visible",
            visible,
            true,
            visible ? "Show UI Element" : "Hide UI Element",
            "UIDocument.Visible:" + std::string(nodeGuid),
            false);
    }

    bool UIDocumentDesignerModel::SetNodeOffsets(
        const std::string_view nodeGuid,
        const NLS::Engine::UI::UIVector2 offsetMin,
        const NLS::Engine::UI::UIVector2 offsetMax,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;

        const auto sameVector = [](const Json& value, const float x, const float y)
        {
            return value.is_array() && value.size() >= 2u &&
                value[0].is_number() && value[1].is_number() &&
                std::abs(value[0].get<float>() - x) <= 0.0001f &&
                std::abs(value[1].get<float>() - y) <= 0.0001f;
        };
        const auto layout = node->value("layout", Json::object());
        if (sameVector(layout.value("offsetMin", Json::array()), offsetMin.x, offsetMin.y) &&
            sameVector(layout.value("offsetMax", Json::array()), offsetMax.x, offsetMax.y))
        {
            return false;
        }

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["layout"]["offsetMin"] = Json::array({ offsetMin.x, offsetMin.y });
        (*node)["layout"]["offsetMax"] = Json::array({ offsetMax.x, offsetMax.y });
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Move UI Element",
            std::string(nodeGuid),
            "UIDocument.Offsets:" + std::string(nodeGuid),
            true);
    }

    bool UIDocumentDesignerModel::SetNodeSizeAndOffsets(
        const std::string_view nodeGuid,
        const NLS::Engine::UI::UIVector2 size,
        const NLS::Engine::UI::UIVector2 offsetMin,
        const NLS::Engine::UI::UIVector2 offsetMax,
        const bool recordUndo)
    {
        auto* node = FindNode(nodeGuid);
        if (node == nullptr)
            return false;

        const auto sameVector = [](const Json& value, const float x, const float y)
        {
            return value.is_array() && value.size() >= 2u &&
                value[0].is_number() && value[1].is_number() &&
                std::abs(value[0].get<float>() - x) <= 0.0001f &&
                std::abs(value[1].get<float>() - y) <= 0.0001f;
        };
        const auto layout = node->value("layout", Json::object());
        if (sameVector(layout.value("size", Json::array()), size.x, size.y) &&
            sameVector(layout.value("offsetMin", Json::array()), offsetMin.x, offsetMin.y) &&
            sameVector(layout.value("offsetMax", Json::array()), offsetMax.x, offsetMax.y))
        {
            return false;
        }

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        (*node)["layout"]["size"] = Json::array({ std::max(0.0f, size.x), std::max(0.0f, size.y) });
        (*node)["layout"]["offsetMin"] = Json::array({ offsetMin.x, offsetMin.y });
        (*node)["layout"]["offsetMax"] = Json::array({ offsetMax.x, offsetMax.y });
        return FinishChange(
            before,
            mergeStartedAt,
            recordUndo,
            "Resize UI Element",
            std::string(nodeGuid),
            "UIDocument.Resize:" + std::string(nodeGuid),
            true);
    }

    std::string UIDocumentDesignerModel::SerializeNode(const std::string_view nodeGuid) const
    {
        const auto* node = FindNode(nodeGuid);
        return node == nullptr ? std::string {} : node->dump();
    }

    bool UIDocumentDesignerModel::PasteNode(
        const std::string_view parentNodeGuid,
        const size_t index,
        const std::string_view serializedNode)
    {
        auto* parent = FindNode(parentNodeGuid);
        if (parent == nullptr || serializedNode.empty())
            return false;

        auto parsed = Json::parse(serializedNode.begin(), serializedNode.end(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object() ||
            !parsed.contains("type") || !parsed["type"].is_string() ||
            !IsSupportedElementType(parsed["type"].get<std::string>()))
        {
            return false;
        }

        AssignFreshIdentifiers(parsed);
        const auto copyGuid = parsed.value("nodeGuid", std::string {});

        const auto mergeStartedAt = std::chrono::steady_clock::now();
        const auto before = CaptureSnapshot();
        auto& children = (*parent)["children"];
        if (!children.is_array())
            children = Json::array();
        const auto position = children.begin() + static_cast<std::ptrdiff_t>(
            std::min(index, children.size()));
        children.insert(position, std::move(parsed));
        SetSelection({ copyGuid });
        return FinishChange(before, mergeStartedAt, true, "Paste UI Element", copyGuid);
    }

    void UIDocumentDesignerModel::BeginTransaction(std::string label, std::string mergeKey)
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
        edit->beforeSelection = before.selection;
        edit->beforeLocalDirty = before.localDirty;
        edit->afterDocument = before.document;
        edit->afterSelection = before.selection;
        edit->afterLocalDirty = before.localDirty;

        ActiveTransaction active { edit, std::nullopt };
        NLS::Editor::Undo::UndoCommandInfo info;
        info.label = std::move(label);
        info.sourceModule = "UIDocumentDesigner";
        info.scopes = { edit->scope };
        if (!before.selection.empty())
            info.affected = before.selection;
        info.mergeKey = std::move(mergeKey);

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

    void UIDocumentDesignerModel::CancelTransaction()
    {
        CancelActiveTransaction();
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

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::FindParentRecursive(
        const Json& node,
        const std::string_view childNodeGuid)
    {
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return nullptr;
        for (const auto& child : *children)
        {
            if (child.value("nodeGuid", std::string {}) == childNodeGuid)
                return &node;
            if (const auto* found = FindParentRecursive(child, childNodeGuid))
                return found;
        }
        return nullptr;
    }

    bool UIDocumentDesignerModel::TryGetNodeIndexInParent(
        const Json& parent,
        const std::string_view childNodeGuid,
        size_t& index)
    {
        const auto children = parent.find("children");
        if (children == parent.end() || !children->is_array())
            return false;
        for (size_t candidate = 0u; candidate < children->size(); ++candidate)
        {
            if ((*children)[candidate].value("nodeGuid", std::string {}) == childNodeGuid)
            {
                index = candidate;
                return true;
            }
        }
        return false;
    }

    void UIDocumentDesignerModel::AssignFreshIdentifiers(Json& node)
    {
        node["nodeGuid"] = NLS::Guid::New().ToString();
        const auto children = node.find("children");
        if (children == node.end() || !children->is_array())
            return;
        for (auto& child : *children)
            AssignFreshIdentifiers(child);
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

    bool UIDocumentDesignerModel::NormalizePropertyValue(
        const std::string_view path,
        const Json& value,
        Json& normalized,
        std::string* reason)
    {
        const auto* spec = FindPropertySpec(path);
        if (spec == nullptr)
        {
            if (reason != nullptr)
                *reason = "unsupported UI property: " + std::string(path);
            return false;
        }

        const auto fail = [reason, path](const char* expected)
        {
            if (reason != nullptr)
                *reason = std::string(expected) + " expected for property " + std::string(path);
            return false;
        };

        switch (spec->kind)
        {
        case PropertyKind::String:
            if (!value.is_string())
                return fail("string");
            normalized = value;
            return true;
        case PropertyKind::Bool:
            if (!value.is_boolean())
                return fail("boolean");
            normalized = value;
            return true;
        case PropertyKind::Vector2:
            return ReadVector2Value(value, normalized) || fail("number pair");
        case PropertyKind::NonNegativeFloat:
            if (!value.is_number())
                return fail("number");
            normalized = std::max(0.0f, value.get<float>());
            return true;
        case PropertyKind::UnitFloat:
            if (!value.is_number())
                return fail("number");
            normalized = std::clamp(value.get<float>(), 0.0f, 1.0f);
            return true;
        case PropertyKind::UInt:
            if (!value.is_number_unsigned() && !value.is_number_integer())
                return fail("unsigned integer");
            normalized = static_cast<uint32_t>(std::max<int64_t>(0, value.get<int64_t>()));
            return true;
        case PropertyKind::UInt64:
            if (!value.is_number_unsigned() && !value.is_number_integer())
                return fail("unsigned integer");
            normalized = value.is_number_unsigned()
                ? value.get<uint64_t>()
                : static_cast<uint64_t>(std::max<int64_t>(0, value.get<int64_t>()));
            return true;
        case PropertyKind::PositiveUInt:
            if (!value.is_number_unsigned() && !value.is_number_integer())
                return fail("unsigned integer");
            normalized = std::max<uint32_t>(
                1u,
                static_cast<uint32_t>(std::max<int64_t>(0, value.get<int64_t>())));
            // The runtime stores fontPixelSize as uint16_t.
            if (path == "fontPixelSize")
                normalized = std::min<uint32_t>(65535u, normalized.get<uint32_t>());
            return true;
        case PropertyKind::Color:
            return ReadColorValue(value, normalized) || fail("color array");
        case PropertyKind::LayoutMode:
            if (!value.is_string() || !IsSupportedLayoutMode(value.get<std::string>()))
                return fail("layout mode");
            normalized = value;
            return true;
        case PropertyKind::Direction:
            if (!value.is_string())
                return fail("string");
            if (value.get<std::string>() != "Horizontal" && value.get<std::string>() != "Vertical")
                return fail("direction");
            normalized = value;
            return true;
        case PropertyKind::Align:
            if (!value.is_string())
                return fail("string");
            if (value.get<std::string>() != "Start" && value.get<std::string>() != "Center" &&
                value.get<std::string>() != "End" && value.get<std::string>() != "Stretch")
            {
                return fail("alignment");
            }
            normalized = value;
            return true;
        }
        return false;
    }

    bool UIDocumentDesignerModel::WriteNodeProperty(
        Json& node,
        const std::string_view path,
        const Json& value)
    {
        if (path.empty())
            return false;

        Json* current = &node;
        size_t start = 0u;
        while (start <= path.size())
        {
            const auto separator = path.find('.', start);
            const auto segment = path.substr(
                start,
                separator == std::string_view::npos
                    ? std::string_view::npos
                    : separator - start);
            if (segment.empty())
                return false;

            if (separator == std::string_view::npos)
            {
                (*current)[std::string(segment)] = value;
                return true;
            }

            auto& child = (*current)[std::string(segment)];
            if (!child.is_object())
                child = Json::object();
            current = &child;
            start = separator + 1u;
        }
        return false;
    }

    const UIDocumentDesignerModel::Json* UIDocumentDesignerModel::ReadNodeProperty(
        const Json& node,
        const std::string_view path)
    {
        if (path.empty())
            return nullptr;

        const Json* current = &node;
        size_t start = 0u;
        while (start <= path.size())
        {
            const auto separator = path.find('.', start);
            const auto segment = path.substr(
                start,
                separator == std::string_view::npos
                    ? std::string_view::npos
                    : separator - start);
            if (segment.empty() || !current->is_object())
                return nullptr;
            const auto member = current->find(std::string(segment));
            if (member == current->end())
                return nullptr;
            if (separator == std::string_view::npos)
                return &*member;
            current = &*member;
            start = separator + 1u;
        }
        return nullptr;
    }

    std::string UIDocumentDesignerModel::DefaultPropertyLabel(const std::string_view path)
    {
        if (path == "name") return "Rename UI Element";
        if (path == "text") return "Edit UI Text";
        if (path == "className") return "Edit UI Class";
        if (path == "password") return "Edit UI Password";
        if (path == "requiresRenderTarget") return "Edit UI Render Target";
        if (path == "fontAssetId" || path == "fontPixelSize") return "Edit UI Font";
        if (path.rfind("texture.", 0u) == 0u || path.rfind("font.", 0u) == 0u)
            return "Edit UI Resource Reference";
        if (path.rfind("nineSlice.", 0u) == 0u) return "Edit UI Nine Slice";
        if (path.rfind("style.", 0u) == 0u) return "Edit UI Style";
        return "Edit UI Layout";
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

    void UIDocumentDesignerModel::SyncPrimarySelection(Detail::UIDocumentDesignerState& state)
    {
        state.selectedNodeGuid = state.selectedNodeGuids.empty()
            ? std::string {}
            : state.selectedNodeGuids.front();
    }

    UIDocumentDesignerModel::DocumentSnapshot UIDocumentDesignerModel::CaptureSnapshot() const
    {
        return {
            m_state->document,
            m_state->selectedNodeGuids,
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
        edit->beforeSelection = before.selection;
        edit->beforeLocalDirty = before.localDirty;
        edit->afterDocument = after.document;
        edit->afterSelection = after.selection;
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
        edit.afterSelection = after.selection;
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
            snapshot.selection,
            snapshot.localDirty);
        m_state->documentRevision = snapshot.documentRevision;
    }

    void UIDocumentDesignerModel::Revalidate()
    {
        RevalidateState(*m_state);
    }
}
