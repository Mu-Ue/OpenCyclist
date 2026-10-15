#include "sim/tcx.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "config.h"

namespace {

struct Sample {
    double distance, elevation, speedKmh;
    int power, cadence, heartRate;
};

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

void AppendTime(std::string& out, long long t) {
    const auto days = std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds(std::chrono::seconds(t)));
    const std::chrono::year_month_day day(days);
    const long long s = t - days.time_since_epoch() / std::chrono::seconds(1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ", int(day.year()), unsigned(day.month()),
                  unsigned(day.day()), s / 3600, s / 60 % 60, s % 60);
    out += buf;
}

}  // namespace

std::string RideToTcx(std::string_view csv) {
    // The first line holds the settings and start time, the second the column names.
    const size_t at = csv.find("start=");
    const size_t firstLine = csv.find('\n');
    if (at == std::string_view::npos || firstLine == std::string_view::npos || at > firstLine) return {};
    const long long start = ParseTime(csv.substr(at + 6));
    const size_t header = csv.find('\n', firstLine + 1);
    if (start < 0 || header == std::string_view::npos) return {};

    // time_s,distance_m,elevation_m,grade_pct,power_w,cadence_rpm,speed_kmh,mode,gear,target,heart_rate_bpm
    std::vector<Sample> samples;
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
            samples.push_back({distance, elevation, speed, std::max(power, 0), static_cast<int>(cadence), heartRate});
    }
    if (samples.empty()) return {};

    double work = 0, maxSpeed = 0;
    long long heartSum = 0;
    int heartCount = 0, maxHeart = 0;
    for (const Sample& s : samples) {
        work += s.power;  // one second each
        maxSpeed = std::max(maxSpeed, s.speedKmh / 3.6);
        if (s.heartRate > 0) heartSum += s.heartRate, ++heartCount, maxHeart = std::max(maxHeart, s.heartRate);
    }

    std::string out;
    out.reserve(samples.size() * 300 + 1024);
    char buf[256];
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<TrainingCenterDatabase xmlns=\"http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2\" "
           "xmlns:ns3=\"http://www.garmin.com/xmlschemas/ActivityExtension/v2\">\n"
           "<Activities><Activity Sport=\"Biking\"><Id>";
    AppendTime(out, start);
    out += "</Id><Lap StartTime=\"";
    AppendTime(out, start);
    std::snprintf(buf, sizeof(buf),
                  "\"><TotalTimeSeconds>%zu</TotalTimeSeconds><DistanceMeters>%.1f</DistanceMeters>"
                  "<MaximumSpeed>%.2f</MaximumSpeed><Calories>%.0f</Calories>",
                  samples.size(), samples.back().distance, maxSpeed, work / 4184.0 / kGrossEfficiency);
    out += buf;
    if (heartCount) {
        std::snprintf(buf, sizeof(buf),
                      "<AverageHeartRateBpm><Value>%lld</Value></AverageHeartRateBpm>"
                      "<MaximumHeartRateBpm><Value>%d</Value></MaximumHeartRateBpm>",
                      (heartSum + heartCount / 2) / heartCount, maxHeart);
        out += buf;
    }
    out += "<Intensity>Active</Intensity><TriggerMethod>Manual</TriggerMethod><Track>\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        out += "<Trackpoint><Time>";
        AppendTime(out, start + static_cast<long long>(i) + 1);
        std::snprintf(buf, sizeof(buf), "</Time><AltitudeMeters>%.1f</AltitudeMeters><DistanceMeters>%.1f</DistanceMeters>",
                      s.elevation, s.distance);
        out += buf;
        if (s.heartRate > 0) {
            std::snprintf(buf, sizeof(buf), "<HeartRateBpm><Value>%d</Value></HeartRateBpm>", std::min(s.heartRate, 255));
            out += buf;
        }
        std::snprintf(buf, sizeof(buf),
                      "<Cadence>%d</Cadence><Extensions><ns3:TPX><ns3:Speed>%.2f</ns3:Speed><ns3:Watts>%d</ns3:Watts>"
                      "</ns3:TPX></Extensions></Trackpoint>\n",
                      std::clamp(s.cadence, 0, 254), s.speedKmh / 3.6, s.power);
        out += buf;
    }
    out += "</Track></Lap><Creator xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"Device_t\">"
           "<Name>OpenCyclist</Name><UnitId>0</UnitId><ProductID>0</ProductID>"
           "<Version><VersionMajor>1</VersionMajor><VersionMinor>0</VersionMinor></Version></Creator>"
           "</Activity></Activities></TrainingCenterDatabase>\n";
    return out;
}
