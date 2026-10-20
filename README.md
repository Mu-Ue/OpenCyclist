<div align="center">
  <img src="docs/icon.png" width="64" alt="">
  <h1>OpenCyclist</h1>
  <p>A minimal indoor cycling app for Windows that works with Bluetooth smart trainers.</p>
  <img src="docs/ride.png" alt="A ride in progress">
</div>

## Download

Get `OpenCyclist.exe` from the latest release and run it. It needs Windows 10 or newer with Bluetooth.

## Usage

Pick a course, click New course until you like it, then Start ride. Sim mode uses virtual gears. ERG mode holds a target power.

Every ride is saved as a FIT file in `%APPDATA%\OpenCyclist\rides`. Import it into COROS, Garmin Connect or Strava.

| Key | Controller | Action |
| --- | --- | --- |
| ↑ → | Right shifters | Harder: gear up or +5 W |
| ↓ ← | Left shifters | Easier |
| Page Up/Down | D-pad up/down | Big steps |
| | Y | Next mode |
| Space | Z | Pause |
| | Hold A | New course, in the first 100 m |
| | B | Devices, from the setup screen |
| Esc | B | End the ride, after a yes |
| | Y | Show the ride file, on the summary |
| H | Z | History, from the setup screen |
| Delete | Z | Delete a ride, on the history screen, after a yes |
| | Y | Show the ride file, on the history screen |
| F11 | | Full screen |

During a ride, drag a card or graph onto another to swap them. The arrangement is remembered.

## Build from source

Install Visual Studio 2022 with the C++ workload and the Windows 11 SDK, then run `build.bat`.
