#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "ble/link.h"
#include "sim/ride.h"

namespace ble {

constexpr WPARAM kHasPower = 1, kHasCadence = 2;

// Whether an advert looks like a smart trainer.
bool IsTrainer(const adv::BluetoothLEAdvertisement& ad);

// A smart trainer over the Fitness Machine Service. Posts its power and cadence to the window and holds the
// latest target.
class Trainer : public Link {
public:
    Trainer(HWND window, uint64_t address, std::wstring name);
    ~Trainer();  // releases the trainer and waits for the connection to close

    void SetTarget(TrainerTarget target);

private:
    IAsyncOperation<bool> Session(Connection& link) override;
    IAsyncOperation<bool> Send(gatt::GattCharacteristic control, TrainerTarget target);
    void OnBikeData(const winrt::Windows::Storage::Streams::IBuffer& data);

    std::atomic<ULONGLONG> lastData_{0};
    int minLevel_ = 0, maxLevel_ = 1000;  // resistance range, in 0.1 units
    std::mutex mutex_;
    TrainerTarget target_;
};

}  // namespace ble
