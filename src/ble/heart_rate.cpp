#include "ble/heart_rate.h"

#include <string_view>

namespace ble {
namespace {

using namespace std::chrono_literals;
using Cccd = gatt::GattClientCharacteristicConfigurationDescriptorValue;

const winrt::guid kHeartRateService = ShortUuid(0x180D);
const winrt::guid kMeasurement = ShortUuid(0x2A37);

constexpr ULONGLONG kStaleMs = 5000;  // straps send every second while they touch skin

}  // namespace

bool IsHeartRateMonitor(const adv::BluetoothLEAdvertisement& ad) {
    for (const auto& id : ad.ServiceUuids())
        if (id == kHeartRateService) return true;
    return std::wstring_view(ad.LocalName()).starts_with(L"Polar H");
}

HeartRateMonitor::HeartRateMonitor(HWND window, uint64_t address, std::wstring name)
    : Link(window, Role::HeartRate, IsHeartRateMonitor, false, address, std::move(name)) {
    Start();
}

HeartRateMonitor::~HeartRateMonitor() { Stop(); }

IAsyncOperation<bool> HeartRateMonitor::Session(Connection& link) {
    link.service = co_await FindService(link.device, kHeartRateService, link.fresh);
    if (!link.service) co_return false;
    const auto measurement = co_await FindCharacteristic(link.service, kMeasurement, link.fresh);
    if (!measurement) co_return false;

    const auto revoker = measurement.ValueChanged(winrt::auto_revoke, [this](const auto&, const gatt::GattValueChangedEventArgs& args) {
        OnMeasurement(args.CharacteristicValue());
    });
    if (!co_await Subscribe(measurement, Cccd::Notify)) co_return false;

    lastData_ = GetTickCount64();
    SetState(State::Connected);
    while (!Changed() && Connected(link.device)) {
        co_await winrt::resume_on_signal(Nudge(), 1s);
        // Off the chest, a strap goes quiet. Show nothing rather than an old number.
        if (GetTickCount64() - lastData_ > kStaleMs) Post(WM_HEART_RATE, 0);
    }
    co_return true;
}

// Heart Rate Measurement: a flags byte, then the rate as a uint8, or a uint16 when bit 0 is set.
void HeartRateMonitor::OnMeasurement(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const uint8_t* p = buffer.data();
    const size_t n = buffer.Length();
    if (n < 2) return;
    int bpm = p[1];
    if (p[0] & 0x01) {
        if (n < 3) return;
        bpm |= p[2] << 8;
    }
    if (bpm >= 300) return;
    lastData_ = GetTickCount64();
    Post(WM_HEART_RATE, static_cast<WPARAM>(bpm));
}

}  // namespace ble
