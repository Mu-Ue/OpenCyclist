#include "ble/ftms.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string_view>

#include "config.h"

namespace ble {
namespace {

using namespace std::chrono_literals;
using Cccd = gatt::GattClientCharacteristicConfigurationDescriptorValue;

const winrt::guid kFtmsService = ShortUuid(0x1826);
const winrt::guid kIndoorBikeData = ShortUuid(0x2AD2);
const winrt::guid kResistanceRange = ShortUuid(0x2AD6);
const winrt::guid kControlPoint = ShortUuid(0x2AD9);

// Control point opcodes.
constexpr uint8_t kRequestControl = 0x00, kReset = 0x01, kSetResistance = 0x04, kSetPower = 0x05, kStart = 0x07;

constexpr ULONGLONG kDataTimeoutMs = 10000;  // trainers send bike data every second, even when idle

bool SameTarget(TrainerTarget a, TrainerTarget b) {
    return a.kind == b.kind && std::abs(a.value - b.value) < (a.kind == TrainerTarget::Kind::Power ? 2 : 1);
}

}  // namespace

bool IsTrainer(const adv::BluetoothLEAdvertisement& ad) {
    for (const auto& id : ad.ServiceUuids())
        if (id == kFtmsService) return true;
    // Some trainers only list their own services in the advert.
    return std::wstring_view(ad.LocalName()).starts_with(L"KICKR");
}

Trainer::Trainer(HWND window, uint64_t address, std::wstring name)
    : Link(window, Role::Trainer, IsTrainer, address, std::move(name)) {
    Start();
}

Trainer::~Trainer() { Stop(); }

void Trainer::SetTarget(TrainerTarget target) {
    std::lock_guard lock(mutex_);
    target_ = target;
}

IAsyncOperation<bool> Trainer::Session(Connection& link) {
    link.service = co_await FindService(link.device, kFtmsService);
    if (!link.service) co_return false;

    const auto data = co_await FindCharacteristic(link.service, kIndoorBikeData);
    const auto control = co_await FindCharacteristic(link.service, kControlPoint);
    if (!data || !control) co_return true;

    // Resistance range: min, max (sint16) and step (uint16), in 0.1 units.
    if (const auto range = co_await FindCharacteristic(link.service, kResistanceRange)) {
        const auto read = co_await range.ReadValueAsync(winrt::Windows::Devices::Bluetooth::BluetoothCacheMode::Uncached);
        if (read.Status() == gatt::GattCommunicationStatus::Success && read.Value().Length() >= 4) {
            const uint8_t* p = read.Value().data();
            const int lo = static_cast<int16_t>(p[0] | p[1] << 8), hi = static_cast<int16_t>(p[2] | p[3] << 8);
            if (hi > lo) minLevel_ = std::max(lo, 0), maxLevel_ = std::min(hi, 255);
        }
    }

    const auto dataRevoker = data.ValueChanged(winrt::auto_revoke, [this](const auto&, const gatt::GattValueChangedEventArgs& args) {
        OnBikeData(args.CharacteristicValue());
    });
    if (!co_await Subscribe(data, Cccd::Notify) || !co_await Subscribe(control, Cccd::Indicate)) co_return true;
    if (!co_await Write(control, {kRequestControl}, true)) co_return true;
    co_await Write(control, {kStart}, true);

    lastData_ = GetTickCount64();
    SetState(State::Connected);

    bool sent = false;
    TrainerTarget last;
    while (!Changed() && Connected(link.device) && GetTickCount64() - lastData_ < kDataTimeoutMs) {
        TrainerTarget target;
        {
            std::lock_guard lock(mutex_);
            target = target_;
        }
        if (!sent || !SameTarget(target, last)) {
            sent = co_await Send(control, target);
            last = target;
        }
        co_await winrt::resume_on_signal(Wake(), std::chrono::milliseconds(kTickMs));
    }
    // Hand the trainer back free-wheeling when the app closes or switches to another one.
    if (Changed()) co_await Write(control, {kReset}, true);
    co_return true;
}

IAsyncOperation<bool> Trainer::Send(gatt::GattCharacteristic control, TrainerTarget target) {
    if (target.kind == TrainerTarget::Kind::Power) {
        const auto watts = static_cast<int16_t>(std::clamp(target.value, 0, kMaxTargetW));
        co_return co_await Write(control, {kSetPower, static_cast<uint8_t>(watts), static_cast<uint8_t>(watts >> 8)}, true);
    }
    const int level = minLevel_ + (maxLevel_ - minLevel_) * std::clamp(target.value, 0, 100) / 100;
    co_return co_await Write(control, {kSetResistance, static_cast<uint8_t>(std::clamp(level, 0, 255))}, true);
}

// Indoor Bike Data: a flags word, then only the fields the flags list, in a fixed order.
void Trainer::OnBikeData(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const uint8_t* p = buffer.data();
    const size_t n = buffer.Length();
    if (n < 2) return;
    const unsigned flags = p[0] | p[1] << 8;
    size_t at = 2;
    auto skip = [&](unsigned bit, size_t bytes) {
        if (flags & bit) at += bytes;
    };
    if (!(flags & 0x01)) at += 2;  // instantaneous speed, present when "more data" is clear
    skip(0x02, 2);                 // average speed
    int cadence = -1, power = INT_MIN;
    if ((flags & 0x04) && at + 2 <= n) cadence = p[at] | p[at + 1] << 8;  // 0.5 rpm
    skip(0x04, 2);
    skip(0x08, 2);  // average cadence
    skip(0x10, 3);  // total distance
    skip(0x20, 2);  // resistance level
    if ((flags & 0x40) && at + 2 <= n) power = static_cast<int16_t>(p[at] | p[at + 1] << 8);

    WPARAM has = 0;
    if (cadence >= 0) has |= kHasCadence;
    if (power != INT_MIN) has |= kHasPower;
    lastData_ = GetTickCount64();
    if (has)
        Post(WM_TRAINER_DATA, has,
             MAKELPARAM(static_cast<uint16_t>(std::max(power, 0)), static_cast<uint16_t>(std::max(cadence, 0))));
}

}  // namespace ble
