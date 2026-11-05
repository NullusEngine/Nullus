#pragma once

#include "Components/Component.h"
#include "Assets/SpriteAsset.h"
#include <Assets/SpriteSortingLayers.h>
#include <Math/Vector4.h>
#include <Math/Vector2.h>
#include <unordered_map>
#include "Components/SpriteRendererComponent.generated.h"

namespace NLS::Engine::Components
{
// One game-thread extraction owns this context. Do not retain it across frames:
// each extraction must observe the current published artifact revision.
struct SpriteAssetCaptureContext
{
    struct Entry
    {
        std::shared_ptr<Assets::SpriteAsset> asset;
        std::string error;
        bool validReference = false;
    };
    std::unordered_map<NLS::InstanceID, Entry> entries;
};

CLASS(NLS_ENGINE_API SpriteRendererComponent, Scriptable, ComponentMenu("Rendering/Sprite Renderer"), DisallowMultipleComponent) : public Component
{
public:
    GENERATED_BODY()
    SpriteRendererComponent() = default;
    ENUM(ESpriteDrawMode) { Simple=0, Sliced=1, Tiled=2 };
    FUNCTION()
    ESpriteDrawMode GetDrawMode() const { return drawMode; }
    FUNCTION()
    void SetDrawMode(ESpriteDrawMode value);
    // Portable script ABI: 0 Simple, 1 Sliced, 2 Tiled. The same setter validates
    // values for native, Inspector, C# and Lua callers.
    FUNCTION(Scriptable)
    int GetDrawModeValue() const { return int(drawMode); }
    FUNCTION(Scriptable)
    void SetDrawModeValue(int value) { SetDrawMode(static_cast<ESpriteDrawMode>(value)); }
    FUNCTION(Scriptable)
    Maths::Vector2 GetSize() const { return size; }
    FUNCTION(Scriptable)
    void SetSize(Maths::Vector2 value);
    FUNCTION()
    Serialize::PPtr<Assets::SpriteAsset> GetSpriteReference() const { return sprite; }
    FUNCTION()
    void SetSpriteReference(Serialize::PPtr<Assets::SpriteAsset> value);
    FUNCTION(Scriptable)
    Maths::Vector4 GetColor() const { return color; }
    FUNCTION(Scriptable)
    void SetColor(Maths::Vector4 value);
    FUNCTION(Scriptable)
    bool GetFlipX() const { return flipX; }
    FUNCTION(Scriptable)
    void SetFlipX(bool value);
    FUNCTION(Scriptable)
    bool GetFlipY() const { return flipY; }
    FUNCTION(Scriptable)
    void SetFlipY(bool value);
    FUNCTION(Scriptable)
    int GetOrderInLayer() const { return orderInLayer; }
    FUNCTION(Scriptable)
    void SetOrderInLayer(int value);
    FUNCTION(Scriptable)
    std::string GetSortingLayer() const { return sortingLayer; }
    FUNCTION(Scriptable)
    void SetSortingLayer(std::string value);
    std::shared_ptr<const Core::Assets::SpriteArtifact> CaptureSprite();
    std::shared_ptr<const Core::Assets::SpriteArtifact> CaptureSprite(SpriteAssetCaptureContext& context);
    const std::string& LastError() const { return m_error; }
    uint64_t GetRenderRevision() const { return m_revision; }
private:
    void Changed();
    PROPERTY(DisplayName("Sprite"), Getter(GetSpriteReference), Setter(SetSpriteReference))
    Serialize::PPtr<Assets::SpriteAsset> sprite;
    PROPERTY(DisplayName("Color"), Getter(GetColor), Setter(SetColor))
    Maths::Vector4 color {1, 1, 1, 1};
    PROPERTY(DisplayName("Flip X"), Getter(GetFlipX), Setter(SetFlipX))
    bool flipX = false;
    PROPERTY(DisplayName("Flip Y"), Getter(GetFlipY), Setter(SetFlipY))
    bool flipY = false;
    PROPERTY(DisplayName("Sorting Layer"), Getter(GetSortingLayer), Setter(SetSortingLayer))
    std::string sortingLayer = Core::Assets::SpriteSortingLayers::DefaultId;
    PROPERTY(DisplayName("Order in Layer"), Getter(GetOrderInLayer), Setter(SetOrderInLayer))
    int orderInLayer = 0;
    PROPERTY(DisplayName("Draw Mode"), Getter(GetDrawMode), Setter(SetDrawMode))
    ESpriteDrawMode drawMode = ESpriteDrawMode::Simple;
    PROPERTY(DisplayName("Size"), Getter(GetSize), Setter(SetSize))
    Maths::Vector2 size{1,1};
    std::shared_ptr<Assets::SpriteAsset> m_asset;
    std::string m_error, m_hash;
    uint64_t m_revision = 1;
};
}
