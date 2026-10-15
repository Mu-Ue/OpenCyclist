#pragma once

#include <string>
#include <string_view>

// Turns a ride log, the CSV the app writes once a second, into a FIT activity, the file bike computers record and
// COROS, Garmin Connect, Strava and others import. It is a virtual ride with no GPS. Each row becomes one second of
// the ride from the start time in the log's first line. Returns an empty string when the log has no start time or
// no rows.
std::string RideToFit(std::string_view csv);
