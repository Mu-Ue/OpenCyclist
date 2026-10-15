#include "ble/common.h"

#include <atomic>
#include <memory>

#include <winrt/Windows.Devices.Radios.h>

namespace ble {

using winrt::Windows::Devices::Bluetooth::BluetoothAdapter;
using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;

winrt::guid ShortUuid(uint16_t id) {
    return winrt::guid{0x00000000u | id, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
}

std::wstring CleanName(std::wstring_view name) {
    constexpr size_t kMaxLength = 48;
    std::wstring clean;
    for (const wchar_t c : name) {
        if (clean.size() == kMaxLength) break;
        if (c >= 0x20 && c != 0x7F) clean += c;
    }
    while (!clean.empty() && clean.back() == L' ') clean.pop_back();
    return clean;
}

IAsyncOperation<bool> RadioOn() {
    const auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
    if (!adapter || !adapter.IsLowEnergySupported()) co_return false;
    const auto radio = co_await adapter.GetRadioAsync();
    co_return radio && radio.State() == winrt::Windows::Devices::Radios::RadioState::On;
}

IAsyncOperation<uint64_t> Scan(std::function<bool(const adv::BluetoothLEAdvertisement&)> match,
                               std::chrono::seconds timeout, HANDLE wake) {
    // Shared with the callbacks, which can still run briefly after the watcher stops.
    struct State {
        std::function<bool(const adv::BluetoothLEAdvertisement&)> match;
        std::atomic<uint64_t> address{0};
        std::atomic<bool> stopped{false};
        winrt::handle found{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    };
    auto state = std::make_shared<State>();
    state->match = std::move(match);

    adv::BluetoothLEAdvertisementWatcher watcher;
    watcher.ScanningMode(adv::BluetoothLEScanningMode::Active);  // scan responses carry the device names
    watcher.Received([state](const auto&, const adv::BluetoothLEAdvertisementReceivedEventArgs& args) {
        if (state->address == 0 && state->match(args.Advertisement())) {
            state->address = args.BluetoothAddress();
            SetEvent(state->found.get());
        }
    });
    // Stopped also fires when the radio goes off or the scan fails. The status alone can't tell, since it can
    // still read Created for a moment after Start.
    watcher.Stopped([state](const auto&, const auto&) {
        state->stopped = true;
        SetEvent(state->found.get());
    });
    watcher.Start();

    const auto deadline = GetTickCount64() + std::chrono::milliseconds(timeout).count();
    while (state->address == 0 && !state->stopped && GetTickCount64() < deadline &&
           WaitForSingleObject(wake, 0) != WAIT_OBJECT_0) {
        co_await winrt::resume_on_signal(state->found.get(), std::chrono::milliseconds(250));
    }
    watcher.Stop();
    co_return state->address.load();
}

IAsyncOperation<gatt::GattDeviceService> FindService(BluetoothLEDevice device, winrt::guid id) {
    const auto result = co_await device.GetGattServicesForUuidAsync(id, BluetoothCacheMode::Uncached);
    if (result.Status() != gatt::GattCommunicationStatus::Success || result.Services().Size() == 0) co_return nullptr;
    co_return result.Services().GetAt(0);
}

IAsyncOperation<gatt::GattCharacteristic> FindCharacteristic(gatt::GattDeviceService service, winrt::guid id) {
    const auto result = co_await service.GetCharacteristicsForUuidAsync(id, BluetoothCacheMode::Uncached);
    if (result.Status() != gatt::GattCommunicationStatus::Success || result.Characteristics().Size() == 0)
        co_return nullptr;
    co_return result.Characteristics().GetAt(0);
}

IAsyncOperation<bool> Subscribe(gatt::GattCharacteristic c, gatt::GattClientCharacteristicConfigurationDescriptorValue how) {
    co_return co_await c.WriteClientCharacteristicConfigurationDescriptorAsync(how) ==
        gatt::GattCommunicationStatus::Success;
}

IAsyncOperation<bool> Write(gatt::GattCharacteristic c, std::vector<uint8_t> bytes, bool withResponse) {
    winrt::Windows::Storage::Streams::DataWriter writer;
    writer.WriteBytes(bytes);
    co_return co_await c.WriteValueAsync(writer.DetachBuffer(), withResponse ? gatt::GattWriteOption::WriteWithResponse
                                                                             : gatt::GattWriteOption::WriteWithoutResponse) ==
        gatt::GattCommunicationStatus::Success;
}

Connection::~Connection() {
    try {
        if (service) service.Close();
        if (device) device.Close();
    } catch (...) {
    }
}

}  // namespace ble
