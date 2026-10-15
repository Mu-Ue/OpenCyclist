cmake_minimum_required(VERSION 3.21)
project(OpenCyclist LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributables
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # lets VS Code IntelliSense see the real flags

# Course generation, physics and the ride state, shared by the app and the test.
add_library(sim STATIC
    src/sim/course.cpp
    src/sim/physics.cpp
    src/sim/ride.cpp)
target_include_directories(sim PUBLIC src)
target_compile_definitions(sim PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(sim PUBLIC /W4 /permissive- /utf-8 /sdl /guard:cf /guard:ehcont)

add_executable(OpenCyclist WIN32
    src/main.cpp
    src/app.cpp
    src/ble/bluetooth.cpp
    src/ble/common.cpp
    src/ble/ftms.cpp
    src/ble/heart_rate.cpp
    src/ble/link.cpp
    src/ble/zwift_ride.cpp
    src/platform/settings.cpp
    src/ui/renderer.cpp
    src/ui/snapshot.cpp
    res/app.rc
    res/app.manifest)
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico)
target_compile_options(OpenCyclist PRIVATE /bigobj)
target_link_libraries(OpenCyclist PRIVATE sim windowsapp kernel32 user32 shell32 ole32 dwmapi d2d1 dwrite
                      windowscodecs delayimp)
# Image encoding loads only for a snapshot.
target_link_options(OpenCyclist PRIVATE /DELAYLOAD:windowscodecs.dll)
# Control-flow guard, shadow stack with checked exception continuations, and DLL imports from System32 only
# (no planted DLLs next to the exe).
target_link_options(OpenCyclist PRIVATE /GUARD:CF /GUARD:EHCONT /CETCOMPAT /DEPENDENTLOADFLAG:0x800)

add_executable(sim_test test/sim_test.cpp)
target_link_libraries(sim_test PRIVATE sim)
