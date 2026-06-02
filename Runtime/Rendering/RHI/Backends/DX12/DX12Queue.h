#pragma once

#include <string>
#include <string_view>

#include "Rendering/RHI/Core/RHISwapchain.h"

struct ID3D12CommandQueue;
struct ID3D12Device;

namespace NLS::Render::Backend
{
	class NativeDX12Queue final : public NLS::Render::RHI::RHIQueue
	{
	public:
		NativeDX12Queue(
			ID3D12Device* device,
			ID3D12CommandQueue* queue,
			NLS::Render::RHI::QueueType queueType,
			const std::string& debugName);
		~NativeDX12Queue() override;

		std::string_view GetDebugName() const override;
		NLS::Render::RHI::QueueType GetType() const override;
		void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override;
		void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override;
		NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(
			const NLS::Render::RHI::RHISubmitDesc& submitDesc) override;
		NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
			const NLS::Render::RHI::RHIPresentDesc& presentDesc) override;

	private:
		ID3D12Device* m_device = nullptr;
		ID3D12CommandQueue* m_queue = nullptr;
		NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
		std::string m_debugName;
	};
}
