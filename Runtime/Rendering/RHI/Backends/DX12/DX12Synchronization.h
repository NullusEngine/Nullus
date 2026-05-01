#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Rendering/RHI/Core/RHISync.h"

struct ID3D12CommandQueue;
struct ID3D12Device;

#if defined(_WIN32)
#include <d3d12.h>
#include <wrl/client.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::Backend
{
	class NativeDX12Fence final : public NLS::Render::RHI::RHIFence
	{
	public:
		NativeDX12Fence(ID3D12Device* device, const std::string& debugName);

		std::string_view GetDebugName() const override;
		bool IsSignaled() const override;
		void Reset() override;
		bool Wait(uint64_t timeoutNanoseconds) override;

#if defined(_WIN32)
		ID3D12Fence* GetFence() const;
		UINT64 GetTargetValue() const;
#endif

	private:
		std::string m_debugName;
#if defined(_WIN32)
		ID3D12Device* m_device = nullptr;
		Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
		UINT64 m_targetValue = 0;
#endif
	};

	class NativeDX12Semaphore final : public NLS::Render::RHI::RHISemaphore
	{
	public:
		NativeDX12Semaphore(ID3D12Device* device, const std::string& debugName);

		std::string_view GetDebugName() const override;
		bool IsSignaled() const override;
		void Reset() override;
		void* GetNativeSemaphoreHandle() override;

#if defined(_WIN32)
		ID3D12Fence* GetFence() const;
		UINT64 GetWaitValue() const;
		bool SignalOnCpu();
		bool SignalOnQueue(ID3D12CommandQueue* queue);
#endif

	private:
		std::string m_debugName;
#if defined(_WIN32)
		ID3D12Device* m_device = nullptr;
		Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
		UINT64 m_signalValue = 0;
		UINT64 m_waitValue = 0;
#endif
	};
}
