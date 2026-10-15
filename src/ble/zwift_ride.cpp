#include "ble/zwift_ride.h"

namespace ble {
namespace {

using namespace std::chrono_literals;
using Cccd = gatt::GattClientCharacteristicConfigurationDescriptorValue;

// The protocol isn't published. These come from open-source apps that talk to the Ride, such as BikeControl.
constexpr uint16_t kZwiftCompanyId = 0x094A;
constexpr uint8_t kRideLeftSide = 0x08;  // first byte of the manufacturer data
const winrt::guid kRideService = ShortUuid(0xFC82);
const winrt::guid kLegacyService{0x00000001, 0x19CA, 0x4651, {0x86, 0xE5, 0xFA, 0x29, 0xDC, 0xDD, 0x09, 0xD1}};
const winrt::guid kAsync{0x00000002, 0x19CA, 0x4651, {0x86, 0xE5, 0xFA, 0x29, 0xDC, 0xDD, 0x09, 0xD1}};
const winrt::guid kSyncRx{0x00000003, 0x19CA, 0x4651, {0x86, 0xE5, 0xFA, 0x29, 0xDC, 0xDD, 0x09, 0xD1}};
const winrt::guid kSyncTx{0x00000004, 0x19CA, 0x4651, {0x86, 0xE5, 0xFA, 0x29, 0xDC, 0xDD, 0x09, 0xD1}};
constexpr uint8_t kButtonMessage = 0x23;

}  // namespace

bool IsRideLeft(const adv::BluetoothLEAdvertisement& ad) {
    for (const auto& data : ad.ManufacturerData()) {
        if (data.CompanyId() != kZwiftCompanyId) continue;
        const auto bytes = data.Data();
        if (bytes.Length() > 0 && bytes.data()[0] == kRideLeftSide) return true;
    }
    return false;
}

Controllers::Controllers(HWND window, uint64_t address, std::wstring name)
    : Link(window, Role::Shifters, IsRideLeft, true, address, std::move(name)) {
    Start();
}

Controllers::~Controllers() { Stop(); }

IAsyncOperation<bool> Controllers::Session(Connection& link) {
    link.service = co_await FindService(link.device, kRideService, link.fresh);
    if (!link.service) link.service = co_await FindService(link.device, kLegacyService, link.fresh);
    if (!link.service) {
        SetState(State::Locked);
        co_await winrt::resume_on_signal(Nudge(), 30s);
        co_return true;
    }

    const auto async = co_await FindCharacteristic(link.service, kAsync, link.fresh);
    const auto syncRx = co_await FindCharacteristic(link.service, kSyncRx, link.fresh);
    const auto syncTx = co_await FindCharacteristic(link.service, kSyncTx, link.fresh);
    if (!async || !syncRx || !syncTx) co_return false;

    auto handler = [this](const auto&, const gatt::GattValueChangedEventArgs& args) { OnMessage(args.CharacteristicValue()); };
    const auto asyncRevoker = async.ValueChanged(winrt::auto_revoke, handler);
    const auto syncRevoker = syncTx.ValueChanged(winrt::auto_revoke, handler);
    if (!co_await Subscribe(async, Cccd::Notify) || !co_await Subscribe(syncTx, Cccd::Indicate)) co_return false;
    if (!co_await Write(syncRx, {'R', 'i', 'd', 'e', 'O', 'n'}, false)) co_return false;

    SetState(State::Connected);
    while (!Changed() && Connected(link.device)) co_await winrt::resume_on_signal(Nudge(), 1s);
    co_return true;
}

// A button message is the type byte, then a protobuf whose field 1 is the button bits, each clear while pressed.
void Controllers::OnMessage(const winrt::Windows::Storage::Streams::IBuffer& buffer) {
    const uint8_t* p = buffer.data();
    const size_t n = buffer.Length();
    if (n < 3 || p[0] != kButtonMessage || p[1] != 0x08) return;  // 0x08: field 1, varint
    uint64_t bits = 0;
    for (size_t i = 2, shift = 0; i < n && shift < 64; ++i, shift += 7) {
        bits |= static_cast<uint64_t>(p[i] & 0x7F) << shift;
        if (!(p[i] & 0x80)) {
            Post(WM_RIDE_BUTTONS, 0, static_cast<LPARAM>(~bits & 0xFFFF));
            return;
        }
    }
}

}  // namespace ble
