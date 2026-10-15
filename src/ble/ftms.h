#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "ble/common.h"
#include "sim/ride.h"

namespace ble {

constexpr WPARAM kHasPower = 1, kHasCadence = 2;

// A smart trainer over the Fitness Machine Service. Finds and connects to it on its own, reconnects when it
// drops, posts its power and cadence to the window, and holds the latest target.
class Trainer {
public:
    enum class State { NoBluetooth, Searching, Connected };

    Trainer(HWND window, uint64_t address);
    ~Trainer();  // releases the trainer and waits for the connection to close
    Trainer(const Trainer&) = delete;
    Trainer& operator=(const Trainer&) = delete;

    void SetTarget(TrainerTarget target);
    uint64_t Address() const { return address_; }

private:
    winrt::fire_and_forget Run();
    IAsyncAction Session();
    IAsyncOperation<bool> Send(gatt::GattCharacteristic control, TrainerTarget target);
    void OnBikeData(const winrt::Windows::Storage::Streams::IBuffer& data);
    void Post(UINT message, WPARAM w, LPARAM l = 0) const { PostMessageW(window_, message, w, l); }

    HWND window_;
    std::atomic<uint64_t> address_;
    int failures_ = 0;  // connects to the saved address that failed in a row
    std::atomic<ULONGLONG> lastData_{0};
    int minLevel_ = 0, maxLevel_ = 1000;  // resistance range, in 0.1 units
    std::mutex mutex_;
    TrainerTarget target_;
    std::atomic<bool> stop_{false};
    winrt::handle wake_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    winrt::handle done_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

}  // namespace ble
