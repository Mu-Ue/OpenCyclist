#include "ble/bluetooth.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>

#include <winrt/Windows.Devices.Radios.h>

#include "ble/ftms.h"
#include "ble/heart_rate.h"
#include "ble/zwift_ride.h"

namespace ble {

namespace radios = winrt::Windows::Devices::Radios;
using winrt::Windows::Devices::Bluetooth::BluetoothAdapter;

namespace {

constexpr size_t kMaxNearby = 32;  // more than any room has, and a bound on what a flood of adverts can fill

std::optional<Role> RoleOf(const adv::BluetoothLEAdvertisement& ad) {
    if (IsTrainer(ad)) return Role::Trainer;
    if (IsRideLeft(ad)) return Role::Shifters;
    if (IsHeartRateMonitor(ad)) return Role::HeartRate;
    return std::nullopt;
}

WPARAM StateOf(radios::RadioState state) {
    return static_cast<WPARAM>(state == radios::RadioState::On ? Bluetooth::State::On : Bluetooth::State::Off);
}

}  // namespace

struct Bluetooth::Shared {
    HWND window = nullptr;
    std::atomic<bool> closed{false};
    mutable std::mutex mutex;
    std::vector<Device> devices;
    radios::Radio radio{nullptr};
    radios::Radio::StateChanged_revoker stateChanged;
};

Bluetooth::Bluetooth(HWND window) : shared_(std::make_shared<Shared>()) {
    shared_->window = window;
    WatchRadio(shared_);
}

Bluetooth::~Bluetooth() {
    Scan(false);
    shared_->closed = true;
    std::lock_guard lock(shared_->mutex);
    shared_->stateChanged.revoke();
}

winrt::fire_and_forget Bluetooth::WatchRadio(std::shared_ptr<Shared> shared) {
    radios::Radio radio{nullptr};
    try {
        const auto adapter = co_await BluetoothAdapter::GetDefaultAsync();
        if (adapter && adapter.IsLowEnergySupported()) radio = co_await adapter.GetRadioAsync();
    } catch (...) {
    }
    std::lock_guard lock(shared->mutex);
    if (shared->closed) co_return;
    if (!radio) {
        PostMessageW(shared->window, WM_RADIO_STATE, static_cast<WPARAM>(State::Missing), 0);
        co_return;
    }
    shared->radio = radio;
    shared->stateChanged = radio.StateChanged(winrt::auto_revoke, [window = shared->window](const radios::Radio& sender, const auto&) {
        PostMessageW(window, WM_RADIO_STATE, StateOf(sender.State()), 0);
    });
    PostMessageW(shared->window, WM_RADIO_STATE, StateOf(radio.State()), 0);
}

winrt::fire_and_forget Bluetooth::TurnOnRadio(std::shared_ptr<Shared> shared) {
    const winrt::apartment_context ui;
    radios::Radio radio{nullptr};
    {
        std::lock_guard lock(shared->mutex);
        radio = shared->radio;
    }
    bool on = false;
    if (radio) {
        try {
            if (co_await radios::Radio::RequestAccessAsync() == radios::RadioAccessStatus::Allowed)
                on = co_await radio.SetStateAsync(radios::RadioState::On) == radios::RadioAccessStatus::Allowed;
        } catch (...) {
        }
    }
    // Windows can deny desktop apps the radio, as can airplane mode, and a missing adapter has none to switch.
    co_await ui;
    if (!on && !shared->closed)
        ShellExecuteW(nullptr, nullptr, L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
}

void Bluetooth::Scan(bool on) {
    if (watcher_) {
        try {
            watcher_.Stop();
        } catch (...) {
        }
        watcher_ = nullptr;
    }
    {
        std::lock_guard lock(shared_->mutex);
        shared_->devices.clear();
    }
    PostMessageW(shared_->window, WM_NEARBY_CHANGED, 0, 0);
    if (!on) return;

    watcher_ = adv::BluetoothLEAdvertisementWatcher();
    watcher_.ScanningMode(adv::BluetoothLEScanningMode::Active);  // scan responses carry the device names
    // A device may tell what it is in one packet and its name in another, so both update the list.
    watcher_.Received([shared = shared_](const auto&, const adv::BluetoothLEAdvertisementReceivedEventArgs& args) {
        const auto ad = args.Advertisement();
        const uint64_t address = args.BluetoothAddress();
        std::wstring name = CleanName(ad.LocalName());
        const std::optional<Role> role = RoleOf(ad);
        {
            std::lock_guard lock(shared->mutex);
            auto& list = shared->devices;
            const auto it = std::find_if(list.begin(), list.end(), [&](const Device& d) { return d.address == address; });
            if (it == list.end()) {
                if (!role || list.size() >= kMaxNearby) return;
                list.push_back({address, *role, std::move(name)});
            } else if (!name.empty() && name != it->name) {
                it->name = std::move(name);
            } else {
                return;
            }
        }
        PostMessageW(shared->window, WM_NEARBY_CHANGED, 0, 0);
    });
    try {
        watcher_.Start();
    } catch (...) {
        // The radio is off. The app scans again when it comes on.
    }
}

std::vector<Bluetooth::Device> Bluetooth::Nearby() const {
    std::lock_guard lock(shared_->mutex);
    return shared_->devices;
}

}  // namespace ble
