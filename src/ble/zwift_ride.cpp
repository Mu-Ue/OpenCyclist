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

bool IsRideLeft(const adv::BluetoothLEAdvertisement& ad) {
    for (const auto& data : ad.ManufacturerData()) {
        if (data.CompanyId() != kZwiftCompanyId) continue;
        const auto bytes = data.Data();
        if (bytes.Length() > 0 && bytes.data()[0] == kRideLeftSide) return true;
    }
    return false;
}

}  // namespace

Controllers::Controllers(HWND window) : window_(window) { Run(); }

Controllers::~Controllers() {
    stop_ = true;
    SetEvent(wake_.get());
    WaitForSingleObject(done_.get(), 5000);
}

winrt::fire_and_forget Controllers::Run() {
    co_await winrt::resume_background();
    while (!stop_) {
        try {
            co_await Session();
        } catch (...) {
        }
        if (stop_) break;
        Post(WM_RIDE_BUTTONS, 0, 0);  // release anything held when it dropped
        co_await winrt::resume_on_signal(wake_.get(), 2s);
    }
    SetEvent(done_.get());
}

IAsyncAction Controllers::Session() {
    if (!co_await RadioOn()) {
        co_await winrt::resume_on_signal(wake_.get(), 3s);
        co_return;
    }
    const uint64_t address = co_await Scan(IsRideLeft, 10s, wake_.get());
    if (address == 0) {
        Post(WM_CONTROLLER_STATE, static_cast<WPARAM>(State::Searching));
        co_return;
    }

    Connection link;
    link.device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(address);
    if (!link.device) co_return;
    link.service = co_await FindService(link.device, kRideService);
    if (!link.service) link.service = co_await FindService(link.device, kLegacyService);
    if (!link.service) {
        Post(WM_CONTROLLER_STATE, static_cast<WPARAM>(State::Locked));
        co_await winrt::resume_on_signal(wake_.get(), 30s);
        co_return;
    }

    const auto async = co_await FindCharacteristic(link.service, kAsync);
    const auto syncRx = co_await FindCharacteristic(link.service, kSyncRx);
    const auto syncTx = co_await FindCharacteristic(link.service, kSyncTx);
    if (!async || !syncRx || !syncTx) co_return;

    auto handler = [this](const auto&, const gatt::GattValueChangedEventArgs& args) { OnMessage(args.CharacteristicValue()); };
    const auto asyncRevoker = async.ValueChanged(winrt::auto_revoke, handler);
    const auto syncRevoker = syncTx.ValueChanged(winrt::auto_revoke, handler);
    if (!co_await Subscribe(async, Cccd::Notify) || !co_await Subscribe(syncTx, Cccd::Indicate)) co_return;
    if (!co_await Write(syncRx, {'R', 'i', 'd', 'e', 'O', 'n'}, false)) co_return;

    Post(WM_CONTROLLER_STATE, static_cast<WPARAM>(State::Connected));
    while (!stop_ && Connected(link.device)) co_await winrt::resume_on_signal(wake_.get(), 1s);
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
