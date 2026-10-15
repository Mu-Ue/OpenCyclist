#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ble/common.h"

namespace ble {

// The Bluetooth radio, and the devices nearby that the app can use. Posts WM_RADIO_STATE when the radio
// changes and WM_NEARBY_CHANGED when the list does.
class Bluetooth {
public:
    enum class State { Unknown, Missing, Off, On };
    struct Device {
        uint64_t address;
        Role role;
        std::wstring name;
        ULONGLONG seen = 0;  // the last advert, by GetTickCount64
    };

    explicit Bluetooth(HWND window);
    ~Bluetooth();
    Bluetooth(const Bluetooth&) = delete;
    Bluetooth& operator=(const Bluetooth&) = delete;

    // Turns the radio on, or opens the Bluetooth settings when Windows won't let the app.
    void TurnOn() { TurnOnRadio(shared_); }
    // Looks for devices while on. Each start forgets the list and looks afresh, and devices that stop advertising,
    // such as a trainer gone to sleep, drop off it.
    void Scan(bool on);
    std::vector<Device> Nearby() const;

private:
    struct Shared;
    static winrt::fire_and_forget WatchRadio(std::shared_ptr<Shared> shared);
    static winrt::fire_and_forget TurnOnRadio(std::shared_ptr<Shared> shared);

    std::shared_ptr<Shared> shared_;  // with the callbacks, which can outlive this
    adv::BluetoothLEAdvertisementWatcher watcher_{nullptr};
};

}  // namespace ble
