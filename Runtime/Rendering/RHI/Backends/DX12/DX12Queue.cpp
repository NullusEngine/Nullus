#include "Rendering/RHI/Backends/DX12/DX12Queue.h"

#include "Rendering/RHI/Backends/DX12/DX12Synchronization.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

#include <cstdint>
#include <string>
#include <vector>

#include <Debug/Logger.h>

#if defined(_WIN32)
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	namespace
	{
		std::wstring Utf8ToWideString(const std::string& value)
		{
			if (value.empty())
				return {};

			const int requiredChars = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
			if (requiredChars <= 1)
				return {};

			std::wstring wide(static_cast<size_t>(requiredChars), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), requiredChars);
			wide.resize(static_cast<size_t>(requiredChars - 1));
			return wide;
		}

		void SetDx12ObjectName(ID3D12Object* object, const std::string& debugName)
		{
			if (object == nullptr || debugName.empty())
				return;

			const std::wstring wideName = Utf8ToWideString(debugName);
			if (!wideName.empty())
				object->SetName(wideName.c_str());
		}

		bool ShouldLogDx12ValidationMessages()
		{
			return NLS::Render::Settings::GetThreadDiagnosticsSettings().dx12LogMessages;
		}

		bool ShouldLogDx12FrameFlow()
		{
			return NLS::Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow;
		}

		void LogDx12DebugMessages(ID3D12Device* device, const std::string& context)
		{
			if (device == nullptr)
				return;

			Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
				return;

			const UINT64 messageCount = infoQueue->GetNumStoredMessages();
			if (messageCount == 0)
				return;

			const UINT64 firstMessage = messageCount > 8 ? messageCount - 8 : 0;
			for (UINT64 messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
			{
				SIZE_T messageSize = 0;
				if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageSize)) || messageSize == 0)
					continue;

				std::vector<char> messageBytes(messageSize);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageBytes.data());
				if (FAILED(infoQueue->GetMessage(messageIndex, message, &messageSize)))
					continue;

				NLS_LOG_ERROR(
					context +
					": D3D12 message id=" + std::to_string(message->ID) +
					" severity=" + std::to_string(static_cast<int>(message->Severity)) +
					" text=" + std::string(message->pDescription));
			}

			infoQueue->ClearStoredMessages();
		}
	}
#endif

	NativeDX12Queue::NativeDX12Queue(
		ID3D12Device* device,
		ID3D12CommandQueue* queue,
		NLS::Render::RHI::QueueType queueType,
		const std::string& debugName)
		: m_device(device)
		, m_queue(queue)
		, m_queueType(queueType)
		, m_debugName(debugName)
	{
#if defined(_WIN32)
		SetDx12ObjectName(m_queue, m_debugName);
		NLS_LOG_INFO(
			"NativeDX12Queue created: device=" +
			std::to_string(reinterpret_cast<uintptr_t>(device)) +
			" queue=" +
			std::to_string(reinterpret_cast<uintptr_t>(queue)));
#else
		(void)device;
		(void)queue;
		(void)queueType;
#endif
	}

	std::string_view NativeDX12Queue::GetDebugName() const
	{
		return m_debugName;
	}

	NLS::Render::RHI::QueueType NativeDX12Queue::GetType() const
	{
		return m_queueType;
	}

	void NativeDX12Queue::Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
	{
#if defined(_WIN32)
		if (m_queue == nullptr)
			return;

		for (const auto& semaphore : submitDesc.waitSemaphores)
		{
			auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
			if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
				continue;

			const HRESULT waitHr = m_queue->Wait(
				nativeSemaphore->GetFence(),
				nativeSemaphore->GetWaitValue());
			if (FAILED(waitHr))
			{
				NLS_LOG_ERROR(
					"NativeDX12Queue::Submit: queue wait on semaphore failed hr=" +
					std::to_string(waitHr));
			}
		}

		std::vector<ID3D12CommandList*> commandLists;
		for (const auto& cmdBuffer : submitDesc.commandBuffers)
		{
			if (cmdBuffer == nullptr)
				continue;

			auto* nativeCommandList = static_cast<ID3D12CommandList*>(cmdBuffer->GetNativeCommandBuffer());
			if (nativeCommandList != nullptr)
				commandLists.push_back(nativeCommandList);
		}

		if (!commandLists.empty())
		{
			if (ShouldLogDx12FrameFlow())
			{
				NLS_LOG_INFO("NativeDX12Queue::Submit: executing " + std::to_string(commandLists.size()) + " command lists");
			}
			m_queue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), commandLists.data());
			if (ShouldLogDx12FrameFlow())
			{
				NLS_LOG_INFO("NativeDX12Queue::Submit: ExecuteCommandLists succeeded");
			}

			if (m_device != nullptr && ShouldLogDx12ValidationMessages())
			{
				LogDx12DebugMessages(m_device, "NativeDX12Queue::Submit(after ExecuteCommandLists)");
			}

			if (m_device != nullptr)
			{
				const HRESULT deviceStatus = m_device->GetDeviceRemovedReason();
				if (FAILED(deviceStatus))
				{
					NLS_LOG_ERROR("NativeDX12Queue::Submit: device status after ExecuteCommandLists hr=" + std::to_string(deviceStatus));
					LogDx12DebugMessages(m_device, "NativeDX12Queue::Submit");
				}
			}
		}
		else if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO("NativeDX12Queue::Submit: no command lists to execute");
		}

		for (const auto& semaphore : submitDesc.signalSemaphores)
		{
			auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
			if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
				continue;

			if (!nativeSemaphore->SignalOnQueue(m_queue))
			{
				NLS_LOG_ERROR("NativeDX12Queue::Submit: queue signal semaphore failed");
			}
		}

		if (submitDesc.signalFence != nullptr)
		{
			auto* nativeFence = dynamic_cast<NativeDX12Fence*>(submitDesc.signalFence.get());
			if (nativeFence != nullptr && nativeFence->GetFence() != nullptr)
			{
				const UINT64 fenceValue = nativeFence->GetTargetValue();
				const HRESULT signalHr = m_queue->Signal(nativeFence->GetFence(), fenceValue);
				if (FAILED(signalHr))
				{
					NLS_LOG_ERROR(
						"NativeDX12Queue::Submit: queue signal fence failed hr=" +
						std::to_string(signalHr) +
						" value=" + std::to_string(fenceValue));
				}
			}
		}
#else
		(void)submitDesc;
#endif
	}

	void NativeDX12Queue::Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc)
	{
#if defined(_WIN32)
		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO(
				"NativeDX12Queue::Present: called with swapchain=" +
				std::to_string(reinterpret_cast<uintptr_t>(presentDesc.swapchain.get())) +
				" imageIndex=" +
				std::to_string(presentDesc.imageIndex));
		}

		if (m_queue == nullptr || presentDesc.swapchain == nullptr)
		{
			NLS_LOG_ERROR("NativeDX12Queue::Present: queue or swapchain is null");
			return;
		}

		auto* swapchain = reinterpret_cast<IDXGISwapChain3*>(presentDesc.swapchain->GetNativeSwapchainHandle());
		if (swapchain == nullptr)
		{
			NLS_LOG_ERROR("NativeDX12Queue::Present: GetNativeSwapchainHandle returned null");
			return;
		}

		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO(
				"NativeDX12Queue::Present: swapchain ptr=" +
				std::to_string(reinterpret_cast<uintptr_t>(swapchain)) +
				" uiSemaphore=" +
				std::to_string(reinterpret_cast<uintptr_t>(presentDesc.uiSignalSemaphore)));
		}

		for (const auto& semaphore : presentDesc.waitSemaphores)
		{
			auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
			if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
				continue;

			const HRESULT waitHr = m_queue->Wait(
				nativeSemaphore->GetFence(),
				nativeSemaphore->GetWaitValue());
			if (FAILED(waitHr))
			{
				NLS_LOG_ERROR(
					"NativeDX12Queue::Present: queue wait on present semaphore failed hr=" +
					std::to_string(waitHr));
			}
		}

		if (presentDesc.uiSignalSemaphore != nullptr)
		{
			auto* uiFence = reinterpret_cast<ID3D12Fence*>(presentDesc.uiSignalSemaphore);
			if (uiFence != nullptr)
			{
				const HRESULT waitHr = m_queue->Wait(uiFence, 1u);
				if (FAILED(waitHr))
				{
					NLS_LOG_WARNING("NativeDX12Queue::Present: failed to wait on UI fence before present hr=" + std::to_string(waitHr));
				}
			}
		}

		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO("NativeDX12Queue::Present: calling swapchain->Present");
		}
		const HRESULT hr = swapchain->Present(0, 0);
		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO("NativeDX12Queue::Present: Present returned hr=" + std::to_string(hr));
		}
		if (FAILED(hr))
		{
			NLS_LOG_ERROR("NativeDX12Queue::Present: Present failed with hr=" + std::to_string(hr));
			LogDx12DebugMessages(m_device, "NativeDX12Queue::Present");

			if (m_device != nullptr)
			{
				const HRESULT reason = m_device->GetDeviceRemovedReason();
				if (FAILED(reason))
				{
					if (hr == DXGI_ERROR_DEVICE_REMOVED)
						NLS_LOG_ERROR("NativeDX12Queue::Present: DXGI_ERROR_DEVICE_REMOVED");
					NLS_LOG_ERROR("NativeDX12Queue::Present: Device removed reason hr=" + std::to_string(reason));
				}
			}
		}
#else
		(void)presentDesc;
#endif
	}
}
