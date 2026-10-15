#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

// Small helpers over the Windows Bluetooth LE APIs. All of them are coroutines meant to run on the thread pool.
namespace ble {

namespace adv = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using winrt::Windows::Devices::Bluetooth::BluetoothLEDevice;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Foundation::IAsyncOperation;

// Messages the BLE threads post to the app window.
constexpr UINT WM_TRAINER_DATA = WM_APP + 1;      // wParam: kHasPower | kHasCadence, lParam: power, cadence x2
constexpr UINT WM_TRAINER_STATE = WM_APP + 2;     // wParam: Trainer::State
constexpr UINT WM_RIDE_BUTTONS = WM_APP + 3;      // lParam: pressed button bits
constexpr UINT WM_CONTROLLER_STATE = WM_APP + 4;  // wParam: Controllers::State

// A 16-bit UUID from the Bluetooth base UUID.
winrt::guid ShortUuid(uint16_t id);

// Whether a Bluetooth LE radio is present and on.
IAsyncOperation<bool> RadioOn();

// Scans until `match` accepts an advertisement or the timeout passes. Returns the device address, or 0. Stops
// early when `wake` is signalled.
IAsyncOperation<uint64_t> Scan(std::function<bool(const adv::BluetoothLEAdvertisement&)> match,
                               std::chrono::seconds timeout, HANDLE wake);

// The first service or characteristic with this UUID, or null.
IAsyncOperation<gatt::GattDeviceService> FindService(BluetoothLEDevice device, winrt::guid id);
IAsyncOperation<gatt::GattCharacteristic> FindCharacteristic(gatt::GattDeviceService service, winrt::guid id);

IAsyncOperation<bool> Subscribe(gatt::GattCharacteristic c, gatt::GattClientCharacteristicConfigurationDescriptorValue how);
IAsyncOperation<bool> Write(gatt::GattCharacteristic c, std::vector<uint8_t> bytes, bool withResponse);

// Closes the service and device when the session ends, however it ends, so Windows drops the connection.
struct Connection {
    BluetoothLEDevice device{nullptr};
    gatt::GattDeviceService service{nullptr};
    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();
};

inline bool Connected(const BluetoothLEDevice& device) {
    return device.ConnectionStatus() == winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus::Connected;
}

}  // namespace ble
