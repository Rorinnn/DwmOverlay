// FrameExchange: Shared texture ownership and frame publication

module;

#include <Windows.h>

module DwmOverlay.Graphics.FrameExchange;

import std;

namespace DwmOverlay
{
namespace
{
#pragma region RuntimeState

std::mutex                 frameMutex;
DisplayFrame               currentFrame;
std::atomic<std::uint64_t> displayAdapter{0};
std::atomic<bool>          hasDisplayAdapter{false};

#pragma endregion
} // namespace

#pragma region PublicFunctions

void SetDisplayAdapter(LUID adapter)
{
    displayAdapter.store(std::bit_cast<std::uint64_t>(adapter), std::memory_order_release);
    hasDisplayAdapter.store(true, std::memory_order_release);
}

bool GetDisplayAdapter(LUID& adapter)
{
    if (!hasDisplayAdapter.load(std::memory_order_acquire))
        return false;
    adapter = std::bit_cast<LUID>(displayAdapter.load(std::memory_order_acquire));
    return true;
}

void PublishDisplayFrame(DisplayFrame frame)
{
    std::lock_guard lock(frameMutex);
    currentFrame = std::move(frame);
}

bool ReadDisplayFrame(DisplayFrame& frame)
{
    std::unique_lock lock(frameMutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    frame = currentFrame;
    return true;
}

#pragma endregion
} // namespace DwmOverlay
