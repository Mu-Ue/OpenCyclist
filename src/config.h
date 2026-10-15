#pragma once

constexpr wchar_t kAppName[] = L"OpenCyclist";

// Physics of the simulated outdoor bike.
constexpr double kGravity = 9.81;           // m/s²
constexpr double kAirDensity = 1.2;         // kg/m³
constexpr double kCdA = 0.32;               // m², on the hoods
constexpr double kCrr = 0.004;              // road tyres on asphalt
constexpr double kDrivetrainLoss = 0.03;    // of pedal power
constexpr double kWheelInertiaKg = 1.0;     // spinning wheels, as extra mass
constexpr double kWheelCircumference = 2.105;  // m, 700x25c
constexpr double kGrossEfficiency = 0.24;    // of food energy that reaches the pedals, for calories

// Virtual gears, spaced evenly in ratio between the lowest and highest, like Zwift's.
constexpr int kGearCount = 24;
constexpr double kLowestRatio = 0.75, kHighestRatio = 5.49;
constexpr int kStartGear = 10;  // 1-based

constexpr double kCourseStep = 10.0;  // m between elevation samples
constexpr double kMaxGradeChange = 0.002;  // per step, about 1 % per 50 m like real roads

constexpr unsigned kTickMs = 250;    // ride update and trainer control rate
constexpr int kMaxTargetW = 1500;
constexpr double kRerollBeforeM = 100.0;  // a running ride can be rerolled only this early
constexpr unsigned kHoldMs = 2000;          // controller buttons that act when held, such as A to reroll
