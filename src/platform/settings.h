#pragma once

#include <cstdint>
#include <string>

#include "sim/course.h"
#include "sim/ride.h"

// A remembered Bluetooth device. Address 0 when there is none.
struct SavedDevice {
    uint64_t address = 0;
    std::wstring name;
};

struct Settings {
    CourseParams course;
    Mode mode = Mode::Sim;
    double riderKg = 75.0;
    int ergW = 150;
    SavedDevice devices[3];  // trainer, shifters and heart rate monitor, in ble::Role order
    std::wstring board;      // how the ride screen's cards and graphs are arranged, as Board::Format writes it
    std::wstring snapshot;  // --snapshot: render a ride to this PNG and quit. Not saved.
};

// %APPDATA%\OpenCyclist and its rides folder, created if missing.
std::wstring AppDataDir();
std::wstring RidesDir();

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Applies --km, --gain and the rest. Returns false with a message on a bad argument.
bool ApplyArgs(Settings& settings, const wchar_t* commandLine, std::wstring& error);
