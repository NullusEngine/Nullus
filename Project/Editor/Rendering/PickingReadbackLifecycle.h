#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <GameObject.h>

#include "Panels/SceneViewPickingPolicy.h"

namespace NLS::Render::RHI
{
    class RHITexture;
}

namespace NLS::Editor::Rendering
{
    template<typename SceneT>
    struct PickingReadbackFrame
    {
        SceneT* scene = nullptr;
        uint16_t width = 0u;
        uint16_t height = 0u;
        uint64_t serial = 0u;
        std::shared_ptr<NLS::Render::RHI::RHITexture> readbackTexture;
        uint64_t readbackGeneration = 0u;
        std::vector<NLS::Engine::GameObject*> pickRegistry;
        NLS::Editor::Panels::HitProxyPickingSignature signature {};

        bool IsValid() const
        {
            return scene != nullptr && width > 0u && height > 0u;
        }
    };

    template<typename SceneT>
    class PickingReadbackLifecycle
    {
    public:
        using Frame = PickingReadbackFrame<SceneT>;

        void QueueSubmittedFrame(Frame frame)
        {
            if (frame.IsValid())
                m_pendingFrame = frame;
            else
                m_pendingFrame.reset();
        }

        void PromotePendingFrameIfReadbackAvailable(const bool readbackAvailable)
        {
            if (!readbackAvailable || !m_pendingFrame.has_value())
                return;

            m_readableFrame = m_pendingFrame;
            m_pendingFrame.reset();
        }

        void MarkSubmittedFrameImmediatelyReadable(Frame frame)
        {
            QueueSubmittedFrame(frame);
            PromotePendingFrameIfReadbackAvailable(true);
        }

        void ResetSubmittedFrame()
        {
            m_pendingFrame.reset();
            m_readableFrame.reset();
            m_pixelReadbackInFlight = false;
        }

        const Frame* GetReadableFrame() const
        {
            return m_readableFrame.has_value() ? &m_readableFrame.value() : nullptr;
        }

        const Frame* GetPendingFrame() const
        {
            return m_pendingFrame.has_value() ? &m_pendingFrame.value() : nullptr;
        }

        bool TryBeginPixelReadback()
        {
            if (m_pixelReadbackInFlight)
                return false;

            m_pixelReadbackInFlight = true;
            return true;
        }

        void EndPixelReadback()
        {
            m_pixelReadbackInFlight = false;
        }

    private:
        std::optional<Frame> m_pendingFrame;
        std::optional<Frame> m_readableFrame;
        bool m_pixelReadbackInFlight = false;
    };
}
