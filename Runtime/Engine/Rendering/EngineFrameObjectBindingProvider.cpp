#include <Debug/Logger.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <Rendering/Core/ABaseRenderer.h>
#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Data/DrawableInstanceCount.h>
#include <Rendering/Data/ObjectDataLimits.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Settings/DriverSettings.h>

#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/IndexedObjectDataShaderSupport.h"

using namespace NLS;

namespace
{
    constexpr size_t kInitialObjectDataBufferCapacity = 256u;
    constexpr uint32_t kObjectDataSlotShrinkIdleFrameCount = 3u;
    constexpr size_t kIndexedObjectDataShaderSupportCacheMaxEntries = 1024u;
    constexpr size_t kInlineObjectSlotCount = 8u;

    class SmallObjectSlotBuffer final
    {
    public:
        SmallObjectSlotBuffer() = default;

        explicit SmallObjectSlotBuffer(const size_t count)
        {
            resize(count);
        }

        void resize(const size_t count)
        {
            if (count <= m_inline.size())
            {
                m_count = count;
                m_heap.clear();
                return;
            }

            if (m_heap.empty() && m_count != 0u)
                m_heap.assign(m_inline.begin(), m_inline.begin() + static_cast<std::ptrdiff_t>(m_count));
            m_heap.resize(count);
            m_count = count;
        }

        void push_back(const uint32_t value)
        {
            if (m_heap.empty() && m_count < m_inline.size())
            {
                m_inline[m_count++] = value;
                return;
            }

            if (m_heap.empty())
            {
                m_heap.assign(m_inline.begin(), m_inline.end());
            }
            m_heap.push_back(value);
            m_count = m_heap.size();
        }

        [[nodiscard]] size_t size() const noexcept { return m_count; }
        [[nodiscard]] bool empty() const noexcept { return m_count == 0u; }
        [[nodiscard]] uint32_t* data() noexcept { return m_heap.empty() ? m_inline.data() : m_heap.data(); }
        [[nodiscard]] const uint32_t* data() const noexcept { return m_heap.empty() ? m_inline.data() : m_heap.data(); }
        [[nodiscard]] uint32_t* begin() noexcept { return data(); }
        [[nodiscard]] const uint32_t* begin() const noexcept { return data(); }
        [[nodiscard]] uint32_t* end() noexcept { return data() + m_count; }
        [[nodiscard]] const uint32_t* end() const noexcept { return data() + m_count; }
        [[nodiscard]] uint32_t& front() noexcept { return data()[0]; }
        [[nodiscard]] const uint32_t& front() const noexcept { return data()[0]; }
        [[nodiscard]] uint32_t& operator[](const size_t index) noexcept { return data()[index]; }
        [[nodiscard]] const uint32_t& operator[](const size_t index) const noexcept { return data()[index]; }

    private:
        std::array<uint32_t, kInlineObjectSlotCount> m_inline {};
        std::vector<uint32_t> m_heap;
        size_t m_count = 0u;
    };

    thread_local std::unordered_map<
        const NLS::Engine::Rendering::EngineFrameObjectBindingProvider*,
        size_t> g_threadBoundCaptureFrameSlots;
    thread_local std::unordered_map<
        const NLS::Engine::Rendering::EngineFrameObjectBindingProvider*,
        std::optional<size_t>> g_threadResolvedCaptureFrameSlots;
}

namespace NLS::Engine::Rendering
{
EngineFrameObjectBindingProvider::EngineFrameObjectBindingProvider(NLS::Render::Core::CompositeRenderer& renderer)
    : FrameObjectBindingProvider(renderer)
{
    m_engineBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4) +
        sizeof(Maths::Matrix4) +
        sizeof(Maths::Matrix4) +
        sizeof(Maths::Vector3) +
        sizeof(float) +
        sizeof(Maths::Matrix4),
        0,
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_hlslFrameBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4) + sizeof(Maths::Vector3) + sizeof(float) + sizeof(Maths::Matrix4),
        NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0),
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_hlslObjectBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4),
        NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0),
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_hlslObjectBufferAlt = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4),
        NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0),
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_startTime = std::chrono::high_resolution_clock::now();
}

NLS::Render::Core::FrameObjectBindingProvider::PreparationState& EngineFrameObjectBindingProvider::ActivePreparationState() const
{
    const auto bound = g_threadBoundCaptureFrameSlots.find(this);
    if (bound == g_threadBoundCaptureFrameSlots.end())
        return m_preparationState;
    return const_cast<EngineFrameObjectBindingProvider*>(this)->CaptureStateForSlot(bound->second).preparation;
}

bool EngineFrameObjectBindingProvider::TryReservePreparedFrameResourcesUntil(
    const std::chrono::steady_clock::time_point retirementDeadline)
{
    {
        std::lock_guard lock(m_stateMutex);
        if (m_preparedFrameObjectDataSlotUnavailable)
            return false;
#if defined(NLS_ENABLE_TEST_HOOKS)
        if (m_activeObjectDataSlotIndexForTesting.has_value())
        {
            const auto slot = ResolveActiveObjectDataSlotIndex();
            CaptureStateForSlot(*slot);
            return true;
        }
#endif
        if (m_preparedFrameHasObjectDataSlot ||
            NLS::Render::Context::DriverRendererAccess::GetActiveFrameContextSlotIndex(m_renderer.GetDriver()).has_value())
        {
            const auto slot = ResolveActiveObjectDataSlotIndex();
            if (slot.has_value())
                CaptureStateForSlot(*slot);
            return slot.has_value();
        }
    }

    // Retirement may require a capture on another thread. Never wait holding provider state.
    const auto slot = NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
        m_renderer.GetDriver(), retirementDeadline);
    std::lock_guard lock(m_stateMutex);
    if (!slot.has_value())
    {
        m_preparedFrameObjectDataSlotUnavailable = true;
        return false;
    }
    m_preparedFrameObjectDataSlotIndex = *slot;
    m_preparedFrameHasObjectDataSlot = true;
    m_preparedFrameObjectDataSlotReserved = true;
    if (m_objectDataSlots.size() <= *slot)
        m_objectDataSlots.resize(*slot + 1u);
    CaptureStateForSlot(*slot);
    return true;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
uint64_t EngineFrameObjectBindingProvider::GetIndexedObjectDataShaderSupportQueryCountForTesting() const
{
    std::lock_guard lock(m_stateMutex);
    return m_indexedObjectDataShaderSupportQueryCount;
}

std::optional<size_t> EngineFrameObjectBindingProvider::GetBoundCaptureFrameSlotForTesting() const
{
    const auto boundIt = g_threadBoundCaptureFrameSlots.find(this);
    if (boundIt == g_threadBoundCaptureFrameSlots.end())
        return std::nullopt;
    return boundIt->second;
}

uint64_t EngineFrameObjectBindingProvider::GetLegacyObjectBufferWriteCountForTesting() const
{
    std::lock_guard lock(m_stateMutex);
    return m_legacyObjectBufferWriteCount;
}

EngineFrameObjectBindingProvider::ObjectDataWorkCountsForTesting
EngineFrameObjectBindingProvider::GetObjectDataWorkCountsForTesting() const
{
    std::lock_guard lock(m_stateMutex);
    return m_objectDataWorkCountsForTesting;
}

void EngineFrameObjectBindingProvider::SetActiveObjectDataSlotIndexForTesting(const size_t slotIndex)
{
    std::lock_guard lock(m_stateMutex);
    m_activeObjectDataSlotIndexForTesting = slotIndex;
}

void EngineFrameObjectBindingProvider::ResetObjectDataSlotForTesting(const size_t slotIndex)
{
    std::lock_guard lock(m_stateMutex);
    if (slotIndex < m_objectDataSlots.size())
        ResetObjectDataSlot(m_objectDataSlots[slotIndex]);
}
#endif

void EngineFrameObjectBindingProvider::PrepareRenderScenePackage(
    const NLS::Render::Context::FrameSnapshot&,
    NLS::Render::Context::RenderScenePackage& package) const
{
    package.frameDataReady = true;
    package.objectDataReady = true;
}

void EngineFrameObjectBindingProvider::OnBeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor)
{
    const auto frameGeneration = m_frameGeneration.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (frameGeneration == 0u)
        m_frameGeneration.store(1u, std::memory_order_relaxed);
    m_indexedObjectDataShaderSupportQueryCount = 0u;
#if defined(NLS_ENABLE_TEST_HOOKS)
    m_legacyObjectBufferWriteCount = 0u;
#endif
    ReleaseStalePreparedObjectDataSlotReservation();
    InvalidateObjectDataDeviceCachesIfNeeded();

    m_preparedFrameHasObjectDataSlot = false;
    m_preparedFrameObjectDataSlotReserved = false;
    m_preparedFrameObjectDataSlotUnavailable = false;
    g_threadBoundCaptureFrameSlots.erase(this);
    g_threadResolvedCaptureFrameSlots.erase(this);


    auto currentTime = std::chrono::high_resolution_clock::now();
    auto elapsedTime = std::chrono::duration_cast<std::chrono::duration<float>>(currentTime - m_startTime);

    size_t offset = sizeof(Maths::Matrix4);
    m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(frameDescriptor.camera->GetViewMatrix()), std::ref(offset));
    m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(frameDescriptor.camera->GetProjectionMatrix()), std::ref(offset));
    m_engineBuffer->SetSubData(frameDescriptor.camera->GetPosition(), std::ref(offset));
    m_engineBuffer->SetSubData(elapsedTime.count(), std::ref(offset));

    size_t hlslFrameOffset = 0;
    const auto viewProjection = frameDescriptor.camera->GetProjectionMatrix() * frameDescriptor.camera->GetViewMatrix();
    auto viewMatrixNoTranslation = frameDescriptor.camera->GetViewMatrix();
    viewMatrixNoTranslation(0, 3) = 0.0f;
    viewMatrixNoTranslation(1, 3) = 0.0f;
    viewMatrixNoTranslation(2, 3) = 0.0f;
    const auto viewProjectionNoTranslation = frameDescriptor.camera->GetProjectionMatrix() * viewMatrixNoTranslation;

    m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjection), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(frameDescriptor.camera->GetPosition(), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(elapsedTime.count(), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjectionNoTranslation), std::ref(hlslFrameOffset));
    // Frame constants changed. The slot that ends up capturing this frame marks
    // its own binding set dirty when it is bound, so a frame that never captures
    // does not reserve frame-slot resources.
    const auto transposedViewProjection = Maths::Matrix4::Transpose(viewProjection);
    const auto transposedNoTranslation = Maths::Matrix4::Transpose(viewProjectionNoTranslation);
    const auto position = frameDescriptor.camera->GetPosition();
    const float time = elapsedTime.count();
    size_t byteOffset = 0u;
    const auto append = [&](const auto& value)
    {
        std::memcpy(m_frameConstantData.data() + byteOffset, &value, sizeof(value));
        byteOffset += sizeof(value);
    };
    append(transposedViewProjection);
    append(position);
    append(time);
    append(transposedNoTranslation);
    m_pendingFrameConstantsDirty.store(true, std::memory_order_release);
    if (const auto activeSlot = NLS::Render::Context::DriverRendererAccess::GetActiveFrameContextSlotIndex(m_renderer.GetDriver());
        activeSlot.has_value())
        CaptureStateForSlot(*activeSlot);
}

void EngineFrameObjectBindingProvider::OnEndFrame()
{
    g_threadBoundCaptureFrameSlots.erase(this);
    g_threadResolvedCaptureFrameSlots[this] = std::nullopt;
}

bool EngineFrameObjectBindingProvider::OnTryReservePreparedFrameResources()
{
    return TryReservePreparedFrameResourcesUntil(
        std::chrono::steady_clock::now() +
        NLS::Render::Context::DriverRendererAccess::GetThreadedPublishRetirementWait(m_renderer.GetDriver()));
}

void EngineFrameObjectBindingProvider::OnReleaseReservedPreparedFrameResources()
{
    ReleaseStalePreparedObjectDataSlotReservation();
}

bool EngineFrameObjectBindingProvider::OnHasReservedPreparedFrameResources() const
{
    return m_preparedFrameObjectDataSlotReserved;
}

bool EngineFrameObjectBindingProvider::OnPrepareDraw(
    PipelineState&,
    const NLS::Render::Entities::Drawable& drawable)
{
    if (!IsFramePrepared())
        InvalidateObjectDataDeviceCachesIfNeeded();

    auto& captureState = ActiveCaptureState();
    captureState.currentDrawUsesIndexedObjectData = false;
    captureState.currentDrawRequiresIndexedObjectData = PreparedShaderRequiresIndexedObjectData();
    captureState.currentDrawUsesInstanceObjectIndirection =
        captureState.currentDrawRequiresIndexedObjectData &&
        PreparedShaderUsesInstanceObjectIndirection();
    captureState.currentDrawPrepared = true;
    captureState.currentDrawObjectConstants = {};

    const auto* descriptor = drawable.TryGetDescriptor<NLS::Render::Data::DrawableObjectDescriptor>();
    if (descriptor != nullptr)
    {
        captureState.currentDrawObjectConstants.objectFlags = descriptor->objectFlags;
        const bool hasExplicitObjectIndex =
            descriptor->objectIndex != NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
        if ((hasExplicitObjectIndex || captureState.currentDrawRequiresIndexedObjectData) &&
            TryPrepareIndexedObjectData(
                drawable,
                *descriptor,
                captureState,
                &captureState.currentDrawObjectConstants.objectIndex))
        {
            return true;
        }

        if (captureState.currentDrawRequiresIndexedObjectData)
        {
            captureState.currentDrawPrepared = false;
            return false;
        }

        m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(descriptor->modelMatrix), 0);
        m_engineBuffer->SetSubData(
            descriptor->userMatrix,
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Vector3) +
            sizeof(float));
#if defined(NLS_ENABLE_TEST_HOOKS)
        m_legacyObjectBufferWriteCount += 2u;
#endif

        auto& writeBuffer = captureState.useAltObjectBuffer ? *captureState.objectConstantsAltBuffer : *captureState.objectConstantsBuffer;
        writeBuffer.SetSubData(Maths::Matrix4::Transpose(descriptor->modelMatrix), 0);
        captureState.legacyObjectBindingSetDirty = true;
    }
    else if (captureState.currentDrawRequiresIndexedObjectData)
    {
        captureState.currentDrawPrepared = false;
        return false;
    }

    return captureState.currentDrawPrepared;
}

void EngineFrameObjectBindingProvider::OnPrepareExplicitDraw(
    NLS::Render::RHI::RHICommandBuffer& commandBuffer,
    PipelineState&,
    const NLS::Render::Entities::Drawable& drawable)
{
    auto& captureState = ActiveCaptureState();
    RefreshExplicitFrameBindingSet();
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> objectBindingSet;
    if (captureState.currentDrawUsesIndexedObjectData)
    {
        auto* slot = ResolveActiveObjectDataSlot();
        objectBindingSet = slot != nullptr ? RefreshExplicitIndexedObjectBindingSet(*slot) : nullptr;
    }
    else
    {
        RefreshExplicitLegacyObjectBindingSet();
        objectBindingSet = captureState.legacyObjectBindingSet;
    }

    if (captureState.frameBindingSet != nullptr)
        commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet, captureState.frameBindingSet);
    if (objectBindingSet != nullptr)
        commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet, objectBindingSet);
}

bool EngineFrameObjectBindingProvider::OnCaptureFrameBindingSet(
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& outBindingSet)
{
    auto& captureState = ActiveCaptureState();
    RefreshExplicitFrameBindingSet();
    outBindingSet = captureState.frameBindingSet;
    return outBindingSet != nullptr;
}

bool EngineFrameObjectBindingProvider::OnCapturePreparedBindingSets(
    PipelineState& pso,
    const NLS::Render::Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    if (!OnCapturePreparedObjectBindingSet(pso, drawable, outBindings))
        return false;

    auto& captureState = ActiveCaptureState();
    RefreshExplicitFrameBindingSet();
    outBindings.frameBindingSet = captureState.frameBindingSet;
    return outBindings.frameBindingSet != nullptr || outBindings.objectBindingSet != nullptr;
}

bool EngineFrameObjectBindingProvider::OnCapturePreparedObjectBindingSet(
    PipelineState&,
    const NLS::Render::Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    auto& captureState = ActiveCaptureState();
    if (!captureState.currentDrawPrepared)
        return false;

    ObjectDataFrameSlot* indexedObjectDataSlot = nullptr;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> objectBindingSet;
    if (captureState.currentDrawUsesIndexedObjectData)
    {
        indexedObjectDataSlot = ResolveActiveObjectDataSlot();
        if (indexedObjectDataSlot != nullptr)
        {
            objectBindingSet = RefreshExplicitIndexedObjectBindingSet(*indexedObjectDataSlot);
        }
    }
    else
    {
        RefreshExplicitLegacyObjectBindingSet();
        objectBindingSet = captureState.legacyObjectBindingSet;
    }

    const bool hasObjectDescriptor =
        drawable.TryGetDescriptor<NLS::Render::Data::DrawableObjectDescriptor>() != nullptr;
    if ((captureState.currentDrawUsesIndexedObjectData || hasObjectDescriptor) &&
        objectBindingSet == nullptr)
    {
        NLS_LOG_ERROR(
            "[EngineFrameObjectBindingProvider] object binding capture failed: "
            "indexed=" + std::to_string(captureState.currentDrawUsesIndexedObjectData ? 1u : 0u) +
            " slot=" + std::to_string(indexedObjectDataSlot != nullptr ? 1u : 0u) +
            " buffer=" + std::to_string(
                indexedObjectDataSlot != nullptr && indexedObjectDataSlot->buffer != nullptr ? 1u : 0u) +
            " device=" + std::to_string(m_explicitDevice != nullptr ? 1u : 0u));
        return false;
    }

    outBindings.objectBindingSet = std::move(objectBindingSet);
    outBindings.objectConstants = captureState.currentDrawObjectConstants;
    outBindings.usesObjectIndex = captureState.currentDrawUsesIndexedObjectData &&
        captureState.currentDrawObjectConstants.objectIndex !=
            NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
    return true;
}

void EngineFrameObjectBindingProvider::RefreshExplicitFrameBindingSet()
{
    if (!IsFramePrepared())
        InvalidateObjectDataDeviceCachesIfNeeded();
    auto& captureState = ActiveCaptureState();
    if (!captureState.frameBindingSetDirty)
        return;

    NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc bindingDesc;
    bindingDesc.set = NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet;
    bindingDesc.registerSpace = NLS::Render::RHI::BindingPointMap::kFrameBindingSpace;
    bindingDesc.binding = 0u;
    bindingDesc.range = sizeof(Maths::Matrix4) + sizeof(Maths::Vector3) + sizeof(float) + sizeof(Maths::Matrix4);
    bindingDesc.entryName = "FrameConstants";
    bindingDesc.layoutDebugName = "EngineFrameBindingLayout";
    bindingDesc.setDebugName = "EngineFrameBindingSet";
    bindingDesc.snapshotDebugName = "EngineFrameConstantsSnapshot";
    bindingDesc.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;
    auto newBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(
        *captureState.frameConstants,
        bindingDesc);

    captureState.deferredFrameBindingSet = std::move(captureState.frameBindingSet);
    captureState.frameBindingSet = std::move(newBindingSet);
    captureState.frameBindingSetDirty = false;
}

void EngineFrameObjectBindingProvider::RefreshExplicitLegacyObjectBindingSet()
{
    if (!IsFramePrepared())
        InvalidateObjectDataDeviceCachesIfNeeded();
    auto& captureState = ActiveCaptureState();
    if (!captureState.legacyObjectBindingSetDirty)
        return;

    NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc bindingDesc;
    bindingDesc.set = NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet;
    bindingDesc.registerSpace = NLS::Render::RHI::BindingPointMap::kObjectBindingSpace;
    bindingDesc.binding = 0u;
    bindingDesc.range = sizeof(Maths::Matrix4);
    bindingDesc.entryName = "ObjectConstants";
    bindingDesc.layoutDebugName = "EngineObjectBindingLayout";
    bindingDesc.setDebugName = "EngineObjectBindingSet";
    bindingDesc.snapshotDebugName = "EngineObjectConstantsSnapshot";
    bindingDesc.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;

    auto& writeBuffer = captureState.useAltObjectBuffer ? *captureState.objectConstantsAltBuffer : *captureState.objectConstantsBuffer;
    auto newBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(writeBuffer, bindingDesc);

    captureState.deferredLegacyObjectBindingSet = std::move(captureState.legacyObjectBindingSet);
    captureState.legacyObjectBindingSet = std::move(newBindingSet);
    captureState.legacyObjectBindingSetDirty = false;
    captureState.currentDrawUsesIndexedObjectData = false;
}

std::optional<size_t> EngineFrameObjectBindingProvider::ResolveActiveObjectDataSlotIndex(
    const std::optional<std::chrono::steady_clock::time_point> retirementDeadline)
{
    if (const auto boundIt = g_threadBoundCaptureFrameSlots.find(this);
        boundIt != g_threadBoundCaptureFrameSlots.end())
    {
        const auto slotIndex = boundIt->second;
        if (m_objectDataSlots.size() <= slotIndex)
            m_objectDataSlots.resize(slotIndex + 1u);
        return slotIndex;
    }

    if (m_preparedFrameObjectDataSlotUnavailable)
        return std::nullopt;

#if defined(NLS_ENABLE_TEST_HOOKS)
    if (m_activeObjectDataSlotIndexForTesting.has_value())
    {
        m_activeObjectDataSlotIndex = m_activeObjectDataSlotIndexForTesting.value();
        if (m_objectDataSlots.size() <= m_activeObjectDataSlotIndex)
            m_objectDataSlots.resize(m_activeObjectDataSlotIndex + 1u);
        return m_activeObjectDataSlotIndex;
    }
#endif

    const auto activeSlotIndex =
        NLS::Render::Context::DriverRendererAccess::GetActiveFrameContextSlotIndex(m_renderer.GetDriver());
    if (activeSlotIndex.has_value())
    {
        m_activeObjectDataSlotIndex = activeSlotIndex.value();
    }
    else if (m_preparedFrameHasObjectDataSlot)
    {
        m_activeObjectDataSlotIndex = m_preparedFrameObjectDataSlotIndex;
    }
    else
    {
        const auto reusableSlotIndex = retirementDeadline.has_value()
            ? NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
                m_renderer.GetDriver(),
                retirementDeadline.value())
            : NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
                m_renderer.GetDriver(), std::chrono::steady_clock::now());
        if (!reusableSlotIndex.has_value())
        {
            m_preparedFrameObjectDataSlotUnavailable = true;
            return std::nullopt;
        }
        m_activeObjectDataSlotIndex = reusableSlotIndex.value();
        m_preparedFrameObjectDataSlotIndex = m_activeObjectDataSlotIndex;
        m_preparedFrameHasObjectDataSlot = true;
        m_preparedFrameObjectDataSlotReserved = true;
    }

    if (m_objectDataSlots.size() <= m_activeObjectDataSlotIndex)
        m_objectDataSlots.resize(m_activeObjectDataSlotIndex + 1u);

    return m_activeObjectDataSlotIndex;
}

EngineFrameObjectBindingProvider::ObjectDataFrameSlot* EngineFrameObjectBindingProvider::ResolveActiveObjectDataSlot()
{
    const auto slotIndex = ResolveActiveObjectDataSlotIndex();
    if (!slotIndex.has_value())
        return nullptr;
    return &m_objectDataSlots[slotIndex.value()];
}

void EngineFrameObjectBindingProvider::ReleaseStalePreparedObjectDataSlotReservation()
{
    if (!m_preparedFrameObjectDataSlotReserved)
        return;

    const bool released = NLS::Render::Context::DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(
        m_renderer.GetDriver(),
        m_preparedFrameObjectDataSlotIndex);
    (void)released;
    m_preparedFrameObjectDataSlotReserved = false;
}

void EngineFrameObjectBindingProvider::ResetObjectDataSlot(ObjectDataFrameSlot& slot)
{
    const auto slotIndex = static_cast<size_t>(&slot - m_objectDataSlots.data());
    {
        std::lock_guard lock(m_frameCaptureStatesMutex);
        if (slotIndex < m_frameCaptureStates.size() && m_frameCaptureStates[slotIndex] != nullptr)
            ResetCaptureStateBindings(*m_frameCaptureStates[slotIndex]);
    }

    slot.buffer.reset();
    slot.indirectBuffer.reset();
    slot.bindingSet.reset();
    slot.deferredBindingSet.reset();
    slot.deviceIdentity = 0u;
    slot.objectDataShadow = {};
    slot.objectDataSourceShadow = {};
    slot.objectDataSourceValid = {};
    slot.objectDataIndirectShadow = {};
    slot.objectDataRevisionMetadata = {};
    slot.objectDataPhysicalSlotOccupancy = {};
    slot.objectDataExplicitLogicalOwnerByPhysicalSlot = {};
    slot.explicitObjectPhysicalSlots = {};
    slot.nextTransientObjectIndex = 0u;
    slot.nextTransientIndirectionIndex = 0u;
    slot.capacity = 0u;
    slot.indirectionCapacity = 0u;
    slot.idleFrameCount = 0u;
    slot.bindingSetDirty = true;
    slot.bindingSetUsesInstanceIndirection = false;
    slot.usedThisFrame = false;
}

void EngineFrameObjectBindingProvider::RetireIdleObjectDataSlots()
{
    for (auto& slot : m_objectDataSlots)
    {
        if (slot.usedThisFrame)
        {
            slot.idleFrameCount = 0u;
            continue;
        }

        if (slot.capacity <= kInitialObjectDataBufferCapacity)
            continue;

        ++slot.idleFrameCount;
        if (slot.idleFrameCount >= kObjectDataSlotShrinkIdleFrameCount)
            ResetObjectDataSlot(slot);
    }
}

void EngineFrameObjectBindingProvider::InvalidateObjectDataDeviceCachesIfNeeded()
{
    const auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_renderer.GetDriver());
    m_explicitDevice = device;
    const auto deviceIdentity = device != nullptr ? device->GetCacheIdentity() : 0u;
    if (deviceIdentity == m_cachedObjectDataDeviceIdentity)
        return;

    m_cachedObjectDataDeviceIdentity = deviceIdentity;
    ResetCaptureStateBindings(m_fallbackCaptureState);
    {
        std::lock_guard lock(m_frameCaptureStatesMutex);
        for (auto& captureState : m_frameCaptureStates)
        {
            if (captureState != nullptr)
                ResetCaptureStateBindings(*captureState);
        }
    }
    m_pendingFrameConstantsDirty.store(true, std::memory_order_release);
    g_threadBoundCaptureFrameSlots.erase(this);
    g_threadResolvedCaptureFrameSlots.erase(this);
    m_objectDataBindingLayout.reset();
    m_objectDataBindingLayoutDeviceIdentity = 0u;
    m_legacyObjectDataBindingLayout.reset();
    m_legacyObjectDataBindingLayoutDeviceIdentity = 0u;
    for (auto& slot : m_objectDataSlots)
        ResetObjectDataSlot(slot);
}

bool EngineFrameObjectBindingProvider::EnsureObjectDataBufferCapacity(
    ObjectDataFrameSlot& slot,
    const uint32_t objectIndex)
{
    const auto& device = m_explicitDevice;
    if (device == nullptr)
        return false;

    const auto deviceIdentity = device->GetCacheIdentity();
    if (slot.deviceIdentity != 0u && slot.deviceIdentity != deviceIdentity)
        ResetObjectDataSlot(slot);

    const auto requiredCapacity = static_cast<size_t>(objectIndex) + 1u;
    if (slot.buffer != nullptr && slot.capacity >= requiredCapacity)
        return true;

    auto newCapacity = slot.capacity != 0u
        ? slot.capacity
        : kInitialObjectDataBufferCapacity;
    while (newCapacity < requiredCapacity)
    {
        if (newCapacity > (std::numeric_limits<size_t>::max)() / 2u)
            return false;
        newCapacity *= 2u;
    }

    NLS::Render::RHI::RHIBufferDesc bufferDesc;
    bufferDesc.size = newCapacity * sizeof(Maths::Matrix4);
    bufferDesc.usage = NLS::Render::RHI::BufferUsageFlags::ShaderRead;
    bufferDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    bufferDesc.debugName = "EngineObjectDataBuffer";
    auto buffer = device->CreateBuffer(bufferDesc);
    if (buffer == nullptr)
        return false;

    if (!slot.objectDataShadow.empty())
    {
        NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
        uploadDesc.data = slot.objectDataShadow.data();
        uploadDesc.dataSize = slot.objectDataShadow.size() * sizeof(Maths::Matrix4);
        uploadDesc.destinationOffset = 0u;
        uploadDesc.debugName = "EngineObjectDataPreservedOnGrow";
        const auto updateResult = buffer->UpdateData(uploadDesc);
        if (!updateResult.Succeeded())
            return false;
#if defined(NLS_ENABLE_TEST_HOOKS)
        ++m_objectDataWorkCountsForTesting.uploadCount;
#endif
    }

    slot.buffer = std::move(buffer);
    slot.deviceIdentity = deviceIdentity;
    slot.capacity = newCapacity;
    slot.bindingSetDirty = true;
    return true;
}

bool EngineFrameObjectBindingProvider::EnsureIndirectionBufferCapacity(
    ObjectDataFrameSlot& slot,
    const uint32_t lastIndex)
{
    const auto& device = m_explicitDevice;
    if (device == nullptr)
        return false;

    const size_t requiredCapacity = static_cast<size_t>(lastIndex) + 1u;
    if (slot.indirectBuffer != nullptr && slot.indirectionCapacity >= requiredCapacity)
        return true;

    auto newCapacity = slot.indirectionCapacity != 0u
        ? slot.indirectionCapacity
        : kInitialObjectDataBufferCapacity;
    while (newCapacity < requiredCapacity)
    {
        if (newCapacity > (std::numeric_limits<size_t>::max)() / 2u)
            return false;
        newCapacity *= 2u;
    }

    NLS::Render::RHI::RHIBufferDesc bufferDesc;
    bufferDesc.size = newCapacity * sizeof(uint32_t);
    bufferDesc.usage = NLS::Render::RHI::BufferUsageFlags::ShaderRead;
    bufferDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    bufferDesc.debugName = "EngineInstanceObjectIndexBuffer";
    auto buffer = device->CreateBuffer(bufferDesc);
    if (buffer == nullptr)
        return false;

    if (!slot.objectDataIndirectShadow.empty())
    {
        NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
        uploadDesc.data = slot.objectDataIndirectShadow.data();
        uploadDesc.dataSize = slot.objectDataIndirectShadow.size() * sizeof(uint32_t);
        uploadDesc.destinationOffset = 0u;
        uploadDesc.debugName = "EngineInstanceObjectIndexPreservedOnGrow";
        if (!buffer->UpdateData(uploadDesc).Succeeded())
            return false;
    }

    slot.indirectBuffer = std::move(buffer);
    slot.indirectionCapacity = newCapacity;
    slot.bindingSetDirty = true;
    return true;
}

void EngineFrameObjectBindingProvider::BindCaptureFrameSlot(const size_t slotIndex)
{
    std::lock_guard lock(m_stateMutex);
    // An existing slot retains the producing frame, even after Game has begun another.
    const auto previous = g_threadBoundCaptureFrameSlots.find(this);
    const auto previousSlot = previous != g_threadBoundCaptureFrameSlots.end()
        ? std::optional<size_t>(previous->second) : std::nullopt;
    g_threadBoundCaptureFrameSlots[this] = slotIndex;
    try
    {
        CaptureStateForSlot(slotIndex);
    }
    catch (...)
    {
        if (previousSlot.has_value())
            g_threadBoundCaptureFrameSlots[this] = *previousSlot;
        else
            g_threadBoundCaptureFrameSlots.erase(this);
        throw;
    }
    g_threadBoundCaptureFrameSlots[this] = slotIndex;
    g_threadResolvedCaptureFrameSlots[this] = slotIndex;
}

std::function<void(size_t)> EngineFrameObjectBindingProvider::FreezeCaptureFrame()
{
    std::lock_guard lock(m_stateMutex);
    return [this, constants = m_frameConstantData, preparation = m_preparationState,
            generation = m_frameGeneration.load(std::memory_order_acquire)](size_t slotIndex)
    {
        std::lock_guard captureLock(m_stateMutex);
        BindCaptureFrameSlot(slotIndex);
        auto& state = CaptureStateForSlot(slotIndex);
        if (state.lastFrameGeneration != generation && slotIndex < m_objectDataSlots.size())
        {
            auto& slot = m_objectDataSlots[slotIndex];
            slot.objectDataPhysicalSlotOccupancy.clear();
            slot.objectDataExplicitLogicalOwnerByPhysicalSlot.clear();
            slot.explicitObjectPhysicalSlots.clear();
            slot.nextTransientObjectIndex = 0u;
            slot.nextTransientIndirectionIndex = 0u;
            slot.usedThisFrame = false;
            slot.deferredBindingSet.reset();
        }
        state.frameConstants->SetRawData(constants.data(), static_cast<uint32_t>(constants.size()));
        state.preparation = preparation;
        state.preparation.framePrepared = true;
        state.lastFrameGeneration = generation;
        state.frameBindingSetDirty = true;
        state.legacyObjectBindingSetDirty = true;
    };
}

void EngineFrameObjectBindingProvider::UnbindCaptureFrameSlot()
{
    std::lock_guard lock(m_stateMutex);
    g_threadBoundCaptureFrameSlots.erase(this);
    // Keep this thread's resolution locked for the remainder of the frame. This
    // preserves the old EndFrame behavior without exposing a provider-wide slot
    // to another capture thread.
    g_threadResolvedCaptureFrameSlots[this] = std::nullopt;
}

EngineFrameObjectBindingProvider::FrameCaptureState&
EngineFrameObjectBindingProvider::CaptureStateForSlot(const size_t slotIndex)
{
    FrameCaptureState* captureState = nullptr;
    {
        std::lock_guard lock(m_frameCaptureStatesMutex);
        if (m_frameCaptureStates.size() <= slotIndex)
            m_frameCaptureStates.resize(slotIndex + 1u);
        if (m_frameCaptureStates[slotIndex] == nullptr)
            m_frameCaptureStates[slotIndex] = std::make_unique<FrameCaptureState>();
        captureState = m_frameCaptureStates[slotIndex].get();
    }
    const bool bound = g_threadBoundCaptureFrameSlots.find(this) != g_threadBoundCaptureFrameSlots.end();
    if (!bound || captureState->frameConstants == nullptr)
    {
        const auto generation = m_frameGeneration.load(std::memory_order_acquire);
        if (captureState->lastFrameGeneration != generation && slotIndex < m_objectDataSlots.size())
        {
            auto& slot = m_objectDataSlots[slotIndex];
            slot.idleFrameCount = slot.usedThisFrame ? 0u : slot.idleFrameCount + 1u;
            if (slot.capacity > kInitialObjectDataBufferCapacity &&
                slot.idleFrameCount >= kObjectDataSlotShrinkIdleFrameCount)
                ResetObjectDataSlot(slot);
            slot.objectDataPhysicalSlotOccupancy.clear();
            slot.objectDataExplicitLogicalOwnerByPhysicalSlot.clear();
            slot.explicitObjectPhysicalSlots.clear();
            slot.nextTransientObjectIndex = 0u;
            slot.nextTransientIndirectionIndex = 0u;
            slot.usedThisFrame = false;
            slot.deferredBindingSet.reset();
        }
        ActivateCaptureStateForCurrentFrame(*captureState);
    }
    return *captureState;
}

void EngineFrameObjectBindingProvider::ResetCaptureStateBindings(FrameCaptureState& captureState)
{
    captureState.frameBindingSet.reset();
    captureState.legacyObjectBindingSet.reset();
    captureState.deferredFrameBindingSet.reset();
    captureState.deferredLegacyObjectBindingSet.reset();
    captureState.lastFrameGeneration = 0u;
    captureState.frameBindingSetDirty = true;
    captureState.legacyObjectBindingSetDirty = true;
}

void EngineFrameObjectBindingProvider::ActivateCaptureStateForCurrentFrame(FrameCaptureState& captureState)
{
    const auto frameGeneration = m_frameGeneration.load(std::memory_order_acquire);
    if (captureState.lastFrameGeneration == frameGeneration && captureState.frameConstants != nullptr)
        return;

    if (captureState.frameConstants == nullptr)
    {
        captureState.frameConstants = std::make_unique<NLS::Render::Buffers::UniformBuffer>(m_frameConstantData.size());
        captureState.objectConstantsBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(sizeof(Maths::Matrix4));
        captureState.objectConstantsAltBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(sizeof(Maths::Matrix4));
    }
    captureState.frameConstants->SetRawData(m_frameConstantData.data(), static_cast<uint32_t>(m_frameConstantData.size()));
    captureState.preparation = m_preparationState;
    captureState.preparation.framePrepared = true;
    captureState.useAltObjectBuffer = !captureState.useAltObjectBuffer;
    captureState.deferredFrameBindingSet.reset();
    captureState.deferredLegacyObjectBindingSet.reset();

    captureState.lastFrameGeneration = frameGeneration;
    captureState.frameBindingSetDirty = true;
    captureState.legacyObjectBindingSetDirty = true;
}

EngineFrameObjectBindingProvider::FrameCaptureState*
EngineFrameObjectBindingProvider::TryResolveActiveCaptureState(
    const std::optional<std::chrono::steady_clock::time_point> retirementDeadline)
{
    if (const auto boundIt = g_threadBoundCaptureFrameSlots.find(this);
        boundIt != g_threadBoundCaptureFrameSlots.end())
    {
        return &CaptureStateForSlot(boundIt->second);
    }

    auto resolvedIt = g_threadResolvedCaptureFrameSlots.find(this);
    if (resolvedIt == g_threadResolvedCaptureFrameSlots.end())
    {
        (void)retirementDeadline;
        // Capture state must never create a frame-slot reservation on its own:
        // that would change publish back-pressure for frames that only capture a
        // frame binding set. Frames without an active slot fall back to the shared
        // state, which matches the single-frame-in-flight execution model.
#if defined(NLS_ENABLE_TEST_HOOKS)
        if (m_activeObjectDataSlotIndexForTesting.has_value())
        {
            resolvedIt = g_threadResolvedCaptureFrameSlots.emplace(
                this,
                m_activeObjectDataSlotIndexForTesting.value()).first;
        }
        else
#endif
        if (const auto activeSlotIndex =
                NLS::Render::Context::DriverRendererAccess::GetActiveFrameContextSlotIndex(m_renderer.GetDriver());
            activeSlotIndex.has_value())
        {
            resolvedIt = g_threadResolvedCaptureFrameSlots.emplace(this, activeSlotIndex.value()).first;
        }
        else if (m_preparedFrameHasObjectDataSlot)
        {
            resolvedIt = g_threadResolvedCaptureFrameSlots.emplace(this, m_preparedFrameObjectDataSlotIndex).first;
        }
        else
        {
            g_threadResolvedCaptureFrameSlots.emplace(this, std::nullopt);
            return nullptr;
        }
    }

    if (!resolvedIt->second.has_value())
        return nullptr;

    auto& captureState = CaptureStateForSlot(resolvedIt->second.value());
    if (m_pendingFrameConstantsDirty.exchange(false, std::memory_order_acq_rel))
    {
        captureState.frameBindingSetDirty = true;
    }
    return &captureState;
}

EngineFrameObjectBindingProvider::FrameCaptureState*
EngineFrameObjectBindingProvider::TryGetActiveCaptureState()
{
    return TryResolveActiveCaptureState();
}

EngineFrameObjectBindingProvider::FrameCaptureState&
EngineFrameObjectBindingProvider::ActiveCaptureState()
{
    if (auto* captureState = TryResolveActiveCaptureState())
        return *captureState;
    ActivateCaptureStateForCurrentFrame(m_fallbackCaptureState);
    if (m_pendingFrameConstantsDirty.exchange(false, std::memory_order_acq_rel))
    {
        m_fallbackCaptureState.frameBindingSetDirty = true;
    }
    return m_fallbackCaptureState;
}

std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> EngineFrameObjectBindingProvider::ResolveObjectDataBindingLayout(
    const bool usesInstanceIndirection,
    const uint64_t deviceIdentity)
{
    auto& layout = usesInstanceIndirection ? m_objectDataBindingLayout : m_legacyObjectDataBindingLayout;
    auto& layoutDeviceIdentity = usesInstanceIndirection
        ? m_objectDataBindingLayoutDeviceIdentity
        : m_legacyObjectDataBindingLayoutDeviceIdentity;

    if (layout != nullptr && layoutDeviceIdentity != 0u && layoutDeviceIdentity != deviceIdentity)
    {
        layout.reset();
        layoutDeviceIdentity = 0u;
    }

    if (layout != nullptr)
        return layout;

    NLS::Render::RHI::RHIBindingLayoutDesc layoutDesc;
    layoutDesc.debugName = usesInstanceIndirection
        ? "EngineObjectBindingLayout"
        : "EngineLegacyObjectBindingLayout";
    layoutDesc.entries.push_back({
        "ObjectData",
        NLS::Render::RHI::BindingType::StructuredBuffer,
        NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet,
        0u,
        1u,
        NLS::Render::RHI::ShaderStageMask::Vertex,
        NLS::Render::RHI::BindingPointMap::kObjectBindingSpace,
        static_cast<uint32_t>(sizeof(Maths::Matrix4))
    });
    if (usesInstanceIndirection)
    {
        layoutDesc.entries.push_back({
            "InstanceObjectIndices",
            NLS::Render::RHI::BindingType::StructuredBuffer,
            NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet,
            1u,
            1u,
            NLS::Render::RHI::ShaderStageMask::Vertex,
            NLS::Render::RHI::BindingPointMap::kObjectBindingSpace,
            static_cast<uint32_t>(sizeof(uint32_t))
        });
    }

    layout = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingLayout(
        m_renderer.GetDriver(),
        layoutDesc);
    layoutDeviceIdentity = layout != nullptr ? deviceIdentity : 0u;
    return layout;
}

std::shared_ptr<NLS::Render::RHI::RHIBindingSet> EngineFrameObjectBindingProvider::RefreshExplicitIndexedObjectBindingSet(
    ObjectDataFrameSlot& slot)
{
    const bool usesInstanceIndirection =
        ActiveCaptureState().currentDrawUsesInstanceObjectIndirection && slot.indirectBuffer != nullptr;
    if (!slot.bindingSetDirty && slot.bindingSetUsesInstanceIndirection == usesInstanceIndirection)
        return slot.bindingSet;
    if (slot.buffer == nullptr)
        return nullptr;

    const auto& device = m_explicitDevice;
    if (device == nullptr)
        return nullptr;
    const auto deviceIdentity = device->GetCacheIdentity();
    if (slot.deviceIdentity != deviceIdentity)
    {
        ResetObjectDataSlot(slot);
        return nullptr;
    }

    const auto bindingLayout = ResolveObjectDataBindingLayout(usesInstanceIndirection, deviceIdentity);
    if (bindingLayout == nullptr)
        return nullptr;

    NLS::Render::RHI::RHIBindingSetDesc bindingSetDesc;
    bindingSetDesc.layout = bindingLayout;
    bindingSetDesc.debugName = "EngineObjectBindingSet";
    bindingSetDesc.entries.push_back({
        0u,
        NLS::Render::RHI::BindingType::StructuredBuffer,
        slot.buffer,
        0u,
        slot.buffer->GetDesc().size,
        static_cast<uint32_t>(sizeof(Maths::Matrix4)),
        nullptr,
        nullptr
    });
    if (usesInstanceIndirection)
    {
        bindingSetDesc.entries.push_back({
            1u,
            NLS::Render::RHI::BindingType::StructuredBuffer,
            slot.indirectBuffer,
            0u,
            slot.indirectBuffer->GetDesc().size,
            static_cast<uint32_t>(sizeof(uint32_t)),
            nullptr,
            nullptr
        });
    }

    auto newBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        m_renderer.GetDriver(),
        bindingSetDesc,
        NLS::Render::RHI::DescriptorAllocationLifetime::Persistent);
    if (newBindingSet == nullptr)
        return nullptr;

    // The engine object binding set is built on the driver directly rather than
    // through the uniform-buffer helper, so report it explicitly or Frame Info
    // under-reports binding set creation for indexed object-data draws.
    m_renderer.RecordRenderBindingSetCreation();

    slot.deferredBindingSet = std::move(slot.bindingSet);
    slot.bindingSet = std::move(newBindingSet);
    slot.bindingSetDirty = false;
    slot.bindingSetUsesInstanceIndirection = usesInstanceIndirection;
    return slot.bindingSet;
}

bool EngineFrameObjectBindingProvider::TryPrepareIndexedObjectData(
    const NLS::Render::Entities::Drawable& drawable,
    const NLS::Render::Data::DrawableObjectDescriptor& descriptor,
    FrameCaptureState& captureState,
    uint32_t* preparedObjectIndex)
{
    auto objectIndex = descriptor.objectIndex;
    auto instanceCount = NLS::Render::Data::ResolveDrawableInstanceCount(drawable).count;
    if (instanceCount == 0u)
        instanceCount = descriptor.objectCount;
    if (instanceCount == 0u)
        return false;
    auto objectCount = descriptor.objectCount;
    if (objectCount == 0u)
        objectCount = instanceCount;
    if (objectCount != instanceCount)
        return false;
    const bool hasExplicitObjectIndices =
        descriptor.instanceObjectIndices.size() >= objectCount &&
        std::all_of(
            descriptor.instanceObjectIndices.begin(),
            descriptor.instanceObjectIndices.begin() + static_cast<std::ptrdiff_t>(objectCount),
            [](const uint32_t value)
            {
                return value != NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
            });
    const bool hasExplicitObjectIndex =
        descriptor.objectIndex != NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
    const auto* shader = GetPreparedShader();
    if (shader != nullptr && !ShaderRequiresIndexedObjectData(*shader))
    {
        return false;
    }

    auto* objectDataSlot = ResolveActiveObjectDataSlot();
    if (objectDataSlot == nullptr)
        return false;

    const Maths::Matrix4* sourceMatrixData = nullptr;
    if (descriptor.instanceModelMatrices.empty())
    {
        if (objectCount != 1u)
            return false;
        sourceMatrixData = &descriptor.modelMatrix;
    }
    else
    {
        if (descriptor.instanceModelMatrices.size() < objectCount)
            return false;
        sourceMatrixData = descriptor.instanceModelMatrices.data();
    }

    constexpr uint8_t kPhysicalSlotFree = 0u;
    constexpr uint8_t kPhysicalSlotTransient = 1u;
    constexpr uint8_t kPhysicalSlotExplicit = 2u;
    const auto maxObjectDataCount = NLS::Render::Data::GetMaxObjectDataCount();
    const bool usesInstanceIndirection = captureState.currentDrawUsesInstanceObjectIndirection;
    const bool canUseSingleObjectFastPath =
        objectCount == 1u &&
        hasExplicitObjectIndex &&
        !hasExplicitObjectIndices &&
        !usesInstanceIndirection &&
        objectIndex < maxObjectDataCount;
    if (canUseSingleObjectFastPath)
    {
        if (objectDataSlot->objectDataPhysicalSlotOccupancy.size() <= objectIndex)
        {
            objectDataSlot->objectDataPhysicalSlotOccupancy.resize(
                static_cast<size_t>(objectIndex) + 1u,
                kPhysicalSlotFree);
        }

        bool canClaimIdentitySlot =
            objectDataSlot->objectDataPhysicalSlotOccupancy[objectIndex] == kPhysicalSlotFree;
        if (!canClaimIdentitySlot &&
            objectDataSlot->objectDataPhysicalSlotOccupancy[objectIndex] == kPhysicalSlotExplicit)
        {
            if (objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot.size() <= objectIndex)
            {
                objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot.resize(
                    static_cast<size_t>(objectIndex) + 1u,
                    NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
            }
            canClaimIdentitySlot =
                objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot[objectIndex] == objectIndex;
        }

        if (canClaimIdentitySlot)
        {
            const bool claimedPhysicalSlot =
                objectDataSlot->objectDataPhysicalSlotOccupancy[objectIndex] == kPhysicalSlotFree;
            const auto rollbackFastPath = [objectDataSlot, objectIndex, claimedPhysicalSlot]()
            {
                if (claimedPhysicalSlot)
                    objectDataSlot->objectDataPhysicalSlotOccupancy[objectIndex] = kPhysicalSlotFree;
                if (objectIndex < objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot.size())
                {
                    objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot[objectIndex] =
                        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
                }
                if (objectIndex < objectDataSlot->objectDataRevisionMetadata.size())
                    objectDataSlot->objectDataRevisionMetadata[objectIndex].valid = false;
            };

            objectDataSlot->objectDataPhysicalSlotOccupancy[objectIndex] = kPhysicalSlotExplicit;
            if (objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot.size() <= objectIndex)
            {
                objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot.resize(
                    static_cast<size_t>(objectIndex) + 1u,
                    NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
            }
            objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot[objectIndex] = objectIndex;
            if (!EnsureObjectDataBufferCapacity(*objectDataSlot, objectIndex) ||
                objectDataSlot->buffer == nullptr)
            {
                rollbackFastPath();
                m_renderer.RecordObjectDataFastPath(false);
                return false;
            }

            const bool canUseRevisionMetadata = descriptor.HasCompleteTrustedSingleObjectDataRevision();
            bool revisionMetadataHit = false;
            NLS::Render::Data::ObjectDataRevisionFallbackReason fallbackReason =
                NLS::Render::Data::ObjectDataRevisionFallbackReason::Descriptor;
            if (canUseRevisionMetadata &&
                objectDataSlot->objectDataRevisionMetadata.size() > objectIndex)
            {
                const auto& metadata = objectDataSlot->objectDataRevisionMetadata[objectIndex];
                if (!metadata.valid)
                {
                    const bool metadataIsUninitialized =
                        metadata.stableSceneIdentity == NLS::Render::Data::StaticDrawSceneIdentity {} &&
                        metadata.transformRevision == 0u &&
                        metadata.objectIndex == NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex &&
                        metadata.objectCount == 0u;
                    fallbackReason = metadataIsUninitialized
                        ? NLS::Render::Data::ObjectDataRevisionFallbackReason::MetadataUninitialized
                        : NLS::Render::Data::ObjectDataRevisionFallbackReason::MetadataInvalid;
                }
                else if (metadata.stableSceneIdentity != descriptor.stableSceneIdentity)
                {
                    fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::StableIdentityMismatch;
                }
                else if (metadata.transformRevision != descriptor.transformRevision)
                {
                    fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::TransformMismatch;
                }
                else if (metadata.objectIndex != descriptor.objectIndex)
                {
                    fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::ObjectIndexMismatch;
                }
                else if (metadata.objectCount != objectCount)
                {
                    fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::ObjectCountMismatch;
                }
                else
                {
                    revisionMetadataHit = true;
                }
            }
            else if (canUseRevisionMetadata)
            {
                fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::MetadataUnavailable;
            }

            if (!revisionMetadataHit)
            {
                if (!canUseRevisionMetadata &&
                    objectDataSlot->objectDataRevisionMetadata.size() > objectIndex)
                {
                    objectDataSlot->objectDataRevisionMetadata[objectIndex].valid = false;
                }

                const auto ensureShadowCapacity = [objectDataSlot](const uint32_t slot)
                {
                    if (objectDataSlot->objectDataShadow.size() <= slot)
                    {
                        objectDataSlot->objectDataShadow.resize(
                            static_cast<size_t>(slot) + 1u,
                            Maths::Matrix4::Identity);
                    }
                    if (objectDataSlot->objectDataSourceShadow.size() <= slot)
                    {
                        objectDataSlot->objectDataSourceShadow.resize(
                            static_cast<size_t>(slot) + 1u,
                            Maths::Matrix4::Identity);
                    }
                    if (objectDataSlot->objectDataSourceValid.size() <= slot)
                    {
                        objectDataSlot->objectDataSourceValid.resize(
                            static_cast<size_t>(slot) + 1u,
                            uint8_t {0u});
                    }
                };
                ensureShadowCapacity(objectIndex);

                const auto& sourceMatrix = *sourceMatrixData;
#if defined(NLS_ENABLE_TEST_HOOKS)
                if (objectDataSlot->objectDataSourceValid[objectIndex] != 0u)
                    ++m_objectDataWorkCountsForTesting.memcmpCount;
#endif
                const bool sourceChanged =
                    objectDataSlot->objectDataSourceValid[objectIndex] == 0u ||
                    !(objectDataSlot->objectDataSourceShadow[objectIndex] == sourceMatrix);
                if (sourceChanged)
                {
#if defined(NLS_ENABLE_TEST_HOOKS)
                    ++m_objectDataWorkCountsForTesting.transposeCount;
#endif
                    const auto transposed = Maths::Matrix4::Transpose(sourceMatrix);
                    NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
                    uploadDesc.data = &transposed;
                    uploadDesc.dataSize = sizeof(Maths::Matrix4);
                    uploadDesc.destinationOffset =
                        static_cast<uint64_t>(objectIndex) * sizeof(Maths::Matrix4);
                    uploadDesc.debugName = "EngineObjectDataSingleFastPath";
                    if (!objectDataSlot->buffer->UpdateData(uploadDesc).Succeeded())
                    {
                        rollbackFastPath();
                        m_renderer.RecordObjectDataFastPath(false);
                        return false;
                    }

                    objectDataSlot->objectDataShadow[objectIndex] = transposed;
                    objectDataSlot->objectDataSourceShadow[objectIndex] = sourceMatrix;
                    objectDataSlot->objectDataSourceValid[objectIndex] = 1u;
#if defined(NLS_ENABLE_TEST_HOOKS)
                    ++m_objectDataWorkCountsForTesting.uploadCount;
#endif
                }

                if (canUseRevisionMetadata)
                {
                    if (objectDataSlot->objectDataRevisionMetadata.size() <= objectIndex)
                    {
                        objectDataSlot->objectDataRevisionMetadata.resize(
                            static_cast<size_t>(objectIndex) + 1u);
                    }
                    objectDataSlot->objectDataRevisionMetadata[objectIndex] = {
                        descriptor.stableSceneIdentity,
                        descriptor.transformRevision,
                        descriptor.objectIndex,
                        objectCount,
                        true
                    };
                }
            }

            m_renderer.RecordObjectDataRevisionReuse(revisionMetadataHit);
            if (!revisionMetadataHit)
                m_renderer.RecordObjectDataRevisionFallback(fallbackReason);
            m_renderer.RecordObjectDataFastPath(true);

            if (RefreshExplicitIndexedObjectBindingSet(*objectDataSlot) == nullptr)
            {
                rollbackFastPath();
                return false;
            }

            objectDataSlot->usedThisFrame = true;
            objectDataSlot->idleFrameCount = 0u;
            captureState.currentDrawUsesIndexedObjectData = true;
            if (preparedObjectIndex != nullptr)
                *preparedObjectIndex = objectIndex;
            return true;
        }

        m_renderer.RecordObjectDataFastPath(false);
    }
    else
    {
        m_renderer.RecordObjectDataFastPath(false);
    }

    SmallObjectSlotBuffer requestedObjectSlots(objectCount);
    if (hasExplicitObjectIndices)
    {
        if (!usesInstanceIndirection)
        {
            const uint32_t baseObjectIndex = descriptor.instanceObjectIndices.front();
            for (uint32_t index = 0u; index < objectCount; ++index)
            {
                if (descriptor.instanceObjectIndices[index] != baseObjectIndex + index)
                    return false;
            }
        }

        for (uint32_t index = 0u; index < objectCount; ++index)
            requestedObjectSlots[index] = descriptor.instanceObjectIndices[index];
    }
    else if (hasExplicitObjectIndex)
    {
        uint32_t requestedRangeEnd = 0u;
        if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(
                descriptor.objectIndex,
                objectCount,
                requestedRangeEnd))
        {
            return false;
        }
        for (uint32_t index = 0u; index < objectCount; ++index)
            requestedObjectSlots[index] = descriptor.objectIndex + index;
    }

    const auto originalNextTransientObjectIndex = objectDataSlot->nextTransientObjectIndex;
    const auto originalNextTransientIndirectionIndex = objectDataSlot->nextTransientIndirectionIndex;
    SmallObjectSlotBuffer newlyClaimedPhysicalSlots;
    SmallObjectSlotBuffer newlyMappedExplicitObjectIndices;
    const auto rollbackPhysicalSlotClaims = [&]()
    {
        for (const auto slot : newlyClaimedPhysicalSlots)
        {
            if (slot < objectDataSlot->objectDataPhysicalSlotOccupancy.size())
                objectDataSlot->objectDataPhysicalSlotOccupancy[slot] = kPhysicalSlotFree;
        }
        for (const auto logicalIndex : newlyMappedExplicitObjectIndices)
            objectDataSlot->explicitObjectPhysicalSlots.erase(logicalIndex);
        objectDataSlot->nextTransientObjectIndex = originalNextTransientObjectIndex;
        objectDataSlot->nextTransientIndirectionIndex = originalNextTransientIndirectionIndex;
    };
    const auto rollbackAndFail = [&]()
    {
        rollbackPhysicalSlotClaims();
        return false;
    };
    const auto ensurePhysicalSlotTracking = [&](const uint32_t physicalSlot)
    {
        if (physicalSlot >= maxObjectDataCount)
            return false;
        if (objectDataSlot->objectDataPhysicalSlotOccupancy.size() <= physicalSlot)
        {
            objectDataSlot->objectDataPhysicalSlotOccupancy.resize(
                static_cast<size_t>(physicalSlot) + 1u,
                kPhysicalSlotFree);
        }
        return true;
    };
    const auto allocateFreePhysicalSlot = [&](const uint8_t owner, uint32_t& outPhysicalSlot)
    {
        auto candidate = objectDataSlot->nextTransientObjectIndex;
        while (candidate < maxObjectDataCount)
        {
            if (!ensurePhysicalSlotTracking(candidate))
                return false;
            if (objectDataSlot->objectDataPhysicalSlotOccupancy[candidate] == kPhysicalSlotFree)
            {
                objectDataSlot->objectDataPhysicalSlotOccupancy[candidate] = owner;
                objectDataSlot->nextTransientObjectIndex = candidate + 1u;
                newlyClaimedPhysicalSlots.push_back(candidate);
                outPhysicalSlot = candidate;
                return true;
            }
            ++candidate;
        }
        return false;
    };
    const auto allocateTransientPhysicalRange = [&](const uint32_t count, SmallObjectSlotBuffer& outSlots)
    {
        auto candidate = objectDataSlot->nextTransientObjectIndex;
        while (candidate < maxObjectDataCount)
        {
            uint32_t lastCandidate = 0u;
            if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(candidate, count, lastCandidate) ||
                !ensurePhysicalSlotTracking(lastCandidate))
            {
                return false;
            }

            auto conflict = candidate;
            while (conflict <= lastCandidate &&
                   objectDataSlot->objectDataPhysicalSlotOccupancy[conflict] == kPhysicalSlotFree)
            {
                ++conflict;
            }
            if (conflict > lastCandidate)
            {
                outSlots.resize(count);
                for (uint32_t index = 0u; index < count; ++index)
                {
                    const auto physicalSlot = candidate + index;
                    objectDataSlot->objectDataPhysicalSlotOccupancy[physicalSlot] = kPhysicalSlotTransient;
                    newlyClaimedPhysicalSlots.push_back(physicalSlot);
                    outSlots[index] = physicalSlot;
                }
                objectDataSlot->nextTransientObjectIndex = lastCandidate + 1u;
                return true;
            }
            candidate = conflict + 1u;
        }
        return false;
    };
    const auto resolveExplicitPhysicalSlot = [&](const uint32_t logicalIndex, uint32_t& outPhysicalSlot)
    {
        const auto setExplicitOwner = [objectDataSlot](const uint32_t physicalSlot, const uint32_t owner)
        {
            if (objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot.size() <= physicalSlot)
            {
                objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot.resize(
                    static_cast<size_t>(physicalSlot) + 1u,
                    NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
            }
            objectDataSlot->objectDataExplicitLogicalOwnerByPhysicalSlot[physicalSlot] = owner;
        };
        if (const auto found = objectDataSlot->explicitObjectPhysicalSlots.find(logicalIndex);
            found != objectDataSlot->explicitObjectPhysicalSlots.end())
        {
            outPhysicalSlot = found->second;
            setExplicitOwner(outPhysicalSlot, logicalIndex);
            return true;
        }
        if (!ensurePhysicalSlotTracking(logicalIndex))
            return false;

        auto physicalSlot = logicalIndex;
        if (objectDataSlot->objectDataPhysicalSlotOccupancy[physicalSlot] != kPhysicalSlotFree)
        {
            if (!allocateFreePhysicalSlot(kPhysicalSlotExplicit, physicalSlot))
            {
                return false;
            }
        }
        else
        {
            objectDataSlot->objectDataPhysicalSlotOccupancy[physicalSlot] = kPhysicalSlotExplicit;
            newlyClaimedPhysicalSlots.push_back(physicalSlot);
        }

        objectDataSlot->explicitObjectPhysicalSlots[logicalIndex] = physicalSlot;
        setExplicitOwner(physicalSlot, logicalIndex);
        newlyMappedExplicitObjectIndices.push_back(logicalIndex);
        outPhysicalSlot = physicalSlot;
        return true;
    };

    SmallObjectSlotBuffer resolvedObjectSlots(objectCount);
    bool resolvedAllObjectSlots = true;
    if (hasExplicitObjectIndices || hasExplicitObjectIndex)
    {
        for (uint32_t index = 0u; index < objectCount && resolvedAllObjectSlots; ++index)
        {
            resolvedAllObjectSlots = resolveExplicitPhysicalSlot(
                requestedObjectSlots[index],
                resolvedObjectSlots[index]);
        }
    }
    else if (usesInstanceIndirection)
    {
        for (uint32_t index = 0u; index < objectCount && resolvedAllObjectSlots; ++index)
        {
            resolvedAllObjectSlots = allocateFreePhysicalSlot(
                kPhysicalSlotTransient,
                resolvedObjectSlots[index]);
        }
    }
    else
    {
        resolvedAllObjectSlots = allocateTransientPhysicalRange(objectCount, resolvedObjectSlots);
    }
    if (!resolvedAllObjectSlots)
    {
        rollbackPhysicalSlotClaims();
        return false;
    }

    if (!usesInstanceIndirection)
    {
        for (uint32_t index = 0u; index < objectCount; ++index)
        {
            if (resolvedObjectSlots[index] != resolvedObjectSlots.front() + index)
            {
                rollbackPhysicalSlotClaims();
                return false;
            }
        }
    }

    objectIndex = resolvedObjectSlots.front();
    uint32_t lastObjectIndex = objectIndex;
    for (const auto resolvedObjectSlot : resolvedObjectSlots)
        lastObjectIndex = std::max(lastObjectIndex, resolvedObjectSlot);

    if (!EnsureObjectDataBufferCapacity(*objectDataSlot, lastObjectIndex) ||
        objectDataSlot->buffer == nullptr)
    {
        return rollbackAndFail();
    }

    uint32_t indirectionBase = 0u;
    if (usesInstanceIndirection)
    {
        indirectionBase = objectDataSlot->nextTransientIndirectionIndex;
        uint32_t lastIndirectionIndex = 0u;
        if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(
                indirectionBase,
                objectCount,
                lastIndirectionIndex) ||
            !EnsureIndirectionBufferCapacity(*objectDataSlot, lastIndirectionIndex) ||
            objectDataSlot->indirectBuffer == nullptr)
        {
            return rollbackAndFail();
        }
        objectDataSlot->nextTransientIndirectionIndex = lastIndirectionIndex + 1u;

        bool indirectionChanged =
            objectDataSlot->objectDataIndirectShadow.size() <
            static_cast<size_t>(lastIndirectionIndex) + 1u;
        if (!indirectionChanged)
        {
            indirectionChanged = !std::equal(
                resolvedObjectSlots.begin(),
                resolvedObjectSlots.end(),
                objectDataSlot->objectDataIndirectShadow.begin() + indirectionBase);
        }
        if (indirectionChanged)
        {
            NLS::Render::RHI::RHIBufferUploadDesc indirectionUpload;
            indirectionUpload.data = resolvedObjectSlots.data();
            indirectionUpload.dataSize = static_cast<size_t>(objectCount) * sizeof(uint32_t);
            indirectionUpload.destinationOffset =
                static_cast<uint64_t>(indirectionBase) * sizeof(uint32_t);
            indirectionUpload.debugName = "EngineInstanceObjectIndexUpdate";
            if (!objectDataSlot->indirectBuffer->UpdateData(indirectionUpload).Succeeded())
                return rollbackAndFail();
            if (objectDataSlot->objectDataIndirectShadow.size() <
                static_cast<size_t>(lastIndirectionIndex) + 1u)
            {
                objectDataSlot->objectDataIndirectShadow.resize(
                    static_cast<size_t>(lastIndirectionIndex) + 1u,
                    0u);
            }
            std::copy(
                resolvedObjectSlots.begin(),
                resolvedObjectSlots.end(),
                objectDataSlot->objectDataIndirectShadow.begin() + indirectionBase);
        }
    }

    const auto requiredShadowSize = static_cast<size_t>(lastObjectIndex) + 1u;
    const auto metadataSlot = resolvedObjectSlots.front();
    const bool canUseRevisionMetadata =
        !hasExplicitObjectIndices &&
        descriptor.HasCompleteTrustedSingleObjectDataRevision() &&
        descriptor.objectCount == objectCount;
    bool revisionMetadataHit = false;
    NLS::Render::Data::ObjectDataRevisionFallbackReason fallbackReason =
        NLS::Render::Data::ObjectDataRevisionFallbackReason::Descriptor;
    if (canUseRevisionMetadata)
    {
        if (objectDataSlot->objectDataRevisionMetadata.size() <= metadataSlot)
        {
            fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::MetadataUnavailable;
        }
        else
        {
            const auto& metadata = objectDataSlot->objectDataRevisionMetadata[metadataSlot];
            if (!metadata.valid)
            {
                const bool metadataIsUninitialized =
                    metadata.stableSceneIdentity == NLS::Render::Data::StaticDrawSceneIdentity {} &&
                    metadata.transformRevision == 0u &&
                    metadata.objectIndex == NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex &&
                    metadata.objectCount == 0u;
                fallbackReason = metadataIsUninitialized
                    ? NLS::Render::Data::ObjectDataRevisionFallbackReason::MetadataUninitialized
                    : NLS::Render::Data::ObjectDataRevisionFallbackReason::MetadataInvalid;
            }
            else if (metadata.stableSceneIdentity != descriptor.stableSceneIdentity)
            {
                fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::StableIdentityMismatch;
            }
            else if (metadata.transformRevision != descriptor.transformRevision)
            {
                fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::TransformMismatch;
            }
            else if (metadata.objectIndex != descriptor.objectIndex)
            {
                fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::ObjectIndexMismatch;
            }
            else if (metadata.objectCount != objectCount)
            {
                fallbackReason = NLS::Render::Data::ObjectDataRevisionFallbackReason::ObjectCountMismatch;
            }
            else
            {
                revisionMetadataHit = true;
            }
        }
    }
    m_renderer.RecordObjectDataRevisionReuse(revisionMetadataHit);
    if (!revisionMetadataHit)
        m_renderer.RecordObjectDataRevisionFallback(fallbackReason);

    if (!revisionMetadataHit)
    {
        for (const auto resolvedObjectSlot : resolvedObjectSlots)
        {
            if (resolvedObjectSlot < objectDataSlot->objectDataRevisionMetadata.size())
                objectDataSlot->objectDataRevisionMetadata[resolvedObjectSlot].valid = false;
        }

        const auto uploadChangedSlot = [&](const uint32_t sourceIndex, const uint32_t targetSlot)
        {
            if (objectDataSlot->objectDataShadow.size() <= targetSlot)
                objectDataSlot->objectDataShadow.resize(static_cast<size_t>(targetSlot) + 1u, Maths::Matrix4::Identity);
            if (objectDataSlot->objectDataSourceShadow.size() <= targetSlot)
                objectDataSlot->objectDataSourceShadow.resize(static_cast<size_t>(targetSlot) + 1u, Maths::Matrix4::Identity);
            if (objectDataSlot->objectDataSourceValid.size() <= targetSlot)
                objectDataSlot->objectDataSourceValid.resize(static_cast<size_t>(targetSlot) + 1u, uint8_t {0u});

            const auto& sourceMatrix = sourceMatrixData[sourceIndex];
#if defined(NLS_ENABLE_TEST_HOOKS)
        ++m_objectDataWorkCountsForTesting.memcmpCount;
#endif
        const bool sourceChanged =
            objectDataSlot->objectDataSourceValid[targetSlot] == 0u ||
            std::memcmp(
                    &objectDataSlot->objectDataSourceShadow[targetSlot],
                    &sourceMatrix,
                    sizeof(Maths::Matrix4)) != 0;
        if (!sourceChanged)
            return true;

#if defined(NLS_ENABLE_TEST_HOOKS)
        ++m_objectDataWorkCountsForTesting.transposeCount;
#endif
        const auto transposed = Maths::Matrix4::Transpose(sourceMatrix);
            NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
            uploadDesc.data = &transposed;
            uploadDesc.dataSize = sizeof(Maths::Matrix4);
            uploadDesc.destinationOffset =
                static_cast<uint64_t>(targetSlot) * sizeof(Maths::Matrix4);
            uploadDesc.debugName = "EngineObjectDataUpdate";
            if (!objectDataSlot->buffer->UpdateData(uploadDesc).Succeeded())
                return false;

            objectDataSlot->objectDataShadow[targetSlot] = transposed;
            objectDataSlot->objectDataSourceShadow[targetSlot] = sourceMatrix;
            objectDataSlot->objectDataSourceValid[targetSlot] = 1u;
#if defined(NLS_ENABLE_TEST_HOOKS)
            ++m_objectDataWorkCountsForTesting.uploadCount;
#endif
            return true;
        };

        bool contiguousObjectSlots = true;
        for (uint32_t index = 0u; index < objectCount; ++index)
        {
            if (resolvedObjectSlots[index] != objectIndex + index)
            {
                contiguousObjectSlots = false;
                break;
            }
        }
        bool allUploaded = true;
        if (contiguousObjectSlots)
        {
            bool objectDataChanged =
                objectDataSlot->objectDataShadow.size() < requiredShadowSize ||
                objectDataSlot->objectDataSourceShadow.size() < requiredShadowSize ||
                objectDataSlot->objectDataSourceValid.size() < requiredShadowSize;
            if (!objectDataChanged)
            {
#if defined(NLS_ENABLE_TEST_HOOKS)
                ++m_objectDataWorkCountsForTesting.validityScanCount;
#endif
                const auto validBegin = objectDataSlot->objectDataSourceValid.begin() + objectIndex;
                const auto validEnd = validBegin + objectCount;
                const bool hasInvalidSource = std::find(validBegin, validEnd, uint8_t {0u}) != validEnd;
                if (hasInvalidSource)
                {
                    objectDataChanged = true;
                }
                else
                {
#if defined(NLS_ENABLE_TEST_HOOKS)
                    ++m_objectDataWorkCountsForTesting.memcmpCount;
#endif
                    objectDataChanged =
                        std::memcmp(
                            objectDataSlot->objectDataSourceShadow.data() + objectIndex,
                            sourceMatrixData,
                            static_cast<size_t>(objectCount) * sizeof(Maths::Matrix4)) != 0;
                }
            }
            if (objectDataChanged)
            {
                m_objectDataTransposeScratch.resize(objectCount);
#if defined(NLS_ENABLE_TEST_HOOKS)
                ++m_objectDataWorkCountsForTesting.transposeCount;
#endif
                std::transform(
                    sourceMatrixData,
                    sourceMatrixData + objectCount,
                    m_objectDataTransposeScratch.begin(),
                    [](const Maths::Matrix4& matrix)
                    {
                        return Maths::Matrix4::Transpose(matrix);
                    });
                NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
                uploadDesc.data = m_objectDataTransposeScratch.data();
                uploadDesc.dataSize = static_cast<size_t>(objectCount) * sizeof(Maths::Matrix4);
                uploadDesc.destinationOffset = static_cast<uint64_t>(objectIndex) * sizeof(Maths::Matrix4);
                uploadDesc.debugName = "EngineObjectDataUpdate";
                allUploaded = objectDataSlot->buffer->UpdateData(uploadDesc).Succeeded();
                if (allUploaded)
                {
                    if (objectDataSlot->objectDataShadow.size() < requiredShadowSize)
                        objectDataSlot->objectDataShadow.resize(requiredShadowSize, Maths::Matrix4::Identity);
                    if (objectDataSlot->objectDataSourceShadow.size() < requiredShadowSize)
                        objectDataSlot->objectDataSourceShadow.resize(requiredShadowSize, Maths::Matrix4::Identity);
                    if (objectDataSlot->objectDataSourceValid.size() < requiredShadowSize)
                        objectDataSlot->objectDataSourceValid.resize(requiredShadowSize, uint8_t {0u});
                    std::copy(
                        m_objectDataTransposeScratch.begin(),
                        m_objectDataTransposeScratch.end(),
                        objectDataSlot->objectDataShadow.begin() + objectIndex);
                    std::copy(
                        sourceMatrixData,
                        sourceMatrixData + objectCount,
                        objectDataSlot->objectDataSourceShadow.begin() + objectIndex);
                    std::fill(
                        objectDataSlot->objectDataSourceValid.begin() + objectIndex,
                        objectDataSlot->objectDataSourceValid.begin() + objectIndex + objectCount,
                        uint8_t {1u});
#if defined(NLS_ENABLE_TEST_HOOKS)
                    ++m_objectDataWorkCountsForTesting.uploadCount;
#endif
                }
            }
        }
        else
        {
            for (uint32_t index = 0u; index < objectCount && allUploaded; ++index)
                allUploaded = uploadChangedSlot(index, resolvedObjectSlots[index]);
        }
        if (!allUploaded)
            return rollbackAndFail();

        if (canUseRevisionMetadata)
        {
            if (objectDataSlot->objectDataRevisionMetadata.size() < requiredShadowSize)
                objectDataSlot->objectDataRevisionMetadata.resize(requiredShadowSize);
            objectDataSlot->objectDataRevisionMetadata[metadataSlot] = {
                descriptor.stableSceneIdentity,
                descriptor.transformRevision,
                descriptor.objectIndex,
                objectCount,
                true
            };
        }
    }

    if (RefreshExplicitIndexedObjectBindingSet(*objectDataSlot) == nullptr)
        return rollbackAndFail();

    objectDataSlot->usedThisFrame = true;
    objectDataSlot->idleFrameCount = 0u;
    captureState.currentDrawUsesIndexedObjectData = true;
    if (preparedObjectIndex != nullptr)
        *preparedObjectIndex = usesInstanceIndirection ? indirectionBase : objectIndex;
    return true;
}

bool EngineFrameObjectBindingProvider::PreparedShaderRequiresIndexedObjectData() const
{
    const auto* shader = GetPreparedShader();
    if (shader == nullptr)
        return false;

    return ShaderRequiresIndexedObjectData(*shader);
}

bool EngineFrameObjectBindingProvider::ShaderRequiresIndexedObjectData(
    const NLS::Render::Resources::Shader& shader) const
{
    const auto generation = shader.GetGeneration();
    const auto shaderInstanceId = shader.GetInstanceId();
    if (const auto found = m_indexedObjectDataShaderSupportCache.find(shaderInstanceId);
        found != m_indexedObjectDataShaderSupportCache.end() &&
        found->second.first == generation)
    {
        return found->second.second;
    }

    const bool supportsIndexedObjectData = ShaderSupportsIndexedObjectData(shader);
    if (m_indexedObjectDataShaderSupportCache.size() >= kIndexedObjectDataShaderSupportCacheMaxEntries)
        m_indexedObjectDataShaderSupportCache.clear();
    m_indexedObjectDataShaderSupportCache[shaderInstanceId] = { generation, supportsIndexedObjectData };
    ++m_indexedObjectDataShaderSupportQueryCount;
    return supportsIndexedObjectData;
}

bool EngineFrameObjectBindingProvider::PreparedShaderUsesInstanceObjectIndirection() const
{
    const auto* shader = GetPreparedShader();
    if (shader == nullptr)
        return false;

    return ShaderUsesInstanceObjectIndirection(*shader);
}

bool EngineFrameObjectBindingProvider::ShaderUsesInstanceObjectIndirection(
    const NLS::Render::Resources::Shader& shader) const
{
    const auto generation = shader.GetGeneration();
    const auto shaderInstanceId = shader.GetInstanceId();
    if (const auto found = m_instanceObjectIndirectionShaderSupportCache.find(shaderInstanceId);
        found != m_instanceObjectIndirectionShaderSupportCache.end() &&
        found->second.first == generation)
    {
        return found->second.second;
    }

    const bool usesIndirection = NLS::Render::Resources::ShaderUsesInstanceObjectIndirection(shader);
    if (m_instanceObjectIndirectionShaderSupportCache.size() >= kIndexedObjectDataShaderSupportCacheMaxEntries)
        m_instanceObjectIndirectionShaderSupportCache.clear();
    m_instanceObjectIndirectionShaderSupportCache[shaderInstanceId] = { generation, usesIndirection };
    return usesIndirection;
}

void EngineFrameObjectBindingProvider::OnDeferredReset()
{
    if (auto* captureState = TryGetActiveCaptureState())
    {
        captureState->deferredFrameBindingSet.reset();
        captureState->deferredLegacyObjectBindingSet.reset();
    }
    m_fallbackCaptureState.deferredFrameBindingSet.reset();
    m_fallbackCaptureState.deferredLegacyObjectBindingSet.reset();
}
}
