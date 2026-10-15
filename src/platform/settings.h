#pragma once

#include <cstdint>
#include <string>

#include "sim/course.h"
#include "sim/ride.h"

struct Settings {
    CourseParams course;
    Mode mode = Mode::Sim;
    double riderKg = 75.0, bikeKg = 9.0;
    int ergW = 150;
    int resistancePct = 30;
    uint64_t trainer = 0;  // Bluetooth address of the last trainer
    std::wstring snapshot;  // --snapshot: render a ride to this PNG and quit. Not saved.
};

// %APPDATA%\OpenCyclist and its rides folder, created if missing.
std::wstring AppDataDir();
std::wstring RidesDir();

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Applies --km, --gain and the rest. Returns false with a message on a bad argument.
bool ApplyArgs(Settings& settings, const wchar_t* commandLine, std::wstring& error);
