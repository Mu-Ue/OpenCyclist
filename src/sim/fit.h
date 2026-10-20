#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sim/course.h"

// Turns a ride log, the CSV the app writes once a second, into a FIT activity, the file bike computers record and
// COROS, Garmin Connect, Strava and others import. It is a virtual ride with no GPS. Each row becomes one second of
// the ride from the start time in the log's first line. Returns an empty string when the log has no start time or
// no rows.
std::string RideToFit(std::string_view csv);

// One second of a ride log.
struct LogSample {
    double distance, elevation, speedKmh;
    int power, cadence, heartRate;  // heart rate 0 without one
};

// A ride log's course settings and rows, for looking back at a ride. False when the log has no start time.
bool ReadLog(std::string_view csv, CourseParams& course, std::vector<LogSample>& samples);

// A ride's totals. All zero without rows, and the heart rate and cadence 0 when there were none.
struct LogTotals {
    int seconds = 0;
    double distance = 0, climbed = 0;  // m
    double calories = 0;                // kcal
    int averagePower = 0, maxPower = 0, averageCadence = 0, averageHeartRate = 0, maxHeartRate = 0;
};
LogTotals SumLog(std::span<const LogSample> samples);

// The course a log was ridden on, made again from its settings and grown as far as the ride went. When that doesn't
// match the log, such as for a ride from before the courses changed, it is the road ridden traced from the log's own
// heights, marked endless since nothing beyond it is known.
Course LoggedCourse(const CourseParams& params, std::span<const LogSample> samples);
