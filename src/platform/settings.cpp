#include "platform/settings.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>

#include "config.h"

namespace {

constexpr wchar_t kSection[] = L"ride";

const wchar_t* ModeName(Mode mode) {
    return mode == Mode::Erg ? L"erg" : L"sim";
}

bool ParseMode(const wchar_t* s, Mode& mode) {
    if (!_wcsicmp(s, L"sim")) mode = Mode::Sim;
    else if (!_wcsicmp(s, L"erg")) mode = Mode::Erg;
    else return false;
    return true;
}

// A number within [lo, hi], with nothing after it.
bool ParseNumber(const wchar_t* s, double lo, double hi, double& out) {
    wchar_t* end = nullptr;
    const double v = std::wcstod(s, &end);
    if (end == s || *end || !(v >= lo && v <= hi)) return false;
    out = v;
    return true;
}

// The address and name keys of each saved device.
constexpr const wchar_t* kDeviceKeys[][2] = {
    {L"trainer", L"trainer_name"}, {L"shifters", L"shifters_name"}, {L"heart_rate", L"heart_rate_name"}};

std::wstring IniPath() { return AppDataDir() + L"\\settings.ini"; }

double ReadNumber(const std::wstring& ini, const wchar_t* key, double lo, double hi, double fallback) {
    wchar_t buf[64];
    GetPrivateProfileStringW(kSection, key, L"", buf, 64, ini.c_str());
    double v;
    return ParseNumber(buf, lo, hi, v) ? v : fallback;
}

// Adds "key=value" to a section being built, each ended by a null.
void Put(std::wstring& section, const wchar_t* key, std::wstring_view value) {
    section.append(key).append(1, L'=').append(value).append(1, L'\0');
}

void PutNumber(std::wstring& section, const wchar_t* key, double value) {
    wchar_t buf[64];
    swprintf_s(buf, L"%g", value);
    Put(section, key, buf);
}

}  // namespace

std::wstring AppDataDir() {
    static const std::wstring dir = [] {
        PWSTR base = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &base))) path = base;
        CoTaskMemFree(base);
        return path + L"\\OpenCyclist";
    }();
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring RidesDir() {
    const std::wstring dir = AppDataDir() + L"\\rides";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

Settings LoadSettings() {
    const std::wstring ini = IniPath();
    Settings s;
    s.course.km = ReadNumber(ini, L"km", 1, 500, s.course.km);
    s.course.gainM = ReadNumber(ini, L"gain", 0, 10000, s.course.gainM);
    s.course.maxGradePct = ReadNumber(ini, L"max_grade", 2, 25, s.course.maxGradePct);
    s.course.loop = ReadNumber(ini, L"loop", 0, 1, 0) != 0;
    s.course.endless = ReadNumber(ini, L"endless", 0, 1, 0) != 0;
    s.course.terrain = static_cast<int>(ReadNumber(ini, L"terrain", 0, kTerrainCount - 1, s.course.terrain));
    s.riderKg = ReadNumber(ini, L"rider_kg", 30, 200, s.riderKg);
    s.ergW = static_cast<int>(ReadNumber(ini, L"erg_w", 0, kMaxErgW, s.ergW));
    wchar_t buf[64];
    GetPrivateProfileStringW(kSection, L"mode", L"", buf, 64, ini.c_str());
    ParseMode(buf, s.mode);
    GetPrivateProfileStringW(kSection, L"board", L"", buf, 64, ini.c_str());
    s.board = buf;
    for (size_t i = 0; i < std::size(kDeviceKeys); ++i) {
        GetPrivateProfileStringW(kSection, kDeviceKeys[i][0], L"", buf, 64, ini.c_str());
        s.devices[i].address = _wcstoui64(buf, nullptr, 16);
        wchar_t name[64];
        GetPrivateProfileStringW(kSection, kDeviceKeys[i][1], L"", name, 64, ini.c_str());
        if (s.devices[i].address) s.devices[i].name = name;
    }
    return s;
}

// The whole section in one write, rather than rewriting the file once for each key.
void SaveSettings(const Settings& s) {
    std::wstring section;
    PutNumber(section, L"km", s.course.km);
    PutNumber(section, L"gain", s.course.gainM);
    PutNumber(section, L"max_grade", s.course.maxGradePct);
    PutNumber(section, L"loop", s.course.loop ? 1 : 0);
    PutNumber(section, L"endless", s.course.endless ? 1 : 0);
    PutNumber(section, L"terrain", s.course.terrain);
    PutNumber(section, L"rider_kg", s.riderKg);
    PutNumber(section, L"erg_w", s.ergW);
    Put(section, L"mode", ModeName(s.mode));
    if (!s.board.empty()) Put(section, L"board", s.board);
    for (size_t i = 0; i < std::size(kDeviceKeys); ++i) {
        wchar_t buf[32];
        swprintf_s(buf, L"%llX", static_cast<unsigned long long>(s.devices[i].address));
        Put(section, kDeviceKeys[i][0], buf);
        Put(section, kDeviceKeys[i][1], s.devices[i].name);
    }
    section += L'\0';  // the list ends with an empty line
    WritePrivateProfileSectionW(kSection, section.c_str(), IniPath().c_str());
}

bool ApplyArgs(Settings& s, const wchar_t* commandLine, std::wstring& error) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(commandLine, &argc);
    if (!argv) return true;
    bool ok = true;
    for (int i = 1; i < argc && ok; ++i) {
        const wchar_t* key = argv[i];
        if (!wcscmp(key, L"--loop") || !wcscmp(key, L"--no-loop") || !wcscmp(key, L"--endless")) {
            s.course.loop = !wcscmp(key, L"--loop");
            s.course.endless = !wcscmp(key, L"--endless");
            continue;
        }
        const wchar_t* value = i + 1 < argc ? argv[++i] : L"";
        double v = 0;
        if (!wcscmp(key, L"--snapshot")) ok = *value != 0, s.snapshot = value;
        else if (!wcscmp(key, L"--km")) ok = ParseNumber(value, 1, 500, s.course.km);
        else if (!wcscmp(key, L"--gain")) ok = ParseNumber(value, 0, 10000, s.course.gainM);
        else if (!wcscmp(key, L"--max-grade")) ok = ParseNumber(value, 2, 25, s.course.maxGradePct);
        else if (!wcscmp(key, L"--terrain")) ok = ParseNumber(value, 0, kTerrainCount - 1, v), s.course.terrain = static_cast<int>(v);
        else if (!wcscmp(key, L"--weight")) ok = ParseNumber(value, 30, 200, s.riderKg);
        else if (!wcscmp(key, L"--mode")) ok = ParseMode(value, s.mode);
        else if (!wcscmp(key, L"--erg")) ok = ParseNumber(value, 0, kMaxErgW, v), s.ergW = static_cast<int>(v);
        else if (!wcscmp(key, L"--seed")) ok = ParseNumber(value, 1, 4294967295.0, v), s.course.seed = static_cast<uint32_t>(v);
        else ok = false;
        if (!ok) error = L"Bad argument: " + std::wstring(key) + L" " + value + L"\n\n";
    }
    LocalFree(argv);
    if (!ok)
        error +=
            L"Usage: OpenCyclist.exe [--km 30] [--gain 1000] [--max-grade 10] [--loop | --no-loop | --endless]\n"
            L"[--terrain 0-3] [--mode sim|erg] [--erg 200] [--weight 75] [--seed 12345] [--snapshot file.png]";
    return ok;
}
