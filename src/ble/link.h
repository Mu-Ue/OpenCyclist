#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "ble/common.h"

namespace ble {

// The device for one role. Connects to it on its own and reconnects when it drops, until it is told to use
// another device or none. Posts its state to the window.
class Link {
public:
    enum class State { Idle, NoBluetooth, Connecting, Missing, Connected, Locked };

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Switches to another device, or to none with address 0. The same device again retries it straight away.
    void Use(uint64_t address, std::wstring name);
    std::wstring Name() const;

protected:
    using Matcher = bool (*)(const adv::BluetoothLEAdvertisement&);

    Link(HWND window, Role role, Matcher matches, uint64_t address, std::wstring name);
    ~Link() = default;

    // Derived classes call Start at the end of their constructor and Stop at the start of their destructor, so
    // a session never runs on a half-built or half-destroyed object.
    void Start() { Run(); }
    void Stop();

    // Uses the connected device until it drops or Changed. Returns false when the device lacks the service,
    // which counts as a failed connect.
    virtual IAsyncOperation<bool> Session(Connection& link) = 0;

    // The app is closing or wants another device. Wake is set when this turns true.
    bool Changed() const { return stop_ || generation_ != session_; }
    HANDLE Wake() const { return wake_.get(); }
    void SetState(State state) const;
    void Post(UINT message, WPARAM w, LPARAM l = 0) const { PostMessageW(window_, message, w, l); }

private:
    winrt::fire_and_forget Run();
    IAsyncAction Connect(uint64_t address, std::wstring name);

    HWND window_;
    Role role_;
    Matcher matches_;
    mutable std::mutex mutex_;
    uint64_t address_;
    std::wstring name_;
    std::atomic<unsigned> generation_{0};  // counts the calls to Use
    unsigned session_ = 0;                 // the generation the running session belongs to
    int failures_ = 0;                     // connects that failed in a row
    std::atomic<bool> stop_{false};
    winrt::handle wake_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    winrt::handle done_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

}  // namespace ble
