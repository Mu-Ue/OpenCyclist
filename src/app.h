#pragma once

#include <windows.h>

#include "platform/settings.h"

// Shows the setup screen and the ride view until the window closes.
int RunApp(HINSTANCE instance, Settings settings);
