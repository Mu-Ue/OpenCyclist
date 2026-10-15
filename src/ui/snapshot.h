#pragma once

#include <string>
#include <string_view>

#include "sim/ride.h"

// Renders the ride view offscreen, in software so it works on any PC, and saves it as a PNG.
bool SaveSnapshot(const Ride& ride, std::wstring_view status, const std::wstring& path, unsigned width = 1280,
                  unsigned height = 720);
