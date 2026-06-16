#pragma once

#include "Assets/AssetThumbnailCache.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NLS::Render::Context
{
class Driver;
}

namespace NLS::Editor::Assets
{
struct EditorThumbnailPreviewResult
{
    std::vector<uint8_t> rgbaPixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
    std::string diagnostic;
};

class EditorThumbnailPreviewRenderer
{
public:
    explicit EditorThumbnailPreviewRenderer(NLS::Render::Context::Driver& driver);
    ~EditorThumbnailPreviewRenderer();

    EditorThumbnailPreviewRenderer(const EditorThumbnailPreviewRenderer&) = delete;
    EditorThumbnailPreviewRenderer& operator=(const EditorThumbnailPreviewRenderer&) = delete;

    bool Supports(const AssetThumbnailRequest& request) const;
    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
