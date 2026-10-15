#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "ble/common.h"

namespace ble {

// Zwift Ride button bits, as the controllers report them.
namespace button {
constexpr uint32_t kLeft = 0x0001, kUp = 0x0002, kRight = 0x0004, kDown = 0x0008;
constexpr uint32_t kA = 0x0010, kB = 0x0020, kY = 0x0040, kZ = 0x0080;
constexpr uint32_t kShiftUpLeft = 0x0100, kShiftDownLeft = 0x0200, kShiftUpRight = 0x1000, kShiftDownRight = 0x2000;
constexpr uint32_t kPowerUpLeft = 0x0400, kPowerUpRight = 0x4000, kOnOffLeft = 0x0800, kOnOffRight = 0x8000;
}  // namespace button

// The Zwift Ride's two controllers. The left one connects and relays the right one's buttons. Posts the pressed
// buttons to the window whenever they change.
class Controllers {
public:
    // Locked: newer Ride firmware hides the controller service until it's unlocked in Zwift.
    enum class State { Searching, Connected, Locked };

    explicit Controllers(HWND window);
    ~Controllers();
    Controllers(const Controllers&) = delete;
    Controllers& operator=(const Controllers&) = delete;

private:
    winrt::fire_and_forget Run();
    IAsyncAction Session();
    void OnMessage(const winrt::Windows::Storage::Streams::IBuffer& data);
    void Post(UINT message, WPARAM w, LPARAM l = 0) const { PostMessageW(window_, message, w, l); }

    HWND window_;
    std::atomic<bool> stop_{false};
    winrt::handle wake_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    winrt::handle done_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

}  // namespace ble
