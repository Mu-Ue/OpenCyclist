#pragma once

#include <string>
#include <string_view>

// Turns a ride log, the CSV the app writes once a second, into a Garmin TCX activity that COROS, Garmin Connect,
// Strava and others import. Each row becomes one second of the ride from the start time in the log's first line.
// Returns an empty string when the log has no start time or no rows.
std::string RideToTcx(std::string_view csv);
