#include "ble/link.h"

namespace ble {

using namespace std::chrono_literals;
namespace bt = winrt::Windows::Devices::Bluetooth;

Link::Link(HWND window, Role role, Matcher matches, bool lowLatency, uint64_t address, std::wstring name)
    : window_(window), role_(role), matches_(matches), lowLatency_(lowLatency), address_(address), name_(std::move(name)) {}

void Link::RequestStop() {
    std::lock_guard lock(mutex_);
    stop_ = true;
    SetEvent(wake_.get());
    SetEvent(nudge_.get());
}

void Link::Stop() {
    RequestStop();
    WaitForSingleObject(done_.get(), 5000);
}

void Link::Use(uint64_t address, std::wstring name) {
    std::lock_guard lock(mutex_);
    address_ = address;
    name_ = std::move(name);
    ++generation_;
    SetEvent(wake_.get());
    SetEvent(nudge_.get());
}

std::wstring Link::Name() const {
    std::lock_guard lock(mutex_);
    return name_;
}

uint64_t Link::Address() const {
    std::lock_guard lock(mutex_);
    return address_;
}

void Link::SetState(State state) const {
    if (!Changed()) Post(WM_LINK_STATE, static_cast<WPARAM>(role_), static_cast<LPARAM>(state));
}

winrt::fire_and_forget Link::Run() {
    co_await winrt::resume_background();
    for (;;) {
        uint64_t address;
        std::wstring name;
        {
            // Wake is set only with the lock held and only when Changed turns true, so clearing it here can't
            // lose a change, and a session never sees it set for nothing.
            std::lock_guard lock(mutex_);
            if (stop_) break;
            ResetEvent(wake_.get());
            if (session_ != generation_) session_ = generation_, failures_ = 0;
            address = address_;
            name = name_;
        }
        try {
            co_await Connect(address, std::move(name));
        } catch (...) {
            // A dropped connection surfaces as an exception from whichever call was in flight. Start over.
            ++failures_;
        }
        // Straight back after a drop, so a blip costs little. A failed try has already waited for the device.
        if (!Changed()) co_await winrt::resume_on_signal(wake_.get(), failures_ ? 1s : 250ms);
    }
    SetEvent(done_.get());
}

IAsyncAction Link::Connect(uint64_t address, std::wstring name) {
    if (address == 0) {
        SetState(State::Idle);
        co_await winrt::resume_on_signal(wake_.get());
        co_return;
    }
    if (!co_await RadioOn()) {
        SetState(State::NoBluetooth);
        co_await winrt::resume_on_signal(wake_.get(), 3s);
        co_return;
    }

    // Connecting straight away is quickest and costs the least: the radio itself watches for the address and
    // Windows connects the moment the device advertises. A device that keeps failing may have moved to a new
    // address, so every third try instead looks for an advert like it, from its address or with its name.
    Connection link;
    link.fresh = failures_ > 0;
    link.device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(address);
    SetState(failures_ >= 2 ? State::Missing : State::Connecting);
    if ((!link.device || !Connected(link.device)) && failures_ % 3 == 2) {
        const uint64_t found = co_await Scan(
            [matches = matches_, address, name](uint64_t from, const adv::BluetoothLEAdvertisement& ad) {
                return from == address || (!name.empty() && matches(ad) && CleanName(ad.LocalName()) == name);
            },
            10s, wake_.get());
        if (found == 0) {
            if (!Changed()) ++failures_;
            co_return;
        }
        if (found != address) {
            {
                std::lock_guard lock(mutex_);
                if (Changed()) co_return;
                address_ = found;
            }
            link.device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(found);
        }
        SetState(State::Connecting);
    }
    if (!link.device) {
        ++failures_;
        co_return;
    }
    if (name.empty()) {
        // A device remembered without a name, such as from an older version, gets the one Windows knows.
        if (std::wstring known = CleanName(link.device.Name()); !known.empty()) {
            std::lock_guard lock(mutex_);
            if (!Changed()) name_ = std::move(known);
        }
    }
    // Windows keeps the link up between messages and brings it back after a blip on its own (Windows 10 1803 and
    // newer), and on Windows 11 a low-latency link can be asked for. Older Windows lacks these, which is fine.
    try {
        link.session = co_await gatt::GattSession::FromDeviceIdAsync(link.device.BluetoothDeviceId());
        if (link.session) link.session.MaintainConnection(true);
    } catch (...) {
    }
    if (lowLatency_) {
        try {
            link.fast = link.device.RequestPreferredConnectionParameters(
                bt::BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
        } catch (...) {
        }
    }
    if (co_await Session(link)) failures_ = 0;
    else ++failures_;
}

}  // namespace ble
