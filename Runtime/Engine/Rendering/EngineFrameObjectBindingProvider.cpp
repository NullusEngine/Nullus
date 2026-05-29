#include <Debug/Logger.h>
#include <algorithm>
#include <array>
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

    bool ShouldLogFrameConstantDiagnostics(const Render::Context::Driver& driver)
    {
        return Render::Context::DriverRendererAccess::GetDiagnosticsSettings(driver).logRenderDrawPath;
    }
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

void EngineFrameObjectBindingProvider::PrepareRenderScenePackage(
    const NLS::Render::Context::FrameSnapshot&,
    NLS::Render::Context::RenderScenePackage& package) const
{
    package.frameDataReady = true;
    package.objectDataReady = true;
}

void EngineFrameObjectBindingProvider::OnBeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor)
{
    ReleaseStalePreparedObjectDataSlotReservation();
    RetireIdleObjectDataSlots();
    m_preparedFrameHasObjectDataSlot = false;
    m_preparedFrameObjectDataSlotReserved = false;
    m_preparedFrameObjectDataSlotUnavailable = false;
    for (auto& slot : m_objectDataSlots)
    {
        slot.objectDataShadow.clear();
        slot.usedThisFrame = false;
    }

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

    if (ShouldLogFrameConstantDiagnostics(m_renderer.GetDriver()))
    {
        const auto& cameraPos = frameDescriptor.camera->GetPosition();
        const auto& clearColor = frameDescriptor.camera->GetClearColor();
        NLS_LOG_INFO(
            "[FrameConstants] renderSize=" +
            std::to_string(frameDescriptor.renderWidth) + "x" + std::to_string(frameDescriptor.renderHeight) +
            " cameraPos=(" + std::to_string(cameraPos.x) + "," + std::to_string(cameraPos.y) + "," + std::to_string(cameraPos.z) + ")" +
            " clearColor=(" + std::to_string(clearColor.x) + "," + std::to_string(clearColor.y) + "," + std::to_string(clearColor.z) + ")" +
            " vpNoTrans_row0=(" + std::to_string(viewProjectionNoTranslation.data[0]) + "," + std::to_string(viewProjectionNoTranslation.data[1]) + "," + std::to_string(viewProjectionNoTranslation.data[2]) + "," + std::to_string(viewProjectionNoTranslation.data[3]) + ")" +
            " vpNoTrans_row1=(" + std::to_string(viewProjectionNoTranslation.data[4]) + "," + std::to_string(viewProjectionNoTranslation.data[5]) + "," + std::to_string(viewProjectionNoTranslation.data[6]) + "," + std::to_string(viewProjectionNoTranslation.data[7]) + ")");
    }

    m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjection), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(frameDescriptor.camera->GetPosition(), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(elapsedTime.count(), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjectionNoTranslation), std::ref(hlslFrameOffset));
    m_explicitFrameBindingSetDirty = true;
}

void EngineFrameObjectBindingProvider::OnEndFrame()
{
    m_useAltObjectBuffer = !m_useAltObjectBuffer;

    OnDeferredReset();
}

bool EngineFrameObjectBindingProvider::OnTryReservePreparedFrameResources()
{
    if (m_preparedFrameObjectDataSlotUnavailable)
        return false;

    const auto slotIndex = ResolveActiveObjectDataSlotIndex();
    if (!slotIndex.has_value())
    {
        m_preparedFrameObjectDataSlotUnavailable = true;
        return false;
    }

    return true;
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
    m_currentDrawUsesIndexedObjectData = false;
    m_currentDrawRequiresIndexedObjectData = DrawableRequiresIndexedObjectData(drawable);
    m_currentDrawPrepared = true;
    m_currentDrawObjectIndex = NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;

    NLS::Render::Data::DrawableObjectDescriptor descriptor;
    if (drawable.TryGetDescriptor<NLS::Render::Data::DrawableObjectDescriptor>(descriptor))
    {
        m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
        m_engineBuffer->SetSubData(
            descriptor.userMatrix,
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Vector3) +
            sizeof(float));

        const bool hasExplicitObjectIndex =
            descriptor.objectIndex != NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
        if ((hasExplicitObjectIndex || m_currentDrawRequiresIndexedObjectData) &&
            TryPrepareIndexedObjectData(drawable, &m_currentDrawObjectIndex))
        {
            return true;
        }

        if (m_currentDrawRequiresIndexedObjectData)
        {
            m_currentDrawPrepared = false;
            m_explicitObjectBindingSet.reset();
            return false;
        }

        auto& writeBuffer = m_useAltObjectBuffer ? *m_hlslObjectBufferAlt : *m_hlslObjectBuffer;
        writeBuffer.SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
        m_explicitObjectBindingSetDirty = true;
    }
    else if (m_currentDrawRequiresIndexedObjectData)
    {
        m_currentDrawPrepared = false;
        m_explicitObjectBindingSet.reset();
        return false;
    }

    return m_currentDrawPrepared;
}

void EngineFrameObjectBindingProvider::OnPrepareExplicitDraw(
    NLS::Render::RHI::RHICommandBuffer& commandBuffer,
    PipelineState&,
    const NLS::Render::Entities::Drawable& drawable)
{
    RefreshExplicitFrameBindingSet();
    if (m_currentDrawUsesIndexedObjectData)
    {
        auto* slot = ResolveActiveObjectDataSlot();
        m_explicitObjectBindingSet = slot != nullptr ? RefreshExplicitIndexedObjectBindingSet(*slot) : nullptr;
    }
    else if (!m_currentDrawPrepared)
    {
        m_explicitObjectBindingSet.reset();
    }
    else
    {
        RefreshExplicitObjectBindingSet();
    }

    if (m_explicitFrameBindingSet != nullptr)
        commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet, m_explicitFrameBindingSet);
    if (m_explicitObjectBindingSet != nullptr)
        commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet, m_explicitObjectBindingSet);
    if (m_currentDrawUsesIndexedObjectData)
    {
        if (m_currentDrawObjectIndex != NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex)
        {
            commandBuffer.PushConstants(
                NLS::Render::RHI::ShaderStageMask::Vertex,
                0u,
                sizeof(m_currentDrawObjectIndex),
                &m_currentDrawObjectIndex);
        }
    }
}

bool EngineFrameObjectBindingProvider::OnCapturePreparedBindingSets(
    PipelineState&,
    const NLS::Render::Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    if (!m_currentDrawPrepared)
        return false;

    RefreshExplicitFrameBindingSet();
    if (m_currentDrawUsesIndexedObjectData)
    {
        auto* slot = ResolveActiveObjectDataSlot();
        m_explicitObjectBindingSet = slot != nullptr ? RefreshExplicitIndexedObjectBindingSet(*slot) : nullptr;
    }
    else if (!m_currentDrawPrepared)
    {
        m_explicitObjectBindingSet.reset();
    }
    else
    {
        RefreshExplicitObjectBindingSet();
    }
    outBindings.frameBindingSet = m_explicitFrameBindingSet;
    outBindings.objectBindingSet = m_explicitObjectBindingSet;
    outBindings.usesObjectIndex = false;
    outBindings.objectIndex = NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
    if (m_currentDrawUsesIndexedObjectData)
    {
        outBindings.objectIndex = m_currentDrawObjectIndex;
        outBindings.usesObjectIndex = m_currentDrawObjectIndex !=
            NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
    }
    return outBindings.frameBindingSet != nullptr || outBindings.objectBindingSet != nullptr;
}

void EngineFrameObjectBindingProvider::RefreshExplicitFrameBindingSet()
{
    if (!m_explicitFrameBindingSetDirty)
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
    auto newBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(*m_hlslFrameBuffer, bindingDesc);

    m_deferredFrameBindingSet = std::move(m_explicitFrameBindingSet);
    m_explicitFrameBindingSet = std::move(newBindingSet);
    m_explicitFrameBindingSetDirty = false;
}

void EngineFrameObjectBindingProvider::RefreshExplicitObjectBindingSet()
{
    if (!m_explicitObjectBindingSetDirty)
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

    auto& writeBuffer = m_useAltObjectBuffer ? *m_hlslObjectBufferAlt : *m_hlslObjectBuffer;
    auto newBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(writeBuffer, bindingDesc);

    m_deferredObjectBindingSet = std::move(m_explicitObjectBindingSet);
    m_explicitObjectBindingSet = std::move(newBindingSet);
    m_explicitObjectBindingSetDirty = false;
    m_currentDrawUsesIndexedObjectData = false;
}

std::optional<size_t> EngineFrameObjectBindingProvider::ResolveActiveObjectDataSlotIndex()
{
    if (m_preparedFrameObjectDataSlotUnavailable)
        return std::nullopt;

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
        const auto reusableSlotIndex =
            NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndex(m_renderer.GetDriver());
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
    slot.buffer.reset();
    slot.bindingSet.reset();
    slot.deferredBindingSet.reset();
    slot.objectDataShadow = {};
    slot.capacity = 0u;
    slot.idleFrameCount = 0u;
    slot.bindingSetDirty = true;
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

bool EngineFrameObjectBindingProvider::EnsureObjectDataBufferCapacity(
    ObjectDataFrameSlot& slot,
    const uint32_t objectIndex)
{
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

    auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_renderer.GetDriver());
    if (device == nullptr)
        return false;

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
    }

    slot.buffer = std::move(buffer);
    slot.capacity = newCapacity;
    slot.bindingSetDirty = true;
    return true;
}

std::shared_ptr<NLS::Render::RHI::RHIBindingSet> EngineFrameObjectBindingProvider::RefreshExplicitIndexedObjectBindingSet(
    ObjectDataFrameSlot& slot)
{
    if (!slot.bindingSetDirty)
        return slot.bindingSet;
    if (slot.buffer == nullptr)
        return nullptr;

    auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_renderer.GetDriver());
    if (device == nullptr)
        return nullptr;

    if (m_objectDataBindingLayout == nullptr)
    {
        NLS::Render::RHI::RHIBindingLayoutDesc layoutDesc;
        layoutDesc.debugName = "EngineObjectBindingLayout";
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
        m_objectDataBindingLayout = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingLayout(
            m_renderer.GetDriver(),
            layoutDesc);
    }

    if (m_objectDataBindingLayout == nullptr)
        return nullptr;

    NLS::Render::RHI::RHIBindingSetDesc bindingSetDesc;
    bindingSetDesc.layout = m_objectDataBindingLayout;
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

    auto newBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        m_renderer.GetDriver(),
        bindingSetDesc,
        NLS::Render::RHI::DescriptorAllocationLifetime::Persistent);
    if (newBindingSet == nullptr)
        return nullptr;

    slot.deferredBindingSet = std::move(slot.bindingSet);
    slot.bindingSet = std::move(newBindingSet);
    slot.bindingSetDirty = false;
    return slot.bindingSet;
}

bool EngineFrameObjectBindingProvider::TryPrepareIndexedObjectData(
    const NLS::Render::Entities::Drawable& drawable,
    uint32_t* preparedObjectIndex)
{
    NLS::Render::Data::DrawableObjectDescriptor descriptor;
    if (!drawable.TryGetDescriptor<NLS::Render::Data::DrawableObjectDescriptor>(descriptor))
        return false;
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
    if (drawable.material != nullptr &&
        drawable.material->GetShader() != nullptr &&
        !ShaderSupportsIndexedObjectData(*drawable.material->GetShader()))
    {
        return false;
    }

    auto* objectDataSlot = ResolveActiveObjectDataSlot();
    if (objectDataSlot == nullptr)
        return false;

    if (objectIndex == NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex)
    {
        if (objectDataSlot->objectDataShadow.size() > (std::numeric_limits<uint32_t>::max)())
            return false;
        objectIndex = static_cast<uint32_t>(objectDataSlot->objectDataShadow.size());
    }

    uint32_t lastObjectIndex = 0u;
    if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(
        objectIndex,
        objectCount,
        lastObjectIndex))
    {
        return false;
    }

    std::array<Maths::Matrix4, 1u> singleShaderMatrix;
    std::vector<Maths::Matrix4> shaderMatrices;
    const Maths::Matrix4* shaderMatrixData = nullptr;
    if (descriptor.instanceModelMatrices.empty())
    {
        if (objectCount != 1u)
            return false;

        singleShaderMatrix[0] = Maths::Matrix4::Transpose(descriptor.modelMatrix);
        shaderMatrixData = singleShaderMatrix.data();
    }
    else
    {
        if (descriptor.instanceModelMatrices.size() < objectCount)
            return false;

        shaderMatrices.reserve(objectCount);
        for (uint32_t matrixIndex = 0u; matrixIndex < objectCount; ++matrixIndex)
            shaderMatrices.push_back(Maths::Matrix4::Transpose(descriptor.instanceModelMatrices[matrixIndex]));
        shaderMatrixData = shaderMatrices.data();
    }

    if (!EnsureObjectDataBufferCapacity(*objectDataSlot, lastObjectIndex) ||
        objectDataSlot->buffer == nullptr)
        return false;

    NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
    uploadDesc.data = shaderMatrixData;
    uploadDesc.dataSize = static_cast<size_t>(objectCount) * sizeof(Maths::Matrix4);
    uploadDesc.destinationOffset = static_cast<uint64_t>(objectIndex) * sizeof(Maths::Matrix4);
    uploadDesc.debugName = "EngineObjectDataUpdate";
    const auto updateResult = objectDataSlot->buffer->UpdateData(uploadDesc);
    if (!updateResult.Succeeded())
        return false;
    const auto requiredShadowSize = static_cast<size_t>(objectIndex) + objectCount;
    if (objectDataSlot->objectDataShadow.size() < requiredShadowSize)
        objectDataSlot->objectDataShadow.resize(requiredShadowSize, Maths::Matrix4::Identity);
    std::copy(
        shaderMatrixData,
        shaderMatrixData + objectCount,
        objectDataSlot->objectDataShadow.begin() + objectIndex);

    if (RefreshExplicitIndexedObjectBindingSet(*objectDataSlot) == nullptr)
        return false;

    objectDataSlot->usedThisFrame = true;
    objectDataSlot->idleFrameCount = 0u;
    m_currentDrawUsesIndexedObjectData = true;
    if (preparedObjectIndex != nullptr)
        *preparedObjectIndex = objectIndex;
    return true;
}

bool EngineFrameObjectBindingProvider::DrawableRequiresIndexedObjectData(
    const NLS::Render::Entities::Drawable& drawable) const
{
    if (drawable.material == nullptr || drawable.material->GetShader() == nullptr)
        return false;

    return ShaderSupportsIndexedObjectData(*drawable.material->GetShader());
}

void EngineFrameObjectBindingProvider::OnDeferredReset()
{
    m_deferredFrameBindingSet.reset();
    m_deferredObjectBindingSet.reset();
    for (auto& slot : m_objectDataSlots)
        slot.deferredBindingSet.reset();
}
}
