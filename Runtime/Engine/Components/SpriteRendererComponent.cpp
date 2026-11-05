#include <Profiling/Profiler.h>
#include "Components/SpriteRendererComponent.h"
#include <cmath>

namespace NLS::Engine::Components
{
void SpriteRendererComponent::Changed() { ++m_revision; MarkRenderStateChanged(); }
void SpriteRendererComponent::SetDrawMode(ESpriteDrawMode value)
{
    if(value!=ESpriteDrawMode::Simple&&value!=ESpriteDrawMode::Sliced&&value!=ESpriteDrawMode::Tiled)return;
    if(drawMode!=value){drawMode=value;Changed();}
}
void SpriteRendererComponent::SetSize(Maths::Vector2 value)
{
    if(!std::isfinite(value.x)||!std::isfinite(value.y)||value.x<=0||value.y<=0||value.x>1000000||value.y>1000000)return;
    if(size.x!=value.x||size.y!=value.y){size=value;Changed();}
}
void SpriteRendererComponent::SetSpriteReference(Serialize::PPtr<Assets::SpriteAsset> value)
{
    if (sprite.GetInstanceID() == value.GetInstanceID()) return;
    sprite = value; m_asset.reset(); m_hash.clear(); m_error.clear(); Changed();
}
void SpriteRendererComponent::SetColor(Maths::Vector4 value)
{
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) || !std::isfinite(value.w)) return;
    color = value; Changed();
}
void SpriteRendererComponent::SetFlipX(bool value) { if (flipX != value) { flipX = value; Changed(); } }
void SpriteRendererComponent::SetFlipY(bool value) { if (flipY != value) { flipY = value; Changed(); } }
void SpriteRendererComponent::SetOrderInLayer(int value) { if (orderInLayer != value) { orderInLayer = value; Changed(); } }
void SpriteRendererComponent::SetSortingLayer(std::string value)
{
    if (value.empty()) value = "Default";
    if (sortingLayer != value) { sortingLayer = std::move(value); Changed(); }
}
std::shared_ptr<const Core::Assets::SpriteArtifact> SpriteRendererComponent::CaptureSprite()
{
    SpriteAssetCaptureContext context;
    return CaptureSprite(context);
}
std::shared_ptr<const Core::Assets::SpriteArtifact> SpriteRendererComponent::CaptureSprite(SpriteAssetCaptureContext& context)
{
    NLS_PROFILE_NAMED_SCOPE("Sprite2D::CaptureAsset");
    auto [entry, inserted] = context.entries.try_emplace(sprite.GetInstanceID());
    auto& captured = entry->second;
    if (inserted)
    {
        Serialize::ObjectIdentifier reference;
        captured.validReference = Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(sprite.GetInstanceID(), reference);
        if (captured.validReference) captured.asset = Assets::LoadSpriteAsset(reference, captured.error);
        else captured.error = "Sprite reference is empty or invalid";
    }
    m_error = captured.error;
    if (!captured.validReference) return {};
    if (const auto& resolved = captured.asset)
    {
        if (m_hash != resolved->ContentHash()) { m_hash = resolved->ContentHash(); Changed(); }
        m_asset = resolved;
    }
    return m_asset ? m_asset->Capture() : nullptr;
}
}
