#pragma once

#include "BaseDef.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace NLS::Debug
{
// Frames are captured on the caller's thread. The resolver owns only a shared
// payload, so a removed Console entry cannot be resurrected by a late result.
struct NativeStackTrace
{
    std::vector<uintptr_t> frames;
    std::mutex mutex;
    std::string resolved;
    std::atomic<bool> ready{false};
};

class NLS_BASE_API StackTrace final
{
public:
    static std::shared_ptr<NativeStackTrace> Capture();
    static std::string RawText(const std::vector<uintptr_t>& frames);
    static void Shutdown();
};
}
