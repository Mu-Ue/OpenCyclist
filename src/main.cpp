#include <windows.h>

#include <string>

#include <winrt/base.h>

#include "app.h"
#include "config.h"
#include "platform/settings.h"
#include "sim/course.h"
#include "sim/ride.h"
#include "ui/snapshot.h"

namespace {

// Loads DLLs only from System32 and never from network shares or low-integrity locations, so a DLL planted next
// to the exe or in the current folder is never picked up. Also fails fast on a bad handle, and loads no legacy
// extension DLLs such as AppInit DLLs.
void HardenProcess() {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY image{};
    image.NoRemoteImages = 1;
    image.NoLowMandatoryLabelImages = 1;
    image.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &image, sizeof(image));
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handles{};
    handles.RaiseExceptionOnInvalidHandleReference = 1;
    handles.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &handles, sizeof(handles));
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensions{};
    extensions.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &extensions, sizeof(extensions));
}

// Renders a ride without any hardware: the course from the arguments, ridden at a steady 230 W to 40 % of it.
// For working on the ride view.
bool Snapshot(const Settings& settings) {
    CourseParams params = settings.course;
    if (!params.seed) params.seed = RandomSeed();
    Ride ride(GenerateCourse(params), settings.mode, settings.riderKg + settings.bikeKg, settings.ergW,
              settings.resistancePct);
    ride.OnPower(230);
    ride.OnCadence(88);
    while (ride.distance() < ride.course().Length() * 0.4 && !ride.finished()) ride.Tick(kTickMs / 1000.0);
    return SaveSnapshot(ride, L"", settings.snapshot);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HardenProcess();
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Settings settings = LoadSettings();
    std::wstring error;
    if (!ApplyArgs(settings, GetCommandLineW(), error)) {
        MessageBoxW(nullptr, error.c_str(), kAppName, MB_ICONWARNING);
        return 1;
    }
    if (!settings.snapshot.empty()) return Snapshot(settings) ? 0 : 1;

    // The trainer takes one controlling app at a time, so run only once.
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, L"OpenCyclist.Running");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const HWND window = FindWindowW(kAppName, nullptr)) {
            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
        return 0;
    }
    SaveSettings(settings);  // arguments stick for the next start, except the seed, which isn't saved
    return RunApp(instance, std::move(settings));
}
