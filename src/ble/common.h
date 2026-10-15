#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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
constexpr UINT WM_TRAINER_DATA = WM_APP + 1;    // wParam: kHasPower | kHasCadence, lParam: power, cadence x2
constexpr UINT WM_LINK_STATE = WM_APP + 2;      // wParam: Role, lParam: Link::State
constexpr UINT WM_RIDE_BUTTONS = WM_APP + 3;    // lParam: pressed button bits
constexpr UINT WM_HEART_RATE = WM_APP + 4;      // wParam: beats per minute, 0 when the strap has nothing
constexpr UINT WM_RADIO_STATE = WM_APP + 5;     // wParam: Bluetooth::State
constexpr UINT WM_NEARBY_CHANGED = WM_APP + 6;  // the list of devices nearby changed
constexpr UINT WM_BLE_LAST = WM_NEARBY_CHANGED;

// What a device is for. Also the order of the devices on screen and in the settings.
enum class Role { Trainer, Shifters, HeartRate };
constexpr size_t kRoleCount = 3;

// A 16-bit UUID from the Bluetooth base UUID.
winrt::guid ShortUuid(uint16_t id);

// A device name from the air without control characters and cut to a sane length, safe to show and save.
std::wstring CleanName(std::wstring_view name);

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
