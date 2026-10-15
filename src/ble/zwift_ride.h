#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "ble/link.h"

namespace ble {

// Zwift Ride button bits, as the controllers report them.
namespace button {
constexpr uint32_t kLeft = 0x0001, kUp = 0x0002, kRight = 0x0004, kDown = 0x0008;
constexpr uint32_t kA = 0x0010, kB = 0x0020, kY = 0x0040, kZ = 0x0080;
constexpr uint32_t kShiftUpLeft = 0x0100, kShiftDownLeft = 0x0200, kShiftUpRight = 0x1000, kShiftDownRight = 0x2000;
constexpr uint32_t kPowerUpLeft = 0x0400, kPowerUpRight = 0x4000, kOnOffLeft = 0x0800, kOnOffRight = 0x8000;
}  // namespace button

// Whether an advert is the Zwift Ride's left controller, the one that connects.
bool IsRideLeft(const adv::BluetoothLEAdvertisement& ad);

// The Zwift Ride's two controllers. The left one connects and relays the right one's buttons. Posts the pressed
// buttons to the window whenever they change. Newer Ride firmware hides the controller service until it's
// unlocked in Zwift, which shows as State::Locked.
class Controllers : public Link {
public:
    Controllers(HWND window, uint64_t address, std::wstring name);
    ~Controllers();

private:
    IAsyncOperation<bool> Session(Connection& link) override;
    void OnMessage(const winrt::Windows::Storage::Streams::IBuffer& data);
};

}  // namespace ble
