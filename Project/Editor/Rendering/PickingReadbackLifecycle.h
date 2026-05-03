#pragma once

#include <cstdint>
#include <optional>

namespace NLS::Editor::Rendering
{
    template<typename SceneT>
    struct PickingReadbackFrame
    {
        SceneT* scene = nullptr;
        uint16_t width = 0u;
        uint16_t height = 0u;

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
        }

        const Frame* GetReadableFrame() const
        {
            return m_readableFrame.has_value() ? &m_readableFrame.value() : nullptr;
        }

    private:
        std::optional<Frame> m_pendingFrame;
        std::optional<Frame> m_readableFrame;
    };
}
