#include "Rendering/RHI/Backends/DX12/DX12Synchronization.h"

#if defined(_WIN32)
#include <Windows.h>

#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"
#endif

namespace NLS::Render::Backend
{
	NativeDX12Fence::NativeDX12Fence(ID3D12Device* device, const std::string& debugName)
	{
#if defined(_WIN32)
		m_device = device;
		m_debugName = debugName;
		if (device != nullptr)
		{
			device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
		}
#else
		(void)device;
		m_debugName = debugName;
#endif
	}

	std::string_view NativeDX12Fence::GetDebugName() const
	{
		return m_debugName;
	}

	bool NativeDX12Fence::IsSignaled() const
	{
#if defined(_WIN32)
		if (m_fence == nullptr)
			return false;
		return m_fence->GetCompletedValue() >= m_targetValue;
#else
		return false;
#endif
	}

	void NativeDX12Fence::Reset()
	{
#if defined(_WIN32)
		++m_targetValue;
#endif
	}

	bool NativeDX12Fence::Wait(uint64_t timeoutNanoseconds)
	{
#if defined(_WIN32)
		if (m_fence == nullptr)
			return false;
		if (m_targetValue == 0)
			return true;
		const UINT64 completedValue = m_fence->GetCompletedValue();
		if (completedValue >= m_targetValue)
			return true;

		HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (event == nullptr)
			return false;

		const HRESULT setEventHr = m_fence->SetEventOnCompletion(m_targetValue, event);
		if (FAILED(setEventHr))
		{
			CloseHandle(event);
			return false;
		}
		const DWORD waitMs = NLS::Render::RHI::DX12::ConvertDX12WaitTimeoutNanosecondsToMilliseconds(timeoutNanoseconds);
		const DWORD result = WaitForSingleObject(event, waitMs);
		CloseHandle(event);
		return result == WAIT_OBJECT_0;
#else
		(void)timeoutNanoseconds;
		return false;
#endif
	}

	NLS::Render::RHI::NativeHandle NativeDX12Fence::GetNativeFenceHandle()
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, m_fence.Get() };
#else
		return {};
#endif
	}

#if defined(_WIN32)
	ID3D12Fence* NativeDX12Fence::GetFence() const
	{
		return m_fence.Get();
	}

	UINT64 NativeDX12Fence::GetTargetValue() const
	{
		return m_targetValue;
	}
#endif

	NativeDX12Semaphore::NativeDX12Semaphore(ID3D12Device* device, const std::string& debugName)
	{
#if defined(_WIN32)
		m_device = device;
		m_debugName = debugName;
		if (device != nullptr)
		{
			device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
		}
#else
		(void)device;
		m_debugName = debugName;
#endif
	}

	std::string_view NativeDX12Semaphore::GetDebugName() const
	{
		return m_debugName;
	}

	bool NativeDX12Semaphore::IsSignaled() const
	{
#if defined(_WIN32)
		if (m_fence == nullptr)
			return false;
		return m_waitValue != 0 && m_fence->GetCompletedValue() >= m_waitValue;
#else
		return false;
#endif
	}

	void NativeDX12Semaphore::Reset()
	{
#if defined(_WIN32)
		m_waitValue = 0;
#endif
	}

	NLS::Render::RHI::NativeHandle NativeDX12Semaphore::GetNativeSemaphoreHandle()
	{
#if defined(_WIN32)
		return { NLS::Render::RHI::BackendType::DX12, m_fence.Get(), m_waitValue };
#else
		return {};
#endif
	}

#if defined(_WIN32)
	ID3D12Fence* NativeDX12Semaphore::GetFence() const
	{
		return m_fence.Get();
	}

	UINT64 NativeDX12Semaphore::GetWaitValue() const
	{
		return m_waitValue;
	}

	bool NativeDX12Semaphore::SignalOnCpu()
	{
		if (m_fence == nullptr)
			return false;

		m_waitValue = ++m_signalValue;
		return SUCCEEDED(m_fence->Signal(m_waitValue));
	}

	bool NativeDX12Semaphore::SignalOnQueue(ID3D12CommandQueue* queue)
	{
		return SUCCEEDED(SignalOnQueueChecked(queue));
	}

	HRESULT NativeDX12Semaphore::SignalOnQueueChecked(ID3D12CommandQueue* queue)
	{
		if (queue == nullptr || m_fence == nullptr)
			return E_INVALIDARG;

		m_waitValue = ++m_signalValue;
		return queue->Signal(m_fence.Get(), m_waitValue);
	}
#endif
}
