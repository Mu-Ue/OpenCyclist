#include "app.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
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
#include "sim/fit.h"
#include "ui/renderer.h"

namespace {

constexpr UINT_PTR kTimer = 1;
constexpr UINT_PTR kRepeatTimer = 2;
constexpr UINT kRepeatDelayMs = 400, kRepeatMs = 60;  // a held setting starts stepping after the delay
constexpr ULONGLONG kContinueMs = 3000;  // the devices screen shows all connected this long before moving on
constexpr int kCards = static_cast<int>(ble::kRoleCount);  // the devices screen's first items, one per role

std::wstring_view RoleName(ble::Role role) {
    switch (role) {
    case ble::Role::Trainer: return L"Trainer";
    case ble::Role::Shifters: return L"Shifters";
    case ble::Role::HeartRate: return L"Heart rate";
    }
    return {};
}

// The settings on the setup screen, top to bottom. The route picks the course settings, and Target shows only in
// ERG mode. The course settings come before Mode.
enum class Field { Route, Km, Terrain, Gain, MaxGrade, Mode, Target, Rider };

constexpr const wchar_t* kTerrainNames[kTerrainCount] = {L"Flat", L"Rolling", L"Hilly", L"Mountains"};

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
    void AskToEnd();
    void EndRide();
    std::wstring Status() const;

    // The summary screen after a ride.
    void CloseSummary();
    void SaveFit();
    void ShowFile() const;
    void OnSummaryClick(D2D1_POINT_2F p);

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
    void StartRepeat(Field field, int dir);
    void StopRepeat();

    void UpdateTimer();
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
    Field repeatField_{};  // a setting held down with the mouse or the shifters, which keeps stepping
    int repeatDir_ = 0;    // 0 when nothing is held

    Course course_;  // on the setup screen, and the one being ridden
    std::wstring stats_[1];  // the course's rough time
    int selected_ = 0;
    std::unique_ptr<Ride> ride_;  // null on the setup and devices screens
    bool confirmEnd_ = false;  // asking whether to end the ride
    std::unique_ptr<Ride> done_;  // the ride just ended, which the summary screen shows when not null
    std::wstring doneDate_, donePath_;  // donePath_ is the ride as a FIT file, empty when it was too short

    bool devices_ = false;  // the devices screen shows instead of the setup screen
    std::vector<ble::Bluetooth::Device> nearby_;  // found nearby and not in use
    int focus_ = 0;  // on the devices screen: the cards, then the nearby devices
    bool autoContinue_ = false;  // move on by itself once the remembered devices connect
    ULONGLONG continueAt_ = 0;

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
    // Buttons that act once when held: A rerolls a ride that has just started.
    struct Hold {
        uint32_t button;
        ULONGLONG since = 0;
        bool done = true;
    } holds_[1] = {{ble::button::kA}};
    ULONGLONG lastTick_ = 0;
    FILE* log_ = nullptr;
    std::wstring logPath_;  // the ride's log, once it has a row
    double logClock_ = 0.0;
    bool timer_ = false;   // ticking, while something on screen moves by itself
    bool closed_ = false;
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
    // Start on the devices screen. Remembered devices connect by themselves, and once they all have, the
    // course screen follows. The window shows first, so Bluetooth starting up doesn't hold up the first frame.
    devices_ = true;
    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);
    const SavedDevice* saved = settings_.devices;
    trainer_ = std::make_unique<ble::Trainer>(hwnd_, saved[0].address, saved[0].name);
    controllers_ = std::make_unique<ble::Controllers>(hwnd_, saved[1].address, saved[1].name);
    monitor_ = std::make_unique<ble::HeartRateMonitor>(hwnd_, saved[2].address, saved[2].name);
    bluetooth_ = std::make_unique<ble::Bluetooth>(hwnd_);
    ShowDevices();
    autoContinue_ = std::any_of(saved, saved + ble::kRoleCount, [](const SavedDevice& d) { return d.address != 0; });
    lastTick_ = GetTickCount64();
    UpdateTimer();

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
    if (closed_ && msg >= WM_APP && msg <= ble::WM_NEARBY_CHANGED) return 0;
    switch (msg) {
    case WM_TIMER:
        if (wParam == kRepeatTimer) {
            if (!repeatDir_ || ride_ || devices_ || done_) {
                StopRepeat();
            } else {
                Change(repeatField_, repeatDir_);
                SetTimer(hwnd_, kRepeatTimer, kRepeatMs, nullptr);
            }
            return 0;
        }
        OnTick();
        UpdateTimer();
        return 0;
    case ble::WM_TRAINER_DATA: {
        const int power = power_;
        const double cadence = cadence_;
        if (wParam & ble::kHasPower) power_ = LOWORD(lParam);
        if (wParam & ble::kHasCadence) cadence_ = HIWORD(lParam) / 2.0;
        if (ride_) ride_->OnPower(power_), ride_->OnCadence(cadence_);
        // A ride redraws on its tick. Elsewhere the numbers show on the devices, when they change.
        else if (!done_ && (power != power_ || cadence != cadence_)) Redraw();
        return 0;
    }
    case ble::WM_HEART_RATE:
        if (!ride_ && !done_ && bpm_ != static_cast<int>(wParam)) Redraw();
        bpm_ = static_cast<int>(wParam);
        if (ride_ && ride_->heartRate() >= 0) ride_->OnHeartRate(bpm_);
        return 0;
    case ble::WM_LINK_STATE:
        if (wParam < ble::kRoleCount) OnLinkState(static_cast<ble::Role>(wParam), static_cast<ble::Link::State>(lParam));
        UpdateTimer();
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
        UpdateTimer();
        return 0;
    case WM_KEYDOWN:
        OnKey(wParam);
        UpdateTimer();
        return 0;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN) {  // Alt+Enter
            ToggleFullscreen();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        OnClick(ToDips({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}));
        UpdateTimer();
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd_) ReleaseCapture();
        StopRepeat();
        return 0;
    case WM_CAPTURECHANGED:
        StopRepeat();
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
    const CourseParams& c = settings_.course;
    std::vector<Field> fields{Field::Route};
    if (!c.endless) fields.insert(fields.end(), {Field::Km, Field::Gain, Field::MaxGrade});
    else if (c.terrain > 0) fields.insert(fields.end(), {Field::Terrain, Field::MaxGrade});
    else fields.push_back(Field::Terrain);
    fields.push_back(Field::Mode);
    if (settings_.mode == Mode::Erg) fields.push_back(Field::Target);
    fields.push_back(Field::Rider);
    return fields;
}

std::wstring_view App::Label(Field field) const {
    switch (field) {
    case Field::Km: return L"Distance";
    case Field::Terrain: return L"Terrain";
    case Field::Gain: return L"Elevation gain";
    case Field::MaxGrade: return L"Max grade";
    case Field::Route: return L"Route";
    case Field::Mode: return L"Mode";
    case Field::Target: return L"Target power";
    case Field::Rider: return L"Rider weight";
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
    case Field::Terrain: return kTerrainNames[std::clamp(c.terrain, 0, kTerrainCount - 1)];
    case Field::Route: return c.endless ? L"Endless" : c.loop ? L"Loop" : L"Point to point";
    case Field::Mode: return settings_.mode == Mode::Erg ? L"ERG" : L"Sim";
    case Field::Target: swprintf_s(buf, L"%d W", settings_.ergW); break;
    case Field::Rider: swprintf_s(buf, L"%g kg", settings_.riderKg); break;
    }
    return buf;
}

// Steps get coarser for long and hilly courses.
void App::Change(Field field, int dir) {
    CourseParams& c = settings_.course;
    switch (field) {
    case Field::Terrain: c.terrain = std::clamp(c.terrain + dir, 0, kTerrainCount - 1); break;
    case Field::Km: c.km = StepValue(c.km, dir, (dir > 0 ? c.km >= 10 : c.km > 10) ? 5 : 1, 1, 500); break;
    case Field::Gain:
        c.gainM = StepValue(c.gainM, dir, (dir > 0 ? c.gainM >= 1000 : c.gainM > 1000) ? 100 : 50, 0, 10000);
        break;
    case Field::MaxGrade: c.maxGradePct = StepValue(c.maxGradePct, dir, 1, 2, 25); break;
    case Field::Route: {
        const int route = ((c.endless ? 2 : c.loop ? 1 : 0) + dir + 3) % 3;
        c.loop = route == 1;
        c.endless = route == 2;
        break;
    }
    case Field::Mode: settings_.mode = settings_.mode == Mode::Sim ? Mode::Erg : Mode::Sim; break;
    case Field::Target: settings_.ergW = static_cast<int>(StepValue(settings_.ergW, dir, 5, 0, kMaxErgW)); break;
    case Field::Rider: settings_.riderKg = StepValue(settings_.riderKg, dir, 1, 30, 200); break;
    }
    selected_ = std::min(selected_, static_cast<int>(Fields().size()) - 1);  // flat terrain hides a row
    if (field <= Field::MaxGrade) MakeCourse();  // same seed, so the course keeps its character
    else Summarize();
    Redraw();
}

void App::Select(int dir) {
    selected_ = std::clamp(selected_ + dir, 0, static_cast<int>(Fields().size()) - 1);
    Redraw();
}

// The gain stops at what the course can climb, so the setting always shows what it gets.
void App::MakeCourse() {
    CourseParams& c = settings_.course;
    if (!c.endless) c.gainM = std::min(c.gainM, std::floor(MaxGain(c) / 10) * 10);
    course_ = GenerateCourse(c);
    Summarize();
}

// A rough moving time for the course at the ERG target or else at 2.5 W/kg. An endless course has none.
void App::Summarize() {
    if (settings_.course.endless) {
        stats_[0].clear();
        return;
    }
    const int watts = settings_.mode == Mode::Erg && settings_.ergW >= 50
                          ? settings_.ergW
                          : static_cast<int>(std::lround(settings_.riderKg * 2.5 / 5)) * 5;
    const int t = static_cast<int>(EstimateTime(course_, settings_.riderKg + kBikeKg, watts));
    wchar_t buf[64];
    if (t > 0) swprintf_s(buf, L"About %d:%02d at %d W", t / 3600, t / 60 % 60, watts);
    else swprintf_s(buf, L"Too steep for %d W", watts);
    stats_[0] = buf;
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
}

void App::Start() {
    SaveSettings(settings_);
    ride_ = std::make_unique<Ride>(course_, settings_.mode, settings_.riderKg + kBikeKg, settings_.ergW);
    ride_->OnPower(power_);
    ride_->OnCadence(cadence_);
    ride_->OnHeartRate(Saved(ble::Role::HeartRate).address ? bpm_ : -1);
    for (Hold& h : holds_) h.done = true;  // a button still held from the setup screen doesn't act in the ride
    confirmEnd_ = false;
    trainer_->SetTarget(ride_->Target());
    Redraw();
}

// A fresh course straight away, for a ride that has only just started.
void App::NewRide() {
    CloseLog();
    logPath_.clear();
    KeepRideSettings();
    settings_.course.seed = RandomSeed();
    MakeCourse();
    Start();
}

// Asks before ending, so a stray key or click doesn't lose the ride.
void App::AskToEnd() {
    confirmEnd_ = true;
    Redraw();
}

// The ride moves on to its summary.
void App::EndRide() {
    CloseLog();
    SaveFit();
    KeepRideSettings();
    SaveSettings(settings_);
    confirmEnd_ = false;
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t date[64] = L"", time[32] = L"";
        GetDateFormatEx(L"en-US", 0, &st, L"dddd d MMMM", date, static_cast<int>(std::size(date)), nullptr);
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, time, static_cast<int>(std::size(time)));
        doneDate_ = std::wstring(date) + L" · " + time;
        logPath_.clear();
        done_ = std::move(ride_);
    }
    ride_.reset();
    selected_ = std::min(selected_, static_cast<int>(Fields().size()) - 1);  // the mode may have changed the rows
    Summarize();
    trainer_->SetTarget({});  // no resistance while setting up
    Redraw();
}

void App::CloseSummary() {
    done_.reset();
    Redraw();
}

// The ride log as a FIT file next to it, which COROS, Garmin Connect, Strava and others import.
void App::SaveFit() {
    donePath_.clear();
    if (logPath_.empty()) return;
    std::ifstream in(logPath_, std::ios::binary);
    const std::string fit = RideToFit(std::string(std::istreambuf_iterator<char>(in), {}));
    if (fit.empty()) return;
    std::wstring path = logPath_.substr(0, logPath_.rfind(L'.')) + L".fit";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out.write(fit.data(), static_cast<std::streamsize>(fit.size()))) donePath_ = std::move(path);
}

// Shows the ride's FIT file in File Explorer, ready to drag into an upload page.
void App::ShowFile() const {
    if (donePath_.empty()) return;
    const std::wstring args = L"/select,\"" + donePath_ + L"\"";
    ShellExecuteW(hwnd_, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void App::OnSummaryClick(D2D1_POINT_2F p) {
    const SummaryLayout l = LayoutSummary(Size());
    auto inside = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    if (inside(l.done)) CloseSummary();
    else if (inside(l.file)) ShowFile();
}

std::wstring App::Status() const {
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
        // A device remembered without a name gets the one it reports, and one that moved is found at its new
        // address next time.
        SavedDevice& saved = Saved(role);
        const ble::Link& link = LinkFor(role);
        std::wstring name = link.Name();
        const uint64_t address = link.Address();
        if (saved.address && ((!name.empty() && name != saved.name) || (address && address != saved.address))) {
            if (!name.empty()) saved.name = std::move(name);
            if (address) saved.address = address;
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
    case S::Missing: info.status = L"Not found yet, wake it up", info.tone = Tone::Problem; break;
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
    case VK_ESCAPE:
    case 'D': ShowCourse(); break;
    }
}

void App::OnDevicesClick(D2D1_POINT_2F p) {
    const DevicesLayout l = LayoutDevices(Size(), nearby_.size());
    auto inside = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    if (inside(l.next)) return ShowCourse();
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
    if (done_) {
        if (key == VK_RETURN || key == VK_ESCAPE || key == VK_SPACE) CloseSummary();
        else if (key == 'F') ShowFile();
        return;
    }
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

    // While asking whether to end the ride, Enter or Y ends it and Esc or N goes back to riding.
    if (confirmEnd_) {
        if (key == VK_RETURN || key == 'Y') EndRide();
        else if (key == VK_ESCAPE || key == 'N') confirmEnd_ = false, Redraw();
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
    case VK_ESCAPE: AskToEnd(); return;
    default: return;
    }
    trainer_->SetTarget(r.Target());
    Redraw();
}

void App::OnClick(D2D1_POINT_2F p) {
    auto inside = [&](const D2D1_RECT_F& r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; };
    if (ride_) {
        if (!confirmEnd_) {
            const RideLayout l = LayoutRide(Size(), ride_->mode() == Mode::Erg);
            if (inside(l.end)) return AskToEnd();
            // The arrows over and under the ERG target's digits step it by 100, 10 or 1 W.
            if (ride_->mode() != Mode::Erg) return;
            for (int i = 0, step = 100; i < 3; ++i, step /= 10) {
                const int dir = inside(l.up[i]) ? 1 : inside(l.down[i]) ? -1 : 0;
                if (!dir) continue;
                ride_->SetErgW(ride_->ergW() + dir * step);
                trainer_->SetTarget(ride_->Target());
                Redraw();
            }
            return;
        }
        const ConfirmLayout c = LayoutConfirm(Size());
        if (inside(c.yes)) EndRide();
        else if (inside(c.no)) confirmEnd_ = false, Redraw();
        return;
    }
    if (done_) return OnSummaryClick(p);
    if (devices_) {
        autoContinue_ = false;
        return OnDevicesClick(p);
    }
    const std::vector<Field> fields = Fields();
    const SetupLayout l = LayoutSetup(Size(), fields.size());
    if (inside(l.start)) return Start();
    if (inside(l.reroll)) return Reroll();
    if (inside(l.devices)) return ShowDevices();
    for (size_t i = 0; i < fields.size() && i < SetupLayout::kMaxRows; ++i) {
        if (!inside(l.rows[i])) continue;
        selected_ = static_cast<int>(i);
        // The value's left half steps down and its right half up, where the arrows are.
        const float valueLeft = l.valueLeft(i);
        if (p.x >= valueLeft) {
            const int dir = p.x < (valueLeft + l.rows[i].right) / 2 ? -1 : 1;
            Change(fields[i], dir);
            StartRepeat(fields[i], dir);
            SetCapture(hwnd_);  // to hear the release outside the window too
        }
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
    if (devices_ || done_) return;
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

// On the devices screen the D-pad and shifters move, A connects or disconnects and B moves on. On
// the setup screen the D-pad moves through the settings and the shifters change them; A starts and B goes back
// to the devices. In a ride, right shifters go harder and left ones easier, like Zwift, and the D-pad moves in
// big steps, and B asks to end the ride.
void App::OnButtons(uint32_t pressed) {
    namespace b = ble::button;
    const uint32_t down = pressed & ~buttons_;
    buttons_ = pressed;
    const ULONGLONG now = GetTickCount64();
    for (Hold& h : holds_)
        if (down & h.button) h.since = now, h.done = false;

    // On the summary, A is done and Y shows the file.
    if (!ride_ && done_) {
        if (down & b::kY) ShowFile();
        else if (down & (b::kA | b::kB)) CloseSummary();
        return;
    }
    if (!ride_ && devices_) {
        if (!down) return;
        autoContinue_ = false;
        if (down & (b::kUp | b::kLeft | b::kShiftUpLeft | b::kShiftDownLeft)) Focus(-1);
        if (down & (b::kDown | b::kRight | b::kShiftUpRight | b::kShiftDownRight)) Focus(+1);
        if (down & b::kA) Act();
        if (down & b::kB) ShowCourse();
        return;
    }
    if (!ride_) {
        constexpr uint32_t kHarder = b::kRight | b::kShiftUpRight | b::kShiftDownRight;
        constexpr uint32_t kEasier = b::kLeft | b::kShiftUpLeft | b::kShiftDownLeft;
        if (down & b::kUp) Select(-1);
        if (down & b::kDown) Select(+1);
        for (const int dir : {+1, -1}) {
            if (!(down & (dir > 0 ? kHarder : kEasier))) continue;
            const Field field = Fields()[selected_];
            Change(field, dir);
            StartRepeat(field, dir);
        }
        if (repeatDir_ && !(pressed & (repeatDir_ > 0 ? kHarder : kEasier))) StopRepeat();
        if (down & b::kY) Reroll();
        if (down & b::kA) Start();
        if (down & b::kB) ShowDevices();
        return;
    }
    if (ride_->finished()) {
        if (down & b::kA) EndRide();
        return;
    }
    if (confirmEnd_) {
        if (down & b::kA) EndRide();
        else if (down & b::kB) confirmEnd_ = false, Redraw();
        return;
    }
    if (down & b::kB) return AskToEnd();
    if (down & (b::kShiftUpRight | b::kShiftDownRight)) ride_->Adjust(+1, false);
    if (down & (b::kShiftUpLeft | b::kShiftDownLeft)) ride_->Adjust(-1, false);
    if (down & b::kUp) ride_->Adjust(+1, true);
    if (down & b::kDown) ride_->Adjust(-1, true);
    if (down & b::kY) ride_->NextMode();
    if (down & b::kZ) ride_->TogglePause();
    trainer_->SetTarget(ride_->Target());
    Redraw();
}

// A held mouse button or shifter keeps stepping a number setting, after a moment. Held keys repeat by themselves.
void App::StartRepeat(Field field, int dir) {
    if (field == Field::Route || field == Field::Mode) return;  // choices that cycle would flicker
    repeatField_ = field;
    repeatDir_ = dir;
    SetTimer(hwnd_, kRepeatTimer, kRepeatDelayMs, nullptr);
}

void App::StopRepeat() {
    if (repeatDir_) KillTimer(hwnd_, kRepeatTimer);
    repeatDir_ = 0;
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

    if (ride_ && !confirmEnd_ && Held(ble::button::kA, now) && ride_->distance() < kRerollBeforeM) NewRide();
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
        if (ride_->finished()) EndRide();
    }
    // Once the remembered devices have all connected, the screen says so for a moment and moves on.
    if (!ride_ && devices_ && autoContinue_) {
        if (!AllConnected()) continueAt_ = 0;
        else if (!continueAt_) continueAt_ = now + kContinueMs;
        else if (now >= continueAt_) ShowCourse();
    }
    if (!IsIconic(hwnd_)) Redraw();
}

// The timer runs while something moves by itself: a ride, or the countdown to the course. Otherwise the window
// sleeps until something happens.
void App::UpdateTimer() {
    const bool counting = devices_ && !ride_ && !done_ && autoContinue_ && AllConnected();
    if (!counting) continueAt_ = 0;
    const bool need = !closed_ && (ride_ || counting);
    if (need == timer_) return;
    timer_ = need;
    if (need) {
        lastTick_ = GetTickCount64();
        SetTimer(hwnd_, kTimer, kTickMs, nullptr);
    } else {
        KillTimer(hwnd_, kTimer);
        Redraw();  // the last frame
    }
}

void App::Paint() {
    if (IsIconic(hwnd_)) return;
    try {
        if (!target_) {
            // In software: a few frames a second take little CPU, and it saves the RAM and startup time of a
            // Direct3D device, and any trouble with the graphics driver.
            RECT rc;
            GetClientRect(hwnd_, &rc);
            const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
            const auto props =
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(), dpi, dpi);
            winrt::check_hresult(d2d_->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), target_.put()));
        }
        target_->BeginDraw();
        DeviceInfo devices[ble::kRoleCount];
        for (size_t i = 0; i < ble::kRoleCount; ++i) devices[i] = Device(static_cast<ble::Role>(i));
        if (ride_) {
            renderer_->Draw(target_.get(), *ride_, Status(), confirmEnd_);
        } else if (done_) {
            SummaryView view;
            view.ride = done_.get();
            view.title = done_->finished() ? L"Ride complete" : L"Ride ended";
            view.date = doneDate_;
            if (donePath_.empty()) {
                view.note = L"Too short to save as a file";
            } else {
                view.note = L"Saved as a FIT file. Import it at t.coros.com, Garmin Connect or Strava.";
                view.file = L"Show file";
            }
            renderer_->DrawSummary(target_.get(), view);
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
                view.radio = Tone::Problem;
                break;
            case ble::Bluetooth::State::Off:
                view.bluetooth = L"Bluetooth is off";
                view.action = L"Turn on";
                view.radio = Tone::Problem;
                break;
            case ble::Bluetooth::State::On:
                view.bluetooth = L"Bluetooth is on and looking for devices";
                view.radio = Tone::Good;
                break;
            }
            wchar_t footer[96] = L"Your devices are remembered and connect by themselves next time.";
            if (autoContinue_ && continueAt_) {
                const ULONGLONG left = continueAt_ - std::min(continueAt_, GetTickCount64());
                swprintf_s(footer, L"All connected. Continuing in %llu s", (left + 999) / 1000);
            } else if (autoContinue_) {
                wcscpy_s(footer, L"Connecting to your devices");
            }
            view.footer = footer;
            renderer_->DrawDevices(target_.get(), view);
        } else {
            std::vector<SetupRow> rows;
            for (Field f : Fields()) rows.push_back({Label(f), Value(f)});
            SetupView view;
            view.rows = rows;
            view.selected = selected_;
            view.course = &course_;
            view.stats = stats_;
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
        logPath_ = dir + name;
        // The start in UTC, a second before this first row.
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER t{{ft.dwLowDateTime, ft.dwHighDateTime}};
        t.QuadPart -= 10'000'000;
        ft = {t.LowPart, t.HighPart};
        SYSTEMTIME utc;
        FileTimeToSystemTime(&ft, &utc);
        const CourseParams& c = ride_->course().params;
        std::fprintf(log_,
                     "# km=%.1f gain=%.0f max_grade=%.0f loop=%d endless=%d terrain=%d seed=%u "
                     "start=%04d-%02d-%02dT%02d:%02d:%02dZ\n",
                     c.km, ride_->course().targetGain, c.maxGradePct, c.loop ? 1 : 0, c.endless ? 1 : 0, c.terrain,
                     c.seed, utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
        std::fprintf(log_, "time_s,distance_m,elevation_m,grade_pct,power_w,cadence_rpm,speed_kmh,mode,gear,target,"
                           "heart_rate_bpm\n");
    }
    static const char* const kModes[] = {"sim", "erg"};
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
    if (closed_) return;
    closed_ = true;
    KillTimer(hwnd_, kTimer);
    StopRepeat();
    timer_ = false;
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
