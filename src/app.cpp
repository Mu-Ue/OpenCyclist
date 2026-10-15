#include "app.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <winrt/base.h>

#include "ble/bluetooth.h"
#include "ble/ftms.h"
#include "ble/heart_rate.h"
#include "ble/zwift_ride.h"
#include "config.h"
#include "sim/course.h"
#include "sim/ride.h"
#include "ui/renderer.h"

namespace {

constexpr UINT_PTR kTimer = 1;
constexpr ULONGLONG kConfirmMs = 3000;  // Esc ends a ride when pressed twice within this
constexpr ULONGLONG kContinueMs = 3000;  // the devices screen shows all connected this long before moving on
constexpr ULONGLONG kSearchMs = 4000;    // the Refresh button shows the search this long
constexpr int kCards = static_cast<int>(ble::kRoleCount);  // the devices screen's first items, one per role

std::wstring_view RoleName(ble::Role role) {
    switch (role) {
    case ble::Role::Trainer: return L"Trainer";
    case ble::Role::Shifters: return L"Shifters";
    case ble::Role::HeartRate: return L"Heart rate";
    }
    return {};
}

// The settings on the setup screen, top to bottom. Target shows only in the ERG and resistance modes.
enum class Field { Km, Gain, MaxGrade, Route, Mode, Target, Rider, Bike };

// The next value up or down from v on a grid of `step`, so odd values from the ini snap onto it.
double StepValue(double v, int dir, double step, double lo, double hi) {
    const double n = dir > 0 ? std::floor(v / step + 1e-9) + 1 : std::ceil(v / step - 1e-9) - 1;
    return std::clamp(n * step, lo, hi);
}

class App {
public:
    explicit App(Settings settings) : settings_(std::move(settings)) {}
    int Run(HINSTANCE instance);

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);

    std::vector<Field> Fields() const;
    std::wstring_view Label(Field field) const;
    std::wstring Value(Field field) const;
    void Change(Field field, int dir);
    void Select(int dir);
    void MakeCourse();
    void Summarize();
    void Reroll();
    void KeepRideSettings();
    void Start();
    void NewRide();
    void EndRide();
    std::wstring Status() const;

    // The devices screen.
    void ShowDevices();
    void ShowCourse();
    ble::Link& LinkFor(ble::Role role);
    SavedDevice& Saved(ble::Role role) { return settings_.devices[static_cast<size_t>(role)]; }
    const SavedDevice& Saved(ble::Role role) const { return settings_.devices[static_cast<size_t>(role)]; }
    void UseDevice(ble::Role role, uint64_t address, std::wstring name);
    void OnLinkState(ble::Role role, ble::Link::State state);
    void UpdateNearby();
    int Items() const;
    void Focus(int dir);
    void Act();
    void Refresh();
    bool AllConnected() const;
    DeviceInfo Device(ble::Role role) const;
    void OnDevicesKey(WPARAM key);
    void OnDevicesClick(D2D1_POINT_2F p);

    void OnKey(WPARAM key);
    void OnClick(D2D1_POINT_2F p);
    void OnWheel(D2D1_POINT_2F p, int dir);
    void OnButtons(uint32_t pressed);
    bool Held(uint32_t button, ULONGLONG now);
    void OnTick();

    void Paint();
    void ToggleFullscreen();
    D2D1_SIZE_F Size() const;
    D2D1_POINT_2F ToDips(POINT p) const;
    void Redraw() const { InvalidateRect(hwnd_, nullptr, FALSE); }

    void WriteLog();
    void CloseLog();
    void Shutdown();

    Settings settings_;
    HWND hwnd_ = nullptr;
    winrt::com_ptr<ID2D1Factory> d2d_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<ID2D1HwndRenderTarget> target_;
    std::unique_ptr<Renderer> renderer_;
    bool fullscreen_ = false;
    WINDOWPLACEMENT placement_{sizeof(placement_)};
    int wheel_ = 0;  // wheel delta not yet turned into steps, for touchpads

    Course course_;  // on the setup screen, and the one being ridden
    std::wstring summary_;
    int selected_ = 0;
    std::unique_ptr<Ride> ride_;  // null on the setup and devices screens
    ULONGLONG escAt_ = 0;

    bool devices_ = false;  // the devices screen shows instead of the setup screen
    std::vector<ble::Bluetooth::Device> nearby_;  // found nearby and not in use
    int focus_ = 0;  // on the devices screen: the cards, then the nearby devices
    bool autoContinue_ = false;  // move on by itself once the remembered devices connect
    ULONGLONG continueAt_ = 0;
    ULONGLONG refreshedAt_ = 0;  // when Refresh was pressed, to show the search for a moment

    std::unique_ptr<ble::Bluetooth> bluetooth_;
    std::unique_ptr<ble::Trainer> trainer_;
    std::unique_ptr<ble::Controllers> controllers_;
    std::unique_ptr<ble::HeartRateMonitor> monitor_;
    ble::Bluetooth::State radio_ = ble::Bluetooth::State::Unknown;
    ble::Link::State states_[ble::kRoleCount]{};
    int power_ = 0;
    double cadence_ = 0.0;
    int bpm_ = 0;
    uint32_t buttons_ = 0;
    // Buttons that act once when held: A rerolls a ride that has just started, B ends a ride.
    struct Hold {
        uint32_t button;
        ULONGLONG since = 0;
        bool done = true;
    } holds_[2] = {{ble::button::kA}, {ble::button::kB}};
    ULONGLONG lastTick_ = 0;
    FILE* log_ = nullptr;
    double logClock_ = 0.0;
};

int App::Run(HINSTANCE instance) {
    try {
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(dwrite_.put())));
        renderer_ = std::make_unique<Renderer>(dwrite_.get());
    } catch (...) {
        return 1;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kAppName;
    RegisterClassExW(&wc);

    // 1280 x 720 at the system scale, shrunk to fit small screens.
    const UINT dpi = GetDpiForSystem();
    RECT rc{0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min<int>(rc.right - rc.left, work.right - work.left);
    const int height = std::min<int>(rc.bottom - rc.top, work.bottom - work.top);
    hwnd_ = CreateWindowExW(0, kAppName, kAppName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) return 1;
    const BOOL dark = TRUE;  // a dark title bar to match; ignored before Windows 10 20H1
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    if (!settings_.course.seed) settings_.course.seed = RandomSeed();
    MakeCourse();
    const SavedDevice* saved = settings_.devices;
    bluetooth_ = std::make_unique<ble::Bluetooth>(hwnd_);
    trainer_ = std::make_unique<ble::Trainer>(hwnd_, saved[0].address, saved[0].name);
    controllers_ = std::make_unique<ble::Controllers>(hwnd_, saved[1].address, saved[1].name);
    monitor_ = std::make_unique<ble::HeartRateMonitor>(hwnd_, saved[2].address, saved[2].name);
    // Start on the devices screen. Remembered devices connect by themselves, and once they all have, the
    // course screen follows.
    ShowDevices();
    autoContinue_ = std::any_of(saved, saved + ble::kRoleCount, [](const SavedDevice& d) { return d.address != 0; });
    lastTick_ = GetTickCount64();
    SetTimer(hwnd_, kTimer, kTickMs, nullptr);
    ShowWindow(hwnd_, SW_SHOWDEFAULT);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

LRESULT CALLBACK App::Proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app && app->hwnd_ ? app->Handle(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    // After shutting down for a Windows sign-out, Bluetooth messages may still be queued.
    if (!trainer_ && msg >= WM_APP && msg <= ble::WM_BLE_LAST) return 0;
    switch (msg) {
    case WM_TIMER:
        OnTick();
        return 0;
    case ble::WM_TRAINER_DATA:
        if (wParam & ble::kHasPower) power_ = LOWORD(lParam);
        if (wParam & ble::kHasCadence) cadence_ = HIWORD(lParam) / 2.0;
        if (ride_) ride_->OnPower(power_), ride_->OnCadence(cadence_);
        return 0;
    case ble::WM_HEART_RATE:
        bpm_ = static_cast<int>(wParam);
        if (ride_ && ride_->heartRate() >= 0) ride_->OnHeartRate(bpm_);
        return 0;
    case ble::WM_LINK_STATE:
        if (wParam < ble::kRoleCount) OnLinkState(static_cast<ble::Role>(wParam), static_cast<ble::Link::State>(lParam));
        return 0;
    case ble::WM_RADIO_STATE:
        radio_ = static_cast<ble::Bluetooth::State>(wParam);
        // Scanning stops with the radio, so it starts again when the radio comes back.
        if (radio_ == ble::Bluetooth::State::On && devices_ && !ride_) bluetooth_->Scan(true);
        Redraw();
        return 0;
    case ble::WM_NEARBY_CHANGED:
        UpdateNearby();
        Redraw();
        return 0;
    case ble::WM_RIDE_BUTTONS:
        OnButtons(static_cast<uint32_t>(lParam));
        return 0;
    case WM_KEYDOWN:
        OnKey(wParam);
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN) {  // Alt+Enter
            ToggleFullscreen();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        OnClick(ToDips({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}));
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &pt);
        wheel_ += GET_WHEEL_DELTA_WPARAM(wParam);
        while (wheel_ >= WHEEL_DELTA || wheel_ <= -WHEEL_DELTA) {
            const int dir = wheel_ > 0 ? 1 : -1;
            wheel_ -= dir * WHEEL_DELTA;
            OnWheel(ToDips(pt), dir);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;  // Direct2D paints every pixel
    case WM_SIZE:
        if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        Redraw();
        return 0;
    case WM_DPICHANGED: {
        const float dpi = LOWORD(wParam);
        if (target_) target_->SetDpi(dpi, dpi);
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        const UINT dpi = GetDpiForWindow(hwnd_);
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {MulDiv(800, dpi, 96), MulDiv(450, dpi, 96)};
        return 0;
    }
    case WM_ENDSESSION:
        if (wParam) Shutdown();
        return 0;
    case WM_DESTROY:
        Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

std::vector<Field> App::Fields() const {
    std::vector<Field> fields{Field::Km, Field::Gain, Field::MaxGrade, Field::Route, Field::Mode};
    if (settings_.mode != Mode::Sim) fields.push_back(Field::Target);
    fields.insert(fields.end(), {Field::Rider, Field::Bike});
    return fields;
}

std::wstring_view App::Label(Field field) const {
    switch (field) {
    case Field::Km: return L"Distance";
    case Field::Gain: return L"Elevation gain";
    case Field::MaxGrade: return L"Max grade";
    case Field::Route: return L"Route";
    case Field::Mode: return L"Mode";
    case Field::Target: return settings_.mode == Mode::Erg ? L"Target power" : L"Resistance";
    case Field::Rider: return L"Rider weight";
    case Field::Bike: return L"Bike weight";
    }
    return {};
}

std::wstring App::Value(Field field) const {
    const CourseParams& c = settings_.course;
    wchar_t buf[32]{};
    switch (field) {
    case Field::Km: swprintf_s(buf, L"%g km", c.km); break;
    case Field::Gain: swprintf_s(buf, L"%g m", c.gainM); break;
    case Field::MaxGrade: swprintf_s(buf, L"%g %%", c.maxGradePct); break;
    case Field::Route: return c.loop ? L"Loop" : L"Point to point";
    case Field::Mode: return settings_.mode == Mode::Sim ? L"Sim" : settings_.mode == Mode::Erg ? L"ERG" : L"Resistance";
    case Field::Target:
        if (settings_.mode == Mode::Erg) swprintf_s(buf, L"%d W", settings_.ergW);
        else swprintf_s(buf, L"%d %%", settings_.resistancePct);
        break;
    case Field::Rider: swprintf_s(buf, L"%g kg", settings_.riderKg); break;
    case Field::Bike: swprintf_s(buf, L"%g kg", settings_.bikeKg); break;
    }
    return buf;
}

// Steps get coarser for long and hilly courses.
void App::Change(Field field, int dir) {
    CourseParams& c = settings_.course;
    switch (field) {
    case Field::Km: c.km = StepValue(c.km, dir, (dir > 0 ? c.km >= 10 : c.km > 10) ? 5 : 1, 1, 500); break;
    case Field::Gain:
        c.gainM = StepValue(c.gainM, dir, (dir > 0 ? c.gainM >= 1000 : c.gainM > 1000) ? 100 : 50, 0, 10000);
        break;
    case Field::MaxGrade: c.maxGradePct = StepValue(c.maxGradePct, dir, 1, 2, 25); break;
    case Field::Route: c.loop = !c.loop; break;
    case Field::Mode: settings_.mode = static_cast<Mode>((static_cast<int>(settings_.mode) + dir + 3) % 3); break;
    case Field::Target:
        if (settings_.mode == Mode::Erg)
            settings_.ergW = static_cast<int>(StepValue(settings_.ergW, dir, 5, 0, kMaxTargetW));
        else
            settings_.resistancePct = static_cast<int>(StepValue(settings_.resistancePct, dir, 5, 0, 100));
        break;
    case Field::Rider: settings_.riderKg = StepValue(settings_.riderKg, dir, 1, 30, 200); break;
    case Field::Bike: settings_.bikeKg = StepValue(settings_.bikeKg, dir, 0.5, 3, 40); break;
    }
    if (field <= Field::Route) MakeCourse();  // same seed, so the course keeps its character
    else Summarize();
    Redraw();
}

void App::Select(int dir) {
    selected_ = std::clamp(selected_ + dir, 0, static_cast<int>(Fields().size()) - 1);
    Redraw();
}

void App::MakeCourse() {
    course_ = GenerateCourse(settings_.course);
    Summarize();
}

// The course in numbers, with a rough moving time at the ERG target or else at 2.5 W/kg.
void App::Summarize() {
    const int watts = settings_.mode == Mode::Erg && settings_.ergW >= 50
                          ? settings_.ergW
                          : static_cast<int>(std::lround(settings_.riderKg * 2.5 / 5)) * 5;
    const int t = static_cast<int>(EstimateTime(course_, settings_.riderKg + settings_.bikeKg, watts));
    wchar_t buf[128];
    const int n = swprintf_s(buf, L"%.1f km · %.0f m up", course_.Length() / 1000, course_.Gain());
    if (t > 0 && n > 0)
        swprintf_s(buf + n, std::size(buf) - n, L" · about %d:%02d at %d W", t / 3600, t / 60 % 60, watts);
    summary_ = buf;
}

void App::Reroll() {
    settings_.course.seed = RandomSeed();
    MakeCourse();
    Redraw();
}

// The rider's mode and targets carry over to the next ride.
void App::KeepRideSettings() {
    settings_.mode = ride_->mode();
    settings_.ergW = ride_->ergW();
    settings_.resistancePct = ride_->resistancePct();
}

void App::Start() {
    SaveSettings(settings_);
    ride_ = std::make_unique<Ride>(course_, settings_.mode, settings_.riderKg + settings_.bikeKg, settings_.ergW,
                                   settings_.resistancePct);
    ride_->OnPower(power_);
    ride_->OnCadence(cadence_);
    ride_->OnHeartRate(Saved(ble::Role::HeartRate).address ? bpm_ : -1);
    for (Hold& h : holds_) h.done = true;  // a button still held from the setup screen doesn't act in the ride
    escAt_ = 0;
    trainer_->SetTarget(ride_->Target());
    Redraw();
}

// A fresh course straight away, for a ride that has only just started.
void App::NewRide() {
    CloseLog();
    KeepRideSettings();
    settings_.course.seed = RandomSeed();
    MakeCourse();
    Start();
}

void App::EndRide() {
    CloseLog();
    KeepRideSettings();
    SaveSettings(settings_);
    ride_.reset();
    selected_ = std::min(selected_, static_cast<int>(Fields().size()) - 1);  // the mode may have changed the rows
    Summarize();
    trainer_->SetTarget({});  // no resistance while setting up
    Redraw();
}

std::wstring App::Status() const {
    if (escAt_ && GetTickCount64() - escAt_ < kConfirmMs) return L"Esc again to end the ride";
    using S = ble::Link::State;
    if (radio_ == ble::Bluetooth::State::Off) return L"Bluetooth is off";
    if (!Saved(ble::Role::Trainer).address) return L"No trainer";
    if (states_[0] != S::Connected) return L"Looking for the trainer";
    if (states_[1] == S::Locked) return L"Shifters locked, unlock them in Zwift";
    if (Saved(ble::Role::HeartRate).address && states_[2] != S::Connected) return L"Looking for the heart rate monitor";
    return {};
}

void App::ShowDevices() {
    devices_ = true;
    focus_ = 0;
    bluetooth_->Scan(true);
    Redraw();
}

void App::ShowCourse() {
    devices_ = false;
    autoContinue_ = false;
    continueAt_ = 0;
    bluetooth_->Scan(false);
    Redraw();
}

ble::Link& App::LinkFor(ble::Role role) {
    switch (role) {
    case ble::Role::Trainer: return *trainer_;
    case ble::Role::Shifters: return *controllers_;
    case ble::Role::HeartRate: break;
    }
    return *monitor_;
}

// Connects a device for a role, or none with address 0, and remembers it.
void App::UseDevice(ble::Role role, uint64_t address, std::wstring name) {
    Saved(role) = {address, name};
    SaveSettings(settings_);
    states_[static_cast<size_t>(role)] = ble::Link::State::Idle;  // until the link reports on the new device
    LinkFor(role).Use(address, std::move(name));
    autoContinue_ = false;
    UpdateNearby();
    Redraw();
}

void App::OnLinkState(ble::Role role, ble::Link::State state) {
    states_[static_cast<size_t>(role)] = state;
    if (state == ble::Link::State::Connected) {
        // A device remembered without a name gets the one it reports.
        SavedDevice& saved = Saved(role);
        if (std::wstring name = LinkFor(role).Name(); saved.address && !name.empty() && name != saved.name) {
            saved.name = std::move(name);
            SaveSettings(settings_);
        }
    } else if (role == ble::Role::Trainer) {
        power_ = 0;
        cadence_ = 0.0;
        if (ride_) ride_->ClearInputs();
    } else if (role == ble::Role::Shifters) {
        OnButtons(0);  // release anything held when they dropped
    } else {
        bpm_ = 0;
        if (ride_ && ride_->heartRate() >= 0) ride_->OnHeartRate(0);
    }
    Redraw();
}

// The nearby devices not already in use, grouped by role.
void App::UpdateNearby() {
    nearby_ = bluetooth_->Nearby();
    std::erase_if(nearby_, [&](const ble::Bluetooth::Device& d) {
        return std::any_of(std::begin(settings_.devices), std::end(settings_.devices),
                           [&](const SavedDevice& s) { return s.address == d.address; });
    });
    std::stable_sort(nearby_.begin(), nearby_.end(), [](const auto& a, const auto& b) { return a.role < b.role; });
    focus_ = std::min(focus_, Items() - 1);
}

// The cards and the nearby devices that fit on screen.
int App::Items() const { return kCards + static_cast<int>(LayoutDevices(Size(), nearby_.size()).nearbyRows); }

void App::Focus(int dir) {
    focus_ = std::clamp(focus_ + dir, 0, Items() - 1);
    Redraw();
}

// Disconnects the focused card's device, or connects the focused nearby one.
void App::Act() {
    if (focus_ < kCards) {
        const auto role = static_cast<ble::Role>(focus_);
        if (Saved(role).address) UseDevice(role, 0, {});
        return;
    }
    const size_t i = static_cast<size_t>(focus_ - kCards);
    if (i >= nearby_.size()) return;
    const ble::Bluetooth::Device d = nearby_[i];
    focus_ = static_cast<int>(d.role);  // follow it to its card
    UseDevice(d.role, d.address, d.name);
}

// Looks afresh for devices nearby, and retries the remembered ones that aren't connected.
void App::Refresh() {
    refreshedAt_ = GetTickCount64();
    bluetooth_->Scan(true);
    for (size_t i = 0; i < ble::kRoleCount; ++i) {
        const auto role = static_cast<ble::Role>(i);
        if (Saved(role).address && states_[i] != ble::Link::State::Connected)
            LinkFor(role).Use(Saved(role).address, Saved(role).name);
    }
    Redraw();
}

bool App::AllConnected() const {
    bool any = false;
    for (size_t i = 0; i < ble::kRoleCount; ++i) {
        if (!settings_.devices[i].address) continue;
        if (states_[i] != ble::Link::State::Connected) return false;
        any = true;
    }
    return any;
}

DeviceInfo App::Device(ble::Role role) const {
    using S = ble::Link::State;
    const size_t i = static_cast<size_t>(role);
    const SavedDevice& saved = settings_.devices[i];
    DeviceInfo info{RoleName(role), saved.name, {}, Tone::Waiting};
    if (saved.address && info.name.empty()) info.name = L"Saved device";
    switch (states_[i]) {
    case S::Idle: info.status = saved.address ? L"Connecting" : L"Not set"; break;
    case S::Connecting: info.status = L"Connecting"; break;
    case S::NoBluetooth: info.status = L"Bluetooth is off", info.tone = Tone::Problem; break;
    case S::Missing: info.status = L"Not found, is it awake?", info.tone = Tone::Problem; break;
    case S::Locked: info.status = L"Locked, unlock it in Zwift", info.tone = Tone::Problem; break;
    case S::Connected: {
        info.tone = Tone::Good;
        wchar_t buf[64] = L"Connected";
        if (role == ble::Role::Trainer) swprintf_s(buf, L"Connected · %d W · %.0f rpm", power_, cadence_);
        else if (role == ble::Role::HeartRate && bpm_ > 0) swprintf_s(buf, L"Connected · %d bpm", bpm_);
        info.status = buf;
        break;
    }
    }
    return info;
}

void App::OnDevicesKey(WPARAM key) {
    switch (key) {
    case VK_UP:
    case VK_LEFT: Focus(-1); break;
    case VK_DOWN:
    case VK_RIGHT: Focus(+1); break;
    case VK_RETURN:
    case VK_SPACE:
        // With Bluetooth off there is nothing to connect, so Enter turns it on.
        if (radio_ == ble::Bluetooth::State::Off || radio_ == ble::Bluetooth::State::Missing) bluetooth_->TurnOn();
        else Act();
        break;
    case 'R': Refresh(); break;
    case VK_ESCAPE:
    case 'D': ShowCourse(); break;
    }
}

void App::OnDevicesClick(D2D1_POINT_2F p) {
    const DevicesLayout l = LayoutDevices(Size(), nearby_.size());
    auto inside = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    if (inside(l.next)) return ShowCourse();
    if (inside(l.refresh)) return Refresh();
    if ((radio_ == ble::Bluetooth::State::Off || radio_ == ble::Bluetooth::State::Missing) && inside(l.action))
        return bluetooth_->TurnOn();
    // A card takes the focus, and its button disconnects. A nearby row connects anywhere on it.
    for (int i = 0; i < kCards; ++i) {
        if (!inside(l.cards[i])) continue;
        focus_ = i;
        if (inside(l.disconnect[i])) Act();
        Redraw();
        return;
    }
    for (size_t i = 0; i < l.nearbyRows; ++i) {
        if (!inside(l.nearby[i])) continue;
        focus_ = kCards + static_cast<int>(i);
        Act();
        return;
    }
}

void App::OnKey(WPARAM key) {
    if (key == VK_F11) return ToggleFullscreen();
    if (!ride_ && devices_) {
        autoContinue_ = false;
        return OnDevicesKey(key);
    }
    if (!ride_) {
        switch (key) {
        case 'D': ShowDevices(); break;
        case VK_UP: Select(-1); break;
        case VK_DOWN: Select(+1); break;
        case VK_LEFT:
        case VK_RIGHT: Change(Fields()[selected_], key == VK_RIGHT ? 1 : -1); break;
        case 'R': Reroll(); break;
        case VK_RETURN: Start(); break;
        case VK_ESCAPE:
            if (fullscreen_) ToggleFullscreen();
            break;
        }
        return;
    }

    // Up and right are harder, down and left easier. Shift and Page Up/Down take big steps.
    Ride& r = *ride_;
    const bool big = GetKeyState(VK_SHIFT) < 0;
    switch (key) {
    case VK_UP:
    case VK_RIGHT: r.Adjust(+1, big); break;
    case VK_DOWN:
    case VK_LEFT: r.Adjust(-1, big); break;
    case VK_PRIOR: r.Adjust(+1, true); break;
    case VK_NEXT: r.Adjust(-1, true); break;
    case VK_SPACE:
    case 'P': r.TogglePause(); break;
    case 'M': r.NextMode(); break;
    case 'R':
        if (r.distance() < kRerollBeforeM || r.finished()) NewRide();
        return;
    case VK_RETURN:
        if (r.finished()) EndRide();
        return;
    case VK_ESCAPE: {
        // Ending a ride that is under way takes a second press, so a stray key doesn't lose it.
        const ULONGLONG now = GetTickCount64();
        if (r.finished() || r.distance() < kRerollBeforeM || (escAt_ && now - escAt_ < kConfirmMs)) {
            EndRide();
            return;
        }
        escAt_ = now;
        break;
    }
    default: return;
    }
    trainer_->SetTarget(r.Target());
    Redraw();
}

void App::OnClick(D2D1_POINT_2F p) {
    if (ride_) return;
    if (devices_) {
        autoContinue_ = false;
        return OnDevicesClick(p);
    }
    const std::vector<Field> fields = Fields();
    const SetupLayout l = LayoutSetup(Size(), fields.size());
    auto inside = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    if (inside(l.start)) return Start();
    if (inside(l.reroll)) return Reroll();
    if (inside(l.devices)) return ShowDevices();
    for (size_t i = 0; i < fields.size() && i < SetupLayout::kMaxRows; ++i) {
        if (!inside(l.rows[i])) continue;
        selected_ = static_cast<int>(i);
        // The value's left half steps down and its right half up, where the arrows are.
        const float valueLeft = l.valueLeft(i);
        if (p.x >= valueLeft) Change(fields[i], p.x < (valueLeft + l.rows[i].right) / 2 ? -1 : 1);
        Redraw();
        return;
    }
}

void App::OnWheel(D2D1_POINT_2F p, int dir) {
    if (ride_) {
        ride_->Adjust(dir, false);
        trainer_->SetTarget(ride_->Target());
        Redraw();
        return;
    }
    if (devices_) return;
    const std::vector<Field> fields = Fields();
    const SetupLayout l = LayoutSetup(Size(), fields.size());
    for (size_t i = 0; i < fields.size() && i < SetupLayout::kMaxRows; ++i) {
        const D2D1_RECT_F& r = l.rows[i];
        if (p.x < r.left || p.x >= r.right || p.y < r.top || p.y >= r.bottom) continue;
        selected_ = static_cast<int>(i);
        Change(fields[i], dir);
        return;
    }
}

// On the devices screen the D-pad and shifters move, A connects or disconnects, Y refreshes and B moves on. On
// the setup screen the D-pad moves through the settings and the shifters change them; A starts and B goes back
// to the devices. In a ride, right shifters go harder and left ones easier, like Zwift, and the D-pad moves in
// big steps.
void App::OnButtons(uint32_t pressed) {
    namespace b = ble::button;
    const uint32_t down = pressed & ~buttons_;
    buttons_ = pressed;
    const ULONGLONG now = GetTickCount64();
    for (Hold& h : holds_)
        if (down & h.button) h.since = now, h.done = false;

    if (!ride_ && devices_) {
        if (!down) return;
        autoContinue_ = false;
        if (down & (b::kUp | b::kLeft | b::kShiftUpLeft | b::kShiftDownLeft)) Focus(-1);
        if (down & (b::kDown | b::kRight | b::kShiftUpRight | b::kShiftDownRight)) Focus(+1);
        if (down & b::kA) Act();
        if (down & b::kY) Refresh();
        if (down & b::kB) ShowCourse();
        return;
    }
    if (!ride_) {
        if (down & b::kUp) Select(-1);
        if (down & b::kDown) Select(+1);
        if (down & (b::kRight | b::kShiftUpRight | b::kShiftDownRight)) Change(Fields()[selected_], +1);
        if (down & (b::kLeft | b::kShiftUpLeft | b::kShiftDownLeft)) Change(Fields()[selected_], -1);
        if (down & b::kY) Reroll();
        if (down & b::kA) Start();
        if (down & b::kB) ShowDevices();
        return;
    }
    if (ride_->finished()) {
        if (down & b::kA) EndRide();
        return;
    }
    if (down & (b::kShiftUpRight | b::kShiftDownRight)) ride_->Adjust(+1, false);
    if (down & (b::kShiftUpLeft | b::kShiftDownLeft)) ride_->Adjust(-1, false);
    if (down & b::kUp) ride_->Adjust(+1, true);
    if (down & b::kDown) ride_->Adjust(-1, true);
    if (down & b::kY) ride_->NextMode();
    if (down & b::kZ) ride_->TogglePause();
    trainer_->SetTarget(ride_->Target());
    Redraw();
}

// True once when the button has been held long enough.
bool App::Held(uint32_t button, ULONGLONG now) {
    for (Hold& h : holds_)
        if (h.button == button && !h.done && (buttons_ & button) && now - h.since >= kHoldMs) return h.done = true;
    return false;
}

void App::OnTick() {
    const ULONGLONG now = GetTickCount64();
    const double dt = std::min((now - lastTick_) / 1000.0, 1.0);
    lastTick_ = now;

    if (ride_ && Held(ble::button::kA, now) && ride_->distance() < kRerollBeforeM) NewRide();
    if (ride_ && Held(ble::button::kB, now)) EndRide();
    if (ride_) {
        ride_->Tick(dt);
        trainer_->SetTarget(ride_->Target());
        if (ride_->distance() > 0 && !ride_->paused() && !ride_->finished()) {
            logClock_ += dt;
            if (logClock_ >= 1.0) {
                logClock_ -= 1.0;
                WriteLog();
            }
        }
        if (ride_->finished()) CloseLog();
    }
    // Once the remembered devices have all connected, the screen says so for a moment and moves on.
    if (!ride_ && devices_ && autoContinue_) {
        if (!AllConnected()) continueAt_ = 0;
        else if (!continueAt_) continueAt_ = now + kContinueMs;
        else if (now >= continueAt_) ShowCourse();
    }
    if (!IsIconic(hwnd_)) Redraw();
}

void App::Paint() {
    if (IsIconic(hwnd_)) return;
    try {
        if (!target_) {
            // Hardware when the driver allows it, otherwise Direct2D falls back to software on its own.
            RECT rc;
            GetClientRect(hwnd_, &rc);
            const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
            const auto props =
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi);
            winrt::check_hresult(d2d_->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), target_.put()));
        }
        target_->BeginDraw();
        DeviceInfo devices[ble::kRoleCount];
        for (size_t i = 0; i < ble::kRoleCount; ++i) devices[i] = Device(static_cast<ble::Role>(i));
        if (ride_) {
            renderer_->Draw(target_.get(), *ride_, Status());
        } else if (devices_) {
            std::vector<NearbyRow> nearby;
            for (const auto& d : nearby_)
                nearby.push_back({RoleName(d.role), d.name.empty() ? std::wstring_view(L"Unnamed") : d.name});
            DevicesView view;
            view.cards = devices;
            view.nearby = nearby;
            view.focus = focus_;
            switch (radio_) {
            case ble::Bluetooth::State::Unknown: view.bluetooth = L"Checking Bluetooth"; break;
            case ble::Bluetooth::State::Missing:
                view.bluetooth = L"No Bluetooth adapter found";
                view.action = L"Open settings";
                break;
            case ble::Bluetooth::State::Off:
                view.bluetooth = L"Bluetooth is off";
                view.action = L"Turn on";
                break;
            case ble::Bluetooth::State::On: view.bluetooth = L"Bluetooth is on and looking for devices"; break;
            }
            wchar_t footer[96] = L"Your devices are remembered and connect by themselves next time.";
            if (autoContinue_ && continueAt_) {
                const ULONGLONG left = continueAt_ - std::min(continueAt_, GetTickCount64());
                swprintf_s(footer, L"All connected. Continuing in %llu s", (left + 999) / 1000);
            } else if (autoContinue_) {
                wcscpy_s(footer, L"Connecting to your devices");
            }
            view.footer = footer;
            // For a few seconds after Refresh the button counts dots, so the press visibly did something.
            const ULONGLONG sinceRefresh = GetTickCount64() - refreshedAt_;
            static constexpr std::wstring_view kSearching[] = {L"Searching.", L"Searching..", L"Searching..."};
            if (refreshedAt_ && sinceRefresh < kSearchMs) {
                view.searching = true;
                view.refresh = kSearching[sinceRefresh / 400 % 3];
            }
            renderer_->DrawDevices(target_.get(), view);
        } else {
            std::vector<SetupRow> rows;
            for (Field f : Fields()) rows.push_back({Label(f), Value(f)});
            SetupView view;
            view.rows = rows;
            view.selected = selected_;
            view.course = &course_;
            view.summary = summary_;
            view.devices = devices;
            renderer_->DrawSetup(target_.get(), view);
        }
        if (target_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) target_ = nullptr;
    } catch (...) {
        target_ = nullptr;  // a lost device or a failed resource: start over on the next frame
    }
}

// Borderless over the whole monitor, and back to where the window was.
void App::ToggleFullscreen() {
    const LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowPlacement(hwnd_, &placement_) ||
            !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = !fullscreen_;
}

D2D1_SIZE_F App::Size() const {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const float scale = 96.0f / GetDpiForWindow(hwnd_);
    return {rc.right * scale, rc.bottom * scale};
}

D2D1_POINT_2F App::ToDips(POINT p) const {
    const float scale = 96.0f / GetDpiForWindow(hwnd_);
    return {p.x * scale, p.y * scale};
}

// One row per second of riding, to look at afterwards.
void App::WriteLog() {
    if (!log_) {
        const std::wstring dir = RidesDir();
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf_s(name, L"\\%04d-%02d-%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                   st.wSecond);
        if (_wfopen_s(&log_, (dir + name).c_str(), L"w") || !log_) {
            log_ = nullptr;
            return;
        }
        const CourseParams& c = ride_->course().params;
        std::fprintf(log_, "# km=%.1f gain=%.0f max_grade=%.0f loop=%d seed=%u\n", c.km, ride_->course().targetGain,
                     c.maxGradePct, c.loop ? 1 : 0, c.seed);
        std::fprintf(log_, "time_s,distance_m,elevation_m,grade_pct,power_w,cadence_rpm,speed_kmh,mode,gear,target,"
                           "heart_rate_bpm\n");
    }
    static const char* const kModes[] = {"sim", "erg", "resistance"};
    const Ride& r = *ride_;
    std::fprintf(log_, "%.0f,%.1f,%.1f,%.1f,%d,%.0f,%.1f,%s,%d,%d,%d\n", r.elapsed(), r.distance(), r.elevation(),
                 r.grade() * 100, r.power(), r.cadence(), r.speed() * 3.6, kModes[static_cast<int>(r.mode())], r.gear(),
                 r.Target().value, std::max(r.heartRate(), 0));
    std::fflush(log_);
}

void App::CloseLog() {
    if (log_) std::fclose(log_);
    log_ = nullptr;
    logClock_ = 0.0;
}

void App::Shutdown() {
    if (!trainer_) return;
    KillTimer(hwnd_, kTimer);
    CloseLog();
    // Release the trainer before the window goes, so it isn't left holding a power target.
    trainer_.reset();
    controllers_.reset();
    monitor_.reset();
    bluetooth_.reset();
    if (ride_) KeepRideSettings();
    SaveSettings(settings_);
}

}  // namespace

int RunApp(HINSTANCE instance, Settings settings) {
    App app(std::move(settings));
    return app.Run(instance);
}
