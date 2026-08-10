#include "Assets/ThumbnailPreviewProxyPool.h"

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Engine/GameObject.h"
#include "Engine/SceneSystem/Scene.h"
#include "Math/Quaternion.h"

namespace NLS::Editor::Assets
{
ThumbnailPreviewProxyPool::Lease::Lease(
    std::shared_ptr<State> state,
    const size_t slot,
    const uint64_t serial) :
    m_state(std::move(state)),
    m_slot(slot),
    m_serial(serial)
{
}

ThumbnailPreviewProxyPool::Lease::Lease(Lease&& other) noexcept :
    m_state(std::move(other.m_state)),
    m_slot(other.m_slot),
    m_serial(other.m_serial)
{
    other.m_slot = 0u;
    other.m_serial = 0u;
}

ThumbnailPreviewProxyPool::Lease& ThumbnailPreviewProxyPool::Lease::operator=(Lease&& other) noexcept
{
    if (this == &other)
        return *this;
    Reset();
    m_state = std::move(other.m_state);
    m_slot = other.m_slot;
    m_serial = other.m_serial;
    other.m_slot = 0u;
    other.m_serial = 0u;
    return *this;
}

ThumbnailPreviewProxyPool::Lease::~Lease()
{
    Reset();
}

NLS::Engine::GameObject* ThumbnailPreviewProxyPool::Lease::Get() const
{
    if (!m_state || m_slot >= m_state->slots.size())
        return nullptr;
    const auto& slot = m_state->slots[m_slot];
    return slot.leased && slot.serial == m_serial ? slot.object : nullptr;
}

uint64_t ThumbnailPreviewProxyPool::Lease::Serial() const
{
    return m_serial;
}

ThumbnailPreviewProxyPool::Lease::operator bool() const
{
    return Get() != nullptr;
}

void ThumbnailPreviewProxyPool::Lease::Reset()
{
    if (m_state)
        m_state->Release(m_slot, m_serial);
    m_state.reset();
    m_slot = 0u;
    m_serial = 0u;
}

ThumbnailPreviewProxyPool::ThumbnailPreviewProxyPool(
    NLS::Engine::SceneSystem::Scene& scene,
    const size_t maxObjects) :
    m_state(std::make_shared<State>())
{
    m_state->scene = &scene;
    m_state->maxObjects = maxObjects;
}

ThumbnailPreviewProxyPool::~ThumbnailPreviewProxyPool()
{
    if (m_state != nullptr)
        m_state->DetachScene();
}

std::optional<ThumbnailPreviewProxyPool::Lease> ThumbnailPreviewProxyPool::Acquire(
    const std::string_view debugName,
    const bool allowReuse)
{
    if (!m_state || m_state->scene == nullptr || m_state->maxObjects == 0u)
        return std::nullopt;

    for (size_t index = 0u; index < m_state->slots.size(); ++index)
    {
        auto& slot = m_state->slots[index];
        if (slot.leased || slot.object == nullptr || !slot.object->IsAlive() ||
            !allowReuse || !slot.pooled)
            continue;

        slot.leased = true;
        ++m_state->activeLeaseCount;
        ++m_state->reuseHitCount;
        ++slot.serial;
        if (slot.serial == 0u)
            slot.serial = 1u;
        slot.object->SetName(std::string(debugName));
        slot.object->SetActive(true);
        return Lease(m_state, index, slot.serial);
    }

    // The diagnostic legacy path deliberately does not reuse a live proxy.
    // Recycle an empty slot, if possible, so repeated A/B runs remain bounded.
    for (size_t index = 0u; index < m_state->slots.size(); ++index)
    {
        auto& slot = m_state->slots[index];
        if (slot.leased)
            continue;
        if (slot.object != nullptr && slot.object->IsAlive())
        {
            (void)m_state->scene->DestroyGameObject(*slot.object);
            m_state->scene->CollectGarbages();
        }
        slot.object = nullptr;
        auto& object = m_state->scene->CreateEditorTransientGameObject(std::string(debugName));
        object.AddComponent<NLS::Engine::Components::MeshFilter>();
        object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        object.SetActive(true);
        slot.object = &object;
        slot.leased = true;
        slot.pooled = allowReuse;
        ++m_state->activeLeaseCount;
        ++m_state->allocationCount;
        ++slot.serial;
        if (slot.serial == 0u)
            slot.serial = 1u;
        return Lease(m_state, index, slot.serial);
    }

    if (m_state->slots.size() >= m_state->maxObjects)
        return std::nullopt;

    auto& object = m_state->scene->CreateEditorTransientGameObject(std::string(debugName));
    object.AddComponent<NLS::Engine::Components::MeshFilter>();
    object.AddComponent<NLS::Engine::Components::MeshRenderer>();
    object.SetActive(true);

    Slot slot;
    slot.object = &object;
    slot.serial = 1u;
    slot.leased = true;
    slot.pooled = allowReuse;
    m_state->slots.push_back(slot);
    ++m_state->activeLeaseCount;
    ++m_state->allocationCount;
    return Lease(m_state, m_state->slots.size() - 1u, slot.serial);
}

size_t ThumbnailPreviewProxyPool::GetObjectCount() const
{
    return m_state != nullptr ? m_state->slots.size() : 0u;
}

size_t ThumbnailPreviewProxyPool::GetActiveLeaseCount() const
{
    return m_state != nullptr ? m_state->activeLeaseCount : 0u;
}

size_t ThumbnailPreviewProxyPool::GetReuseHitCount() const
{
    return m_state != nullptr ? m_state->reuseHitCount : 0u;
}

size_t ThumbnailPreviewProxyPool::GetAllocationCount() const
{
    return m_state != nullptr ? m_state->allocationCount : 0u;
}

ThumbnailPreviewProxyPool::State::~State()
{
    if (scene == nullptr)
        return;
    for (auto& slot : slots)
    {
        if (slot.object != nullptr && slot.object->IsAlive())
            (void)scene->DestroyGameObject(*slot.object);
    }
    scene->CollectGarbages();
}

void ThumbnailPreviewProxyPool::State::DetachScene()
{
    auto* detachedScene = scene;
    if (detachedScene != nullptr)
    {
        for (auto& slot : slots)
        {
            // Unleased objects are still owned by this pool and can be
            // removed while the scene is known to be alive.  A leased object
            // remains owned by the scene until its normal scene teardown;
            // clearing the handle below prevents a late lease from touching
            // that object through a dangling pointer.
            if (!slot.leased && slot.object != nullptr && slot.object->IsAlive())
            {
                ResetSlot(slot);
                (void)detachedScene->DestroyGameObject(*slot.object);
            }
            slot.object = nullptr;
            slot.leased = false;
        }
        detachedScene->CollectGarbages();
    }
    else
    {
        for (auto& slot : slots)
        {
            slot.object = nullptr;
            slot.leased = false;
        }
    }

    activeLeaseCount = 0u;
    scene = nullptr;
}

void ThumbnailPreviewProxyPool::State::Release(
    const size_t slotIndex,
    const uint64_t expectedSerial)
{
    if (slotIndex >= slots.size())
        return;
    auto& slot = slots[slotIndex];
    if (!slot.leased || slot.serial != expectedSerial)
        return;

    if (scene != nullptr)
    {
        ResetSlot(slot);
        if (!slot.pooled && slot.object != nullptr && slot.object->IsAlive())
        {
            (void)scene->DestroyGameObject(*slot.object);
            scene->CollectGarbages();
            slot.object = nullptr;
        }
    }
    else
        slot.object = nullptr;
    slot.leased = false;
    if (activeLeaseCount > 0u)
        --activeLeaseCount;
}

void ThumbnailPreviewProxyPool::State::ResetSlot(Slot& slot)
{
    if (slot.object == nullptr || !slot.object->IsAlive())
        return;

    slot.object->SetActive(false);
    if (auto* transform = slot.object->GetTransform())
    {
        transform->SetLocalPosition({0.0f, 0.0f, 0.0f});
        transform->SetLocalRotation(NLS::Maths::Quaternion::Identity);
        transform->SetLocalScale({1.0f, 1.0f, 1.0f});
    }
    if (auto* filter = slot.object->GetComponent<NLS::Engine::Components::MeshFilter>())
        filter->SetMesh(nullptr);
    if (auto* renderer = slot.object->GetComponent<NLS::Engine::Components::MeshRenderer>())
    {
        renderer->RemoveAllMaterials();
        renderer->SetTransientRenderingSuppressed(false);
    }
}
}
