<div align="center">
  <img src="docs/icon.png" width="64" alt="">
  <h1>OpenCyclist</h1>
  <p>A minimal indoor cycling app for Windows, built for the Zwift Ride.</p>
  <img src="docs/ride.png" alt="A ride in progress">
</div>

## Download

Get `OpenCyclist.exe` from the latest release and run it. It needs Windows 10 or newer with Bluetooth.

## Usage

Close Zwift and Zwift Companion first. The trainer takes one app at a time.

Pick a distance, elevation gain and max grade. The course appears on the right. Press R until you like it, then Enter to ride.

The trainer and the Zwift Ride shifters connect on their own. Distance comes from your real power, so climbs take as long as they would outside.

There are three modes. Sim uses virtual gears and makes climbs harder. ERG holds a target power. Resistance holds a fixed level.

| Key | Zwift Ride | Action |
| --- | --- | --- |
| ↑ → | Right shifters | Harder: gear up, +5 W or +5 % |
| ↓ ← | Left shifters | Easier |
| Shift, Page Up/Down | D-pad up/down | Big steps |
| M | Y | Next mode |
| Space | Z | Pause |
| R | Hold A | New course, in the first 100 m |
| Esc twice | Hold B | End the ride |
| F11 | | Full screen |

On the setup screen, the D-pad picks a setting, the shifters change it and A starts.

Every ride is saved as a CSV in `%APPDATA%\OpenCyclist\rides`.

Options such as `--km 30 --gain 1000 --max-grade 10 --loop --mode erg --erg 200 --seed 12345` set up the course from the command line.

## Build from source

Install Visual Studio 2022 with the C++ workload and the Windows 11 SDK, then run `build.bat`.
