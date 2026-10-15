#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "ble/link.h"

namespace ble {

// Whether an advert is a heart rate monitor, such as a Polar H10 chest strap.
bool IsHeartRateMonitor(const adv::BluetoothLEAdvertisement& ad);

// A heart rate monitor over the standard Heart Rate Service. Posts the heart rate to the window.
class HeartRateMonitor : public Link {
public:
    HeartRateMonitor(HWND window, uint64_t address, std::wstring name);
    ~HeartRateMonitor();

private:
    IAsyncOperation<bool> Session(Connection& link) override;
    void OnMeasurement(const winrt::Windows::Storage::Streams::IBuffer& data);

    std::atomic<ULONGLONG> lastData_{0};
};

}  // namespace ble
