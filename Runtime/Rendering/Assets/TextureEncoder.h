#pragma once

#include "RenderDef.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Assets/TextureBuildSettings.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::Render::Assets
{
struct TextureEncodeRequest
{
    const TextureBuildSettings* buildSettings = nullptr;
    const TextureArtifactData* sourceMips = nullptr;
};

struct TextureEncodeDiagnostic
{
    std::string stage = "encoding";
    std::string message;
};

struct TextureEncodeResult
{
    bool succeeded = false;
    TextureArtifactData artifact;
    std::vector<TextureEncodeDiagnostic> diagnostics;
};

class NLS_RENDER_API ITextureEncoder
{
public:
    virtual ~ITextureEncoder() = default;

    virtual std::string_view GetId() const = 0;
    virtual uint32_t GetVersion() const = 0;
    virtual bool SupportsFormat(RHI::TextureFormat format) const = 0;
    virtual TextureEncodeResult Encode(const TextureEncodeRequest& request) const = 0;
};

class NLS_RENDER_API TextureEncoderRegistry
{
public:
    bool Register(std::shared_ptr<ITextureEncoder> encoder);
    const ITextureEncoder* Find(std::string_view encoderId) const;
    const ITextureEncoder* FindForFormat(RHI::TextureFormat format) const;
    bool IsEmpty() const { return m_encoders.empty(); }

private:
    std::vector<std::shared_ptr<ITextureEncoder>> m_encoders;
};
}
