#include "sim/fit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "config.h"

namespace {

// "2026-10-15T15:04:05Z" as seconds since 1970, or -1.
long long ParseTime(std::string_view s) {
    int y, mo, d, h, mi, se;
    if (s.size() < 20 || sscanf_s(std::string(s.substr(0, 20)).c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h,
                                     &mi, &se) != 6)
        return -1;
    const std::chrono::year_month_day day{std::chrono::year(y), std::chrono::month(mo), std::chrono::day(d)};
    if (!day.ok() || h > 23 || mi > 59 || se > 60) return -1;
    return std::chrono::sys_days(day).time_since_epoch() / std::chrono::seconds(1) + h * 3600LL + mi * 60LL + se;
}

// FIT times count from 1989-12-31 instead of 1970.
constexpr long long kFitEpoch = 631065600;

// FIT base types, and the global message numbers and enum values from the FIT profile.
enum : uint8_t { kEnum = 0x00, kU8 = 0x02, kU16 = 0x84, kU32 = 0x86 };
enum : uint16_t { kFileId = 0, kSession = 18, kLap = 19, kRecord = 20, kEvent = 21, kActivity = 34 };
enum : uint8_t { kTimer = 0, kSessionEvent = 8, kLapEvent = 9, kActivityEvent = 26 };
enum : uint8_t { kStart = 0, kStop = 1, kStopAll = 4 };
enum : uint8_t { kCycling = 2, kVirtualActivity = 58 };
constexpr uint32_t kNoU8 = 0xFF;  // FIT's "no value"

struct Field {
    uint8_t number, type;
};

// Little-endian FIT messages. A definition says which fields a local message type has, and the data messages
// after it give their values in the same order.
struct Writer {
    std::string data;
    void U8(uint32_t v) { data += static_cast<char>(v & 0xFF); }
    void U16(uint32_t v) { U8(v), U8(v >> 8); }
    void U32(uint32_t v) { U16(v), U16(v >> 16); }
    void Define(uint8_t local, uint16_t global, std::initializer_list<Field> fields) {
        U8(0x40u | local), U8(0), U8(0), U16(global), U8(static_cast<uint32_t>(fields.size()));
        for (const Field& f : fields) U8(f.number), U8(f.type == kU32 ? 4 : f.type == kU16 ? 2 : 1), U8(f.type);
    }
};

uint32_t Scaled(double v, double scale) { return static_cast<uint32_t>(std::lround(std::max(v, 0.0) * scale)); }

// FIT's CRC-16.
uint16_t Crc(std::string_view bytes) {
    uint16_t crc = 0;
    for (const unsigned char b : bytes) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) crc = static_cast<uint16_t>(crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1);
    }
    return crc;
}

// A number after " key=" on the line, or `fallback` when it isn't there.
double Setting(std::string_view line, std::string_view key, double fallback) {
    const size_t at = line.find(key);
    if (at == std::string_view::npos) return fallback;
    double v = fallback;
    sscanf_s(std::string(line.substr(at + key.size(), 24)).c_str(), "%lf", &v);
    return v;
}

// A ride log's start time, course and rows. False when it has no start time.
bool ParseLog(std::string_view csv, long long& start, CourseParams& course, std::vector<LogSample>& samples) {
    // The first line holds the settings and start time, the second the column names.
    const size_t at = csv.find("start=");
    const size_t firstLine = csv.find('\n');
    if (at == std::string_view::npos || firstLine == std::string_view::npos || at > firstLine) return false;
    start = ParseTime(csv.substr(at + 6));
    const size_t header = csv.find('\n', firstLine + 1);
    if (start < kFitEpoch || header == std::string_view::npos) return false;

    // Older logs lack the later settings.
    const std::string_view first = csv.substr(0, firstLine);
    course.km = Setting(first, " km=", course.km);
    course.gainM = Setting(first, " gain=", course.gainM);
    course.maxGradePct = Setting(first, " max_grade=", course.maxGradePct);
    course.loop = Setting(first, " loop=", 0) != 0;
    course.endless = Setting(first, " endless=", 0) != 0;
    course.terrain = std::clamp(static_cast<int>(Setting(first, " terrain=", course.terrain)), 0, kTerrainCount - 1);
    course.seed = static_cast<uint32_t>(Setting(first, " seed=", 0));

    // time_s,distance_m,elevation_m,grade_pct,power_w,cadence_rpm,speed_kmh,mode,gear,target,heart_rate_bpm
    samples.clear();
    samples.reserve(std::count(csv.begin(), csv.end(), '\n'));
    for (size_t line = header + 1; line < csv.size();) {
        size_t end = csv.find('\n', line);
        if (end == std::string_view::npos) end = csv.size();
        const std::string row(csv.substr(line, end - line));
        line = end + 1;
        double time, distance, elevation, grade, cadence, speed;
        int power, heartRate;
        char mode[16];
        int gear, target;
        if (sscanf_s(row.c_str(), "%lf,%lf,%lf,%lf,%d,%lf,%lf,%15[^,],%d,%d,%d", &time, &distance, &elevation, &grade,
                        &power, &cadence, &speed, mode, static_cast<unsigned>(sizeof(mode)), &gear, &target, &heartRate) == 11)
            samples.push_back({distance, elevation, speed, std::clamp(power, 0, 65534), std::clamp(static_cast<int>(cadence), 0, 254),
                               std::clamp(heartRate, 0, 254)});
    }
    return true;
}

}  // namespace

bool ReadLog(std::string_view csv, CourseParams& course, std::vector<LogSample>& samples) {
    long long start = 0;
    return ParseLog(csv, start, course, samples);
}

LogTotals SumLog(std::span<const LogSample> samples) {
    LogTotals t;
    if (samples.empty()) return t;
    double work = 0;
    long long cadenceSum = 0, heartSum = 0;
    int cadenceCount = 0, heartCount = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const LogSample& s = samples[i];
        work += s.power;  // one second each
        t.maxPower = std::max(t.maxPower, s.power);
        if (s.cadence > 0) cadenceSum += s.cadence, ++cadenceCount;
        if (s.heartRate > 0) heartSum += s.heartRate, ++heartCount, t.maxHeartRate = std::max(t.maxHeartRate, s.heartRate);
        if (i) t.climbed += std::max(0.0, s.elevation - samples[i - 1].elevation);
    }
    t.seconds = static_cast<int>(samples.size());
    t.distance = samples.back().distance;
    t.averagePower = static_cast<int>(std::lround(work / samples.size()));
    t.averageCadence = cadenceCount ? static_cast<int>((cadenceSum + cadenceCount / 2) / cadenceCount) : 0;
    t.averageHeartRate = heartCount ? static_cast<int>((heartSum + heartCount / 2) / heartCount) : 0;
    t.calories = work / 4184.0 / kGrossEfficiency;
    return t;
}

Course LoggedCourse(const CourseParams& params, std::span<const LogSample> samples) {
    const double ridden = samples.empty() ? 0.0 : samples.back().distance;
    Course course = GenerateCourse(params);
    while (params.endless && course.Length() - ridden < kEndlessAheadM) course.Extend();  // as the ride grew it
    // Growing an endless road can lift all of it, so the heights are compared by how they change from second to
    // second. A few seconds may differ where the road was lifted.
    size_t misses = 0;
    for (size_t i = 1; i < samples.size(); ++i) {
        const double logged = samples[i].elevation - samples[i - 1].elevation;
        const double made = course.ElevationAt(samples[i].distance) - course.ElevationAt(samples[i - 1].distance);
        if (std::abs(logged - made) > 0.3) ++misses;  // the log rounds heights to 0.1 m
    }
    if (course.Length() + kCourseStep >= ridden && misses <= 3 + samples.size() / 100) return course;

    Course road;
    road.params = params;
    road.params.endless = true;
    road.elevation.resize(static_cast<size_t>(ridden / kCourseStep) + 2);
    for (size_t i = 0, j = 0; i < road.elevation.size(); ++i) {
        const double d = static_cast<double>(i) * kCourseStep;
        while (j + 1 < samples.size() && samples[j + 1].distance < d) ++j;
        const LogSample a = samples.empty() ? LogSample{} : samples[j];
        const LogSample b = j + 1 < samples.size() ? samples[j + 1] : a;
        const double f = b.distance > a.distance ? std::clamp((d - a.distance) / (b.distance - a.distance), 0.0, 1.0) : 0.0;
        road.elevation[i] = static_cast<float>(a.elevation + (b.elevation - a.elevation) * f);
    }
    return road;
}

std::string RideToFit(std::string_view csv) {
    long long start = 0;
    CourseParams course;
    std::vector<LogSample> samples;
    if (!ParseLog(csv, start, course, samples) || samples.empty()) return {};

    double work = 0, maxSpeed = 0, ascent = 0, descent = 0;
    long long heartSum = 0, cadenceSum = 0;
    int heartCount = 0, maxHeart = 0, cadenceCount = 0, maxCadence = 0, maxPower = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const LogSample& s = samples[i];
        work += s.power;  // one second each
        maxPower = std::max(maxPower, s.power);
        maxSpeed = std::max(maxSpeed, s.speedKmh / 3.6);
        if (s.heartRate > 0) heartSum += s.heartRate, ++heartCount, maxHeart = std::max(maxHeart, s.heartRate);
        if (s.cadence > 0) cadenceSum += s.cadence, ++cadenceCount, maxCadence = std::max(maxCadence, s.cadence);
        if (i) (s.elevation > samples[i - 1].elevation ? ascent : descent) += std::abs(s.elevation - samples[i - 1].elevation);
    }
    const auto n = static_cast<uint32_t>(samples.size());
    const auto t0 = static_cast<uint32_t>(start - kFitEpoch), t1 = t0 + n;
    const double distance = samples.back().distance;

    Writer w;
    w.Define(0, kFileId, {{0, kEnum}, {1, kU16}, {2, kU16}, {4, kU32}});
    w.U8(0), w.U8(4 /* activity */), w.U16(255 /* development */), w.U16(0), w.U32(t0);

    w.Define(1, kEvent, {{253, kU32}, {0, kEnum}, {1, kEnum}});
    w.U8(1), w.U32(t0), w.U8(kTimer), w.U8(kStart);

    w.Define(2, kRecord, {{253, kU32}, {5, kU32}, {2, kU16}, {6, kU16}, {7, kU16}, {4, kU8}, {3, kU8}});
    for (uint32_t i = 0; i < n; ++i) {
        const LogSample& s = samples[i];
        w.U8(2), w.U32(t0 + i + 1), w.U32(Scaled(s.distance, 100)), w.U16(Scaled(s.elevation + 500, 5)),
            w.U16(Scaled(s.speedKmh / 3.6, 1000)), w.U16(static_cast<uint32_t>(s.power)),
            w.U8(static_cast<uint32_t>(s.cadence)), w.U8(s.heartRate ? static_cast<uint32_t>(s.heartRate) : kNoU8);
    }

    w.U8(1), w.U32(t1), w.U8(kTimer), w.U8(kStopAll);

    // The lap and the session are the whole ride, with the same totals in the same order.
    auto totals = [&] {
        w.U32(n * 1000), w.U32(n * 1000), w.U32(Scaled(distance, 100)),
            w.U16(Scaled(work / 4184.0 / kGrossEfficiency, 1)), w.U16(Scaled(distance / n, 1000)),
            w.U16(Scaled(maxSpeed, 1000));
        w.U8(heartCount ? static_cast<uint32_t>((heartSum + heartCount / 2) / heartCount) : kNoU8);
        w.U8(heartCount ? static_cast<uint32_t>(maxHeart) : kNoU8);
        w.U8(cadenceCount ? static_cast<uint32_t>((cadenceSum + cadenceCount / 2) / cadenceCount) : kNoU8);
        w.U8(cadenceCount ? static_cast<uint32_t>(maxCadence) : kNoU8);
        w.U16(Scaled(work / n, 1)), w.U16(static_cast<uint32_t>(maxPower)), w.U16(Scaled(ascent, 1)),
            w.U16(Scaled(descent, 1)), w.U8(kCycling), w.U8(kVirtualActivity);
    };
    w.Define(3, kLap, {{253, kU32}, {0, kEnum}, {1, kEnum}, {2, kU32}, {7, kU32}, {8, kU32}, {9, kU32}, {11, kU16},
                       {13, kU16}, {14, kU16}, {15, kU8}, {16, kU8}, {17, kU8}, {18, kU8}, {19, kU16}, {20, kU16},
                       {21, kU16}, {22, kU16}, {25, kEnum}, {39, kEnum}});
    w.U8(3), w.U32(t1), w.U8(kLapEvent), w.U8(kStop), w.U32(t0);
    totals();
    w.Define(4, kSession, {{253, kU32}, {0, kEnum}, {1, kEnum}, {2, kU32}, {7, kU32}, {8, kU32}, {9, kU32}, {11, kU16},
                           {14, kU16}, {15, kU16}, {16, kU8}, {17, kU8}, {18, kU8}, {19, kU8}, {20, kU16}, {21, kU16},
                           {22, kU16}, {23, kU16}, {5, kEnum}, {6, kEnum}, {25, kU16}, {26, kU16}});
    w.U8(4), w.U32(t1), w.U8(kSessionEvent), w.U8(kStop), w.U32(t0);
    totals();
    w.U16(0), w.U16(1);  // first lap, number of laps

    w.Define(5, kActivity, {{253, kU32}, {0, kU32}, {1, kU16}, {2, kEnum}, {3, kEnum}, {4, kEnum}});
    w.U8(5), w.U32(t1), w.U32(n * 1000), w.U16(1), w.U8(0 /* manual */), w.U8(kActivityEvent), w.U8(kStop);

    // A 14-byte header, the messages, then a CRC over both.
    Writer file;
    file.U8(14), file.U8(0x10 /* protocol 1.0 */), file.U16(2100 /* profile 21.00 */);
    file.U32(static_cast<uint32_t>(w.data.size()));
    file.data += ".FIT";
    file.U16(Crc(file.data));
    file.data += w.data;
    file.U16(Crc(file.data));
    return file.data;
}
