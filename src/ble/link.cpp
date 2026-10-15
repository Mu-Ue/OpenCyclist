#include "ble/link.h"

namespace ble {

using namespace std::chrono_literals;

Link::Link(HWND window, Role role, Matcher matches, uint64_t address, std::wstring name)
    : window_(window), role_(role), matches_(matches), address_(address), name_(std::move(name)) {}

void Link::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        SetEvent(wake_.get());
    }
    WaitForSingleObject(done_.get(), 5000);
}

void Link::Use(uint64_t address, std::wstring name) {
    std::lock_guard lock(mutex_);
    address_ = address;
    name_ = std::move(name);
    ++generation_;
    SetEvent(wake_.get());
}

std::wstring Link::Name() const {
    std::lock_guard lock(mutex_);
    return name_;
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
        }
        if (!Changed()) co_await winrt::resume_on_signal(wake_.get(), 2s);
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

    // Try the saved address first. If it keeps failing, the device may have a new address, so look for one like
    // it, with the same name when the advert carries one.
    if (failures_ >= 2) {
        SetState(State::Missing);
        address = co_await Scan(
            [matches = matches_, name](const adv::BluetoothLEAdvertisement& ad) {
                if (!matches(ad)) return false;
                const std::wstring local = CleanName(ad.LocalName());
                return name.empty() || local.empty() || local == name;
            },
            10s, wake_.get());
        if (address == 0) co_return;
    }
    SetState(State::Connecting);

    Connection link;
    link.device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(address);
    if (link.device && name.empty()) {
        // A device remembered without a name, such as from an older version, gets the one Windows knows.
        if (std::wstring known = CleanName(link.device.Name()); !known.empty()) {
            std::lock_guard lock(mutex_);
            if (!Changed()) name_ = std::move(known);
        }
    }
    if (link.device && co_await Session(link)) failures_ = 0;
    else ++failures_;
}

}  // namespace ble
